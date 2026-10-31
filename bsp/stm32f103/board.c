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
 * 时钟方案：HSE 8MHz × PLL9 = 72MHz（AHB 72 / APB1 36 / APB2 72）；
 * HSE 或 PLL 起振失败时自动留在 HSI 8MHz。各驱动都按 RCC_CFGR 现算频率，
 * 换主频不用改驱动。原理与踩坑见 docs/stm32f1-clock.md。
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
#include "gpio.h"
#include "vs1053.h"
#include "vs1053_config.h"
#include "w25q128.h"
#include "w25q128_config.h"
#include "sd.h"
#include "sd_blkdev.h"
#include "stm32f1_sdio.h"

/* ================= 板级事实 ================= */
#define UART_BAUD       115200u
#define BOARD_I2C_BUS_NR 0u     /* stm32f1_i2c_gpio_init() 固定把这条总线注册成 0 */
#define RCC_BASE        0x40021000u
#define RCC_CR          (*(volatile unsigned long *)(RCC_BASE + 0x00u))
#define RCC_CFGR        (*(volatile unsigned long *)(RCC_BASE + 0x04u))

#define FLASH_ACR       (*(volatile unsigned long *)0x40022000u)
#define FLASH_ACR_LATENCY_2  (2u << 0)
#define FLASH_ACR_HALF_CYCLE (1u << 3)   /* 与预取互斥，72MHz 下不能开 */
#define FLASH_ACR_PREFETCH   (1u << 4)

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

/* ================= 系统时钟：HSE 8MHz × PLL9 = 72MHz =================
 *
 * 之前一直跑复位默认的 HSI 8MHz（只读 RCC_CFGR 推算，从没配过）。72MHz 能
 * 让 SDIO 的 FIFO 轮询、网络、所有软件循环都宽裕 9 倍 —— 4 位 SD 卡跑不动
 * 的根因就是 CPU 跟不上数据率。
 *
 * 顺序不能变：Flash 等待周期 → APB1 分频 → PLL → 切 SW。
 * 任何一步漏了，表现都是上电跑飞或串口乱码。
 *
 * HSE 起振失败时**什么都不改**，继续用 HSI 8MHz —— rcc_*_hz() 都是运行时
 * 从 RCC_CFGR 推算的，各驱动会自动按 8MHz 工作。
 */
