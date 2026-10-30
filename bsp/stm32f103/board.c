/*
 * board.c - 板级初始化（STM32F103，战舰 V4）
 *
 * 职责（只做板级「配置 + 编排」，不含任何外设驱动实现）：
 *   1) 按 RCC_CFGR 实际位配置推算 SYSCLK / HCLK / PCLK2
 *   2) 配置 SysTick 产生 1ms 系统时钟节拍
 *   3) 初始化 RT-Thread 动态内存堆
 *   4) 编排各驱动初始化顺序，并把板级事实（波特率、总线时钟）注入给驱动
 *
 * 串口实现见 drivers/uart/stm32f1_uart.c：本文件只负责把 PCLK2 传进去，
 * 驱动内部不写死主频，换 HSE/PLL 也不用改驱动。
 *
 * 时钟方案：使用内部高速时钟 HSI（8MHz）作为系统时钟，不做 PLL 超频，
 * 降低对外部晶振的依赖，便于在多种 F103 板卡上跑通。
 */
#include <rtthread.h>

#include "board.h"
#include "uart.h"
#include "stm32f1_i2c.h"
#include "at24c02.h"
#include "at24c02_config.h"
#include "eeprom.h"
#include "eeprom_storage.h"
#include "spi.h"
#include "spi_config.h"
#include "stm32f1_spi.h"
#include "w25q128.h"
#include "w25q128_config.h"
#include "sd.h"
#include "stm32f1_sdio.h"

/* ================= 板级事实 ================= */
#define UART_BAUD       115200u
#define BOARD_I2C_BUS_NR 0u     /* stm32f1_i2c_gpio_init() 固定把这条总线注册成 0 */
#define RCC_BASE        0x40021000u
#define RCC_CFGR        (*(volatile unsigned long *)(RCC_BASE + 0x04u))

#define SYSTICK_BASE    0xE000E010u
#define SYSTICK_CTRL    (*(volatile unsigned long *)(SYSTICK_BASE + 0x00u))
#define SYSTICK_LOAD    (*(volatile unsigned long *)(SYSTICK_BASE + 0x04u))
#define SYSTICK_VAL     (*(volatile unsigned long *)(SYSTICK_BASE + 0x08u))
#define SYSTICK_CFLAG_CLKSOURCE (0x1u << 2)
#define SYSTICK_CFLAG_TICKINT   (0x1u << 1)
#define SYSTICK_CFLAG_ENABLE    (0x1u << 0)

/* ================= RCC 时钟估算（防乱码：UART/SysTick 与实际时钟解耦） =================
 * 始终按 STM32F1 RCC_CFGR 实际位配置推算系统时钟/总线时钟：
 *   - UART1(BRR)  用 PCLK2(APB2)，保证波特率在任何时钟方案下都精确 = 115200
 *   - SysTick     CLKSOURCE=1 时时钟 = HCLK(AHB)，保证 1ms 节拍精确
 * 头注释默认"HSI 8MHz"，补丁后即使换成 HSE/PLL(72MHz) 也不会出现串口乱码/节拍偏快。 */
#define HSI_CLOCK_HZ    8000000u   /* 内部高速时钟 */
#define HSE_CLOCK_HZ    8000000u   /* 战舰V4 外部晶振 */

static unsigned long rcc_sysclk_hz(void)
{
    unsigned long cfgr = RCC_CFGR;
    unsigned long src  = (cfgr >> 2) & 0x3u;             /* SW: 0=HSI 1=HSE 2=PLL */
    unsigned long clk;

    if (src == 1u)
    {
        clk = HSE_CLOCK_HZ;
    }
    else if (src == 2u)
    {
        unsigned long pllsrc = (cfgr >> 16) & 0x1u;      /* PLLSRC: 0=HSI/2 1=HSE */
        unsigned long pllxt  = (cfgr >> 17) & 0x1u;      /* PLLXTPRE: 0=HSE 1=HSE/2 */
        unsigned long pllmul = ((cfgr >> 18) & 0xFu) + 2u; /* PLLMUL: ×2..×16 */
        unsigned long in = pllsrc ? (HSE_CLOCK_HZ >> pllxt) : (HSI_CLOCK_HZ / 2u);
        clk = in * pllmul;
    }
    else
    {
        clk = HSI_CLOCK_HZ;                              /* SW=0 或预留值按 HSI */
    }
    return clk;
}

