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
 * 用户还能用 msh 的 i2c_scan / i2c_read 排查。 */
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
 * 换器件的动作全在这一个函数里（at24c02_* → 别的 xxx_register），入口不动。 */
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

    /* 阶段 4：配置存储 —— EEPROM → 适配层 → module 默认实例（msh 的 cfg_* 用它） */
    board_storage_init();
}