static int system_clock_init(void)
{
    unsigned long timeout;

    /* 1) Flash：72MHz 需要 2 个等待周期，且必须在提速**之前**设好，
     *    否则取指直接跑飞。
     *    ⚠️ 只开预取（PRFTBE），**不能**同时开半周期访问（HLFCYA）——
     *    两者互斥，都置上会让 72MHz 下的取指全错（实测 PC 直接跳到 0）。 */
    FLASH_ACR = FLASH_ACR_LATENCY_2 | FLASH_ACR_PREFETCH;

    /* 2) 打开 HSE（板载 8MHz 晶振），等起振 */
    RCC_CR |= (1u << 16);                   /* HSEON */
    timeout = 0x40000u;
    while ((RCC_CR & (1u << 17)) == 0u)     /* HSERDY */
    {
        if (--timeout == 0u)
        {
            RCC_CR &= ~(1u << 16);          /* 起振失败，关掉，留在 HSI */
            return -1;
        }
    }

    /* 3) 总线分频 + PLL 参数：
     *      AHB  = /1  → 72MHz
     *      APB1 = /2  → 36MHz（F103 的 APB1 上限）
     *      APB2 = /1  → 72MHz
     *      ADC  = /6  → 12MHz（不用 ADC，但按规范设）
     *      PLL  = HSE(8MHz) × 9 = 72MHz
     *    位段：HPRE[7:4] PPRE1[10:8] PPRE2[13:11] ADCPRE[15:14]
     *          PLLSRC[16] PLLXTPRE[17] PLLMUL[21:18] */
    RCC_CFGR = (RCC_CFGR & ~0x003FFFF0u) |
               (0x0u << 4) |                /* HPRE  /1  */
               (0x4u << 8) |                /* PPRE1 /2  */
               (0x0u << 11) |               /* PPRE2 /1  */
               (0x2u << 14) |               /* ADCPRE /6 */
               (1u << 16) |                 /* PLLSRC = HSE（不是 HSI/2） */
               (0x7u << 18);                /* PLLMUL = ×9 */

    /* 4) 打开 PLL 并等锁定 */
    RCC_CR |= (1u << 24);                   /* PLLON */
    timeout = 0x40000u;
    while ((RCC_CR & (1u << 25)) == 0u)     /* PLLRDY */
    {
        if (--timeout == 0u)
        {
            RCC_CR &= ~(1u << 24);
            return -1;                      /* PLL 没锁上，仍跑 HSI */
        }
    }

    /* 5) 切换系统时钟到 PLL，并等切换完成（SWS） */
    RCC_CFGR = (RCC_CFGR & ~0x3u) | 0x2u;   /* SW = PLL */
    timeout = 0x40000u;
    while (((RCC_CFGR >> 2) & 0x3u) != 0x2u)
    {
        if (--timeout == 0u)
        {
            return -1;
        }
    }

    return 0;
}

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
    /* SPI1：PA5/PA6/PA7，CH395Q 与 VS1053 共用。
     * 名字由 stm32f1_spi.c 起成 "stm32f1_spi1"，ch395_port.c 按名字取它的锁。 */
    static const struct stm32f1_spi_pins spi1_pins =
    {
        0u, 5u,     /* SCK  PA5 */
        0u, 6u,     /* MISO PA6 */
        0u, 7u,     /* MOSI PA7 */
    };
    unsigned long pclk = (SPI0_HW == 1u) ? rcc_pclk2_hz() : rcc_pclk1_hz();

    if (stm32f1_spi_init(SPI0_HW, (uint32_t)pclk, (uint32_t)rcc_ahb_hz(), &pins) == NULL)
    {
        rt_kprintf("[spi] SPI%u 初始化失败，SPI 总线不可用\n", (unsigned int)SPI0_HW);
    }

    if (stm32f1_spi_init(1u, (uint32_t)rcc_pclk2_hz(),
                         (uint32_t)rcc_ahb_hz(), &spi1_pins) == NULL)
    {
        rt_kprintf("[spi] SPI1 初始化失败，CH395 / VS1053 不可用\n");
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

/* ================= VS1053 音频（只绑定，不做 I/O） =================
 * VS1053 与 CH395 共用 SPI1：
 *   - 这里把 XCS/XDCS 拉高、RST 拉低（保持复位），避免它在上电阶段
 *     驱动 MISO 和 CH395 抢总线；
 *   - 真正的复位/软复位/音量设置推迟到第一次 music 命令（调度器起来后）。
 */
static struct vs1053 s_vs1053;

static void board_audio_init(void)
{
    struct spi_controller *spi1 = spi_get_controller_by_name("stm32f1_spi1");

    /* 先把输出锁存器设成期望电平，再切输出，避免切换瞬间的低电平毛刺 */
    gpio_write(VS1053_XCS_PIN, 1);
    gpio_write(VS1053_XDCS_PIN, 1);
    gpio_output_init(VS1053_XCS_PIN);
    gpio_output_init(VS1053_XDCS_PIN);
    gpio_write(VS1053_RST_PIN, 0);
    gpio_output_init(VS1053_RST_PIN);

    if (spi1 == NULL)
    {
        rt_kprintf("[audio] SPI1 未注册，VS1053 不可用\n");
        return;
    }

    if (vs1053_bind(&s_vs1053, spi1) != VS1053_OK)
    {
        rt_kprintf("[audio] VS1053 绑定失败\n");
        return;
    }
    vs1053_set_default(&s_vs1053);
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

    /* 注册成块设备 "sd0"：只登记，不识别卡。文件系统第一次 f_mount 时
     * 才通过 blkdev_init() 触发 sd_init()。 */
    if (sd_blkdev_register(&s_sd, "sd0") == NULL)
    {
        rt_kprintf("[sd] 块设备注册失败（注册表满？）\n");
    }
}

/* ================= 板级入口（由 rtthread_startup 调用） ================= */
void rt_hw_board_init(void)
{
    int clock_ok;

    /* 阶段 0：系统时钟（HSE ×9 = 72MHz；失败自动留在 HSI 8MHz）。
     * 必须在串口之前 —— 波特率、SysTick、各外设分频都按切换后的频率算。
     * 注意：失败时这里**不能**打印，串口还没初始化（先记下来，后面再报）。 */
    clock_ok = system_clock_init();

    /* 阶段 1：串口硬件就绪（此刻还没有堆，驱动只配寄存器） */
    uart_hw_init(UART_BAUD, rcc_pclk2_hz());

    if (clock_ok != 0)
    {
        rt_kprintf("[clock] HSE/PLL 未就绪，退回 HSI %lu Hz\n",
                   (unsigned long)rcc_sysclk_hz());
    }
    else
    {
        rt_kprintf("[clock] SYSCLK %lu Hz（HCLK %lu / PCLK1 %lu / PCLK2 %lu）\n",
                   (unsigned long)rcc_sysclk_hz(),
                   (unsigned long)rcc_ahb_hz(),
                   (unsigned long)rcc_pclk1_hz(),
                   (unsigned long)rcc_pclk2_hz());
    }

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

    /* 阶段 5.5：VS1053 绑定（只配引脚，不做 SPI 通信） */
    board_audio_init();

    /* 阶段 6：板载 NOR FLASH —— 挂在 spi0 上，片选 PB12 */
    board_flash_init();

    /* 阶段 7：SDIO 主机（只配引脚/上电，卡的识别推迟到第一次访问） */
    board_sd_init();
}