static unsigned long rcc_ahb_hz(void)                     /* HCLK */
{
    static const unsigned long hpre_div[16] =
        { 1,1,1,1,1,1,1,1, 2,4,8,16,64,128,256,512 };
    unsigned long div = (RCC_CFGR >> 4) & 0xFu;
    return rcc_sysclk_hz() / hpre_div[div];
}

static unsigned long rcc_pclk2_hz(void)                   /* PCLK2 = APB2 */
{
    static const unsigned long ppre2_div[8] = { 1,1,1,1, 2,4,8,16 };
    unsigned long div = (RCC_CFGR >> 11) & 0x7u;
    return rcc_ahb_hz() / ppre2_div[div];
}

static unsigned long rcc_pclk1_hz(void)                   /* PCLK1 = APB1 */
{
    static const unsigned long ppre1_div[8] = { 1,1,1,1, 2,4,8,16 };
    unsigned long div = (RCC_CFGR >> 8) & 0x7u;
    return rcc_ahb_hz() / ppre1_div[div];
}

/* ================= 控制台钩子（实现委托 drivers/uart） ================= */
/* 覆盖 RT-Thread 的 weak 空实现：所有内核输出最终汇入串口驱动 */
void rt_hw_console_output(const char *str)
{
    uart_puts(str);
}

/* FINSH 读字节：缓冲空时阻塞在驱动的接收信号量上，shell 线程不占 CPU */
signed char rt_hw_console_getchar(void)
{
    return (signed char)uart_getchar();
}

/* ================= 启动延时（马赛对齐 Zephyr CONFIG_BOOT_DELAY=3000） ================= */
static void boot_delay_ms(unsigned int ms)
{
    /*
     * 忙等延时，用 SysTick 数 1ms 一次（不开 TICKINT，纯查询 COUNTFLAG），
     * 因为 rt_hw_board_init() 早期还没调用 rt_system_timer_init()，
     * 若此时开 SysTick 中断会在第一次 tick 遍历未初始化链表时 HardFault。
     * LOAD 按 HCLK 换算（与串口同源防乱码），1ms ≤ 2^24 计数，精度可靠。
     */
    unsigned long load = rcc_ahb_hz() / 1000u - 1u;

    while (ms-- > 0u)
    {
        SYSTICK_LOAD = load;
        SYSTICK_VAL  = 0u;
        SYSTICK_CTRL = (SYSTICK_CFLAG_CLKSOURCE | SYSTICK_CFLAG_ENABLE);
        while ((SYSTICK_CTRL & 0x10000u) == 0u) { } /* COUNTFLAG */
        SYSTICK_CTRL = 0u; /* 停，留待 systick_init() 重新配置为系统节拍 */
    }
}

/* ================= SysTick（系统节拍） ================= */
static void systick_init(void)
{
    /* 1ms 节拍：LOAD = HCLK/RT_TICK_PER_SECOND - 1（CLKSOURCE=1 时 SysTick 时钟=HCLK） */
    SYSTICK_LOAD = (rcc_ahb_hz() / RT_TICK_PER_SECOND) - 1u;
    SYSTICK_VAL  = 0u;
    SYSTICK_CTRL = (SYSTICK_CFLAG_CLKSOURCE | SYSTICK_CFLAG_TICKINT |
                    SYSTICK_CFLAG_ENABLE);
}

void SysTick_Handler(void)
{
    rt_interrupt_enter();
    rt_tick_increase();
    rt_interrupt_leave();
}

