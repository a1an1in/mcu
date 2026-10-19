/*
 * board.c - 板级初始化（STM32F103）
 *
 * 职责：
 *   1) 初始化 UART1（115200-8-N-1），作为 rt_kprintf / osal_printf 的输出通道
 *   2) 配置 SysTick 产生 1ms 系统时钟节拍
 *   3) 初始化 RT-Thread 动态内存堆
 *   4) 实现 rt_hw_console_output（覆盖 RT-Thread 的 weak 空实现）
 *
 * 时钟方案：使用内部高速时钟 HSI（8MHz）作为系统时钟，不做 PLL 超频，
 * 降低对外部晶振的依赖，便于在多种 F103 板卡上跑通。
 */
#include <rtthread.h>

#include "board.h"

/* ================= STM32F1 寄存器（最小子集） ================= */
#define RCC_BASE        0x40021000u
#define RCC_APB2ENR     (*(volatile unsigned long *)(RCC_BASE + 0x18u))
#define RCC_IOPAEN      (0x1u << 2)
#define RCC_USART1EN    (0x1u << 14)

#define GPIOA_BASE      0x40010800u
#define GPIOA_CRH       (*(volatile unsigned long *)(GPIOA_BASE + 0x04u))

#define USART1_BASE     0x40013800u
#define USART1_SR       (*(volatile unsigned long *)(USART1_BASE + 0x00u))
#define USART1_DR       (*(volatile unsigned long *)(USART1_BASE + 0x04u))
#define USART1_BRR      (*(volatile unsigned long *)(USART1_BASE + 0x08u))
#define USART1_CR1      (*(volatile unsigned long *)(USART1_BASE + 0x0Cu))

#define SYSTICK_BASE    0xE000E010u
#define SYSTICK_CTRL    (*(volatile unsigned long *)(SYSTICK_BASE + 0x00u))
#define SYSTICK_LOAD    (*(volatile unsigned long *)(SYSTICK_BASE + 0x04u))
#define SYSTICK_VAL     (*(volatile unsigned long *)(SYSTICK_BASE + 0x08u))
#define SYSTICK_CFLAG_CLKSOURCE (0x1u << 2)
#define SYSTICK_CFLAG_TICKINT   (0x1u << 1)
#define SYSTICK_CFLAG_ENABLE    (0x1u << 0)

#define UART_BAUD       115200u

/* ================= RCC 时钟估算（防乱码：UART/SysTick 与实际时钟解耦） =================
 * 始终按 STM32F1 RCC_CFGR 实际位配置推算系统时钟/总线时钟：
 *   - UART1(BRR)  用 PCLK2(APB2)，保证波特率在任何时钟方案下都精确 = 115200
 *   - SysTick     CLKSOURCE=1 时时钟 = HCLK(AHB)，保证 1ms 节拍精确
 * 头注释默认"HSI 8MHz"，补丁后即使换成 HSE/PLL(72MHz) 也不会出现串口乱码/节拍偏快。 */
#define RCC_CFGR        (*(volatile unsigned long *)(RCC_BASE + 0x04u))

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

/* ================= UART1（控制台输出） ================= */
static void uart1_init(void)
{
    unsigned long brr;

    /* 使能 GPIOA、USART1 时钟 */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* PA9 = TX：推挽 + 复用输出，50MHz（CRH 引脚9，CNF=10, MODE=11 → 0b1011） */
    GPIOA_CRH = (GPIOA_CRH & ~(0x0Fu << 4)) | (0x0Bu << 4);

    /* 波特率：BRR = PCLK2(实际APB2) / 115200，与系统时钟方案无关，恒为精确 115200 */
    brr = (rcc_pclk2_hz() + (UART_BAUD / 2u)) / UART_BAUD;
    USART1_BRR = (unsigned long)brr;

    /* UE=13, TE=3, RE=2 */
    USART1_CR1 = (0x1u << 13) | (0x1u << 3) | (0x1u << 2);
}

static void uart1_putc(char c)
{
    if (c == '\n')
    {
        while ((USART1_SR & (0x1u << 7)) == 0u) { }
        USART1_DR = (unsigned long)'\r';
    }
    while ((USART1_SR & (0x1u << 7)) == 0u) { }
    USART1_DR = (unsigned long)c;
}

/* 覆盖 RT-Thread 的 weak 空实现：所有内核输出最终汇入此函数 */
void rt_hw_console_output(const char *str)
{
    while (*str != '\0')
    {
        uart1_putc(*str++);
    }
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

/* ================= 板级入口（由 rtthread_startup 调用） ================= */
void rt_hw_board_init(void)
{
    uart1_init();

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
}