/* ================= 板级外设（每个功能一个函数，入口处只占一行） ================= */

/* I2C0：PB6 = SCL / PB7 = SDA，板载 AT24C02 就挂在这条总线上。
 * 建总线（注册成 adapter 0）+ 空闲电平自检。异常只告警不阻止启动：
 * 用户还能用 msh 的 `i2c scan` / `i2c read` 排查。 */
static void board_i2c_init(void)
{
    struct i2c_adapter *i2c0 = stm32f1_i2c_gpio_init((uint32_t)rcc_ahb_hz());

    /* 两条线都开漏、靠上拉变高；空闲态读回不是高电平说明没上拉或总线被拉死 */
    if (i2c0 == NULL || i2c_bit_recover(i2c0) != 0)
    {
        rt_kprintf("[i2c] I2C0 空闲电平异常（SCL/SDA 未上拉或被拉低），I2C 可能不可用\n");
    }
}

/* 配置存储：EEPROM 器件 → eeprom 适配层 → eeprom_storage 默认实例。
 * 失败只告警：配置读不出来时系统仍应能用默认值跑起来。
 * 换器件的动作全在这一个函数里（at24c02_* → 别的 xxx_register），入口不动。
 * 上层（含 main.c 的自测）一律只认这个默认实例，不再碰器件层。 */
static void board_storage_init(void)
{
    /* module 只记设备指针且要用很久，所以 chip / rom 必须静态 */
    static struct at24c02_dev chip;
    static struct eeprom_dev  rom;

    struct i2c_adapter *i2c0 = i2c_get_adapter(BOARD_I2C_BUS_NR);
    eeprom_storage_t *es;

    if (i2c0 == NULL)
    {
        rt_kprintf("[storage] I2C 总线未就绪，配置存储不可用\n");
        return;
    }

    at24c02_init(&chip, i2c0, (uint16_t)AT24C02_BOARD_ADDR);
    if (at24c02_register(&rom, &chip, AT24C02_CAPACITY) != EEPROM_OK)
    {
        rt_kprintf("[storage] EEPROM 注册失败，配置存储不可用\n");
        return;
    }

    es = eeprom_storage_create();
    if (es == NULL || es->init(es, &rom) != OSAL_OK)
    {
        eeprom_storage_destroy(es);
        rt_kprintf("[storage] 配置存储初始化失败\n");
        return;
    }
    eeprom_storage_set_default(es);
}

/* ================= SPI 总线 =================
 * 本板 SPI1 已被 CH395 / VS1053 / W25Q128 占用（各自驱动自己配），
 * 通用总线用 spi_config.h 里指定的那一路（默认 SPI2，PB13/14/15）。
 * 这里只建「总线」，不建任何设备 —— 片选属于设备，谁用谁在 board/驱动里
 * 用 SPI_CS_PIN(port,pin) 填一个 struct spi_device 即可。
 */
static void board_spi_init(void)
{
    static const struct stm32f1_spi_pins pins =
    {
        SPI0_SCK_PORT_IDX,  SPI0_SCK_PIN,
        SPI0_MISO_PORT_IDX, SPI0_MISO_PIN,
        SPI0_MOSI_PORT_IDX, SPI0_MOSI_PIN,
    };
    unsigned long pclk = (SPI0_HW == 1u) ? rcc_pclk2_hz() : rcc_pclk1_hz();

    if (stm32f1_spi_init(SPI0_HW, (uint32_t)pclk, (uint32_t)rcc_ahb_hz(), &pins) == NULL)
    {
        rt_kprintf("[spi] SPI%u 初始化失败，SPI 总线不可用\n", (unsigned int)SPI0_HW);
    }
}

/* ================= 板载 NOR FLASH（25Q128） =================
 * 挂在 SPI2（spi0）上，片选 PB12。失败只告警：没有 FLASH 时系统照常跑，
 * 只是 flash 相关功能不可用（msh 的 flash 命令会提示未注册）。
 */
static struct w25q128_dev s_flash;

static void board_flash_init(void)
{
    struct spi_controller *spi0 = spi_get_controller(W25Q128_SPI_BUS);

    if (spi0 == NULL)
    {
        rt_kprintf("[flash] SPI 总线未就绪，25Q128 不可用\n");
        return;
    }

    /* 只绑总线，不读 ID —— 这个函数跑在 rt_hw_board_init 阶段，调度器还没起来，
     * 碰总线会在加锁时写坏线程控制块（IMPRECISERR）。读 ID / 等 BUSY 推迟到
     * 第一次真正访问（main 的 flash 自测、msh 的 flash 命令）。 */
    if (w25q128_init(&s_flash, spi0,
                     SPI_CS_PIN(W25Q128_CS_PORT_IDX, W25Q128_CS_PIN)) != 0)
    {
        rt_kprintf("[flash] 25Q128 绑定失败（参数非法）\n");
        return;
    }

    w25q128_set_default(&s_flash);
}

/* ================= SD 卡 =================
 * 只建 SDIO 主机（配引脚 + 上电，不发命令）；**不识别卡**。
 * sd_init() 要和卡通信（CMD0/CMD8/ACMD41…），必须等内核调度器起来之后
 * 才能调 —— 放到这里会在 rt_system_scheduler_init 之前碰 OS 对象。
 * 识别动作由第一次 sd_read_blocks / msh 的 sd 命令自动触发。
 */
static struct sd_card s_sd;

static void board_sd_init(void)
{
    struct sdio_host *host = stm32f1_sdio_init((uint32_t)rcc_ahb_hz());

    if (host == NULL)
    {
        rt_kprintf("[sd] SDIO 主机初始化失败\n");
        return;
    }

    s_sd.host = host;
    s_sd.type = (uint8_t)SD_TYPE_UNKNOWN;
    sd_set_default(&s_sd);
}

/* ================= 板级入口（由 rtthread_startup 调用） ================= */
void rt_hw_board_init(void)
{
    /* 阶段 1：串口硬件就绪（此刻还没有堆，驱动只配寄存器） */
    uart_hw_init(UART_BAUD, rcc_pclk2_hz());

    /* 启动延时 3s：与 Zephyr CONFIG_BOOT_DELAY=3000 对齐，给串口工具抓启动日志开窗口 */
    rt_kprintf("[RT-Thread] boot 启动等待 3.0s（对齐 Zephyr CONFIG_BOOT_DELAY=3000）...\n");
    boot_delay_ms(3000u);

    /*
     * 在使能 SysTick 中断之前，必须先初始化内核定时器链表。
     * rtthread_startup() 里 rt_system_timer_init() 位于 rt_hw_board_init() 之后；
     * 若此处过早使能 SysTick，第一次 tick 触发 rt_timer_check() 会遍历
     * 尚未初始化（全 0 / NULL）的 _timer_list，造成 HardFault（INVSTATE）。
     */
    rt_system_timer_init();
    systick_init();

#ifdef RT_USING_HEAP
    rt_system_heap_init(HEAP_BEGIN, HEAP_END);
#endif

    /* 阶段 2：接收中断 + 字符设备注册 + 设为控制台
     * 必须在 heap_init 之后（rt_sem_create 需要动态内存） */
    uart_console_attach();

    /* 阶段 3：I2C0 总线 —— 建总线 + 空闲电平自检（细节见 board_i2c_init） */
    board_i2c_init();

    /* 阶段 4：配置存储 —— EEPROM → 适配层 → module 默认实例（msh 的 eeprom_* 用它） */
    board_storage_init();

    /* 阶段 5：通用 SPI 总线（换板只改 spi_config.h） */
    board_spi_init();

    /* 阶段 6：板载 NOR FLASH —— 挂在 spi0 上，片选 PB12 */
    board_flash_init();

    /* 阶段 7：SDIO 主机（只配引脚/上电，卡的识别推迟到第一次访问） */
    board_sd_init();
}
