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

/* ================= UART1（控制台输出） ================= */
static void uart1_init(void)
{
    unsigned long brr;

    /* 使能 GPIOA、USART1 时钟 */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* PA9 = TX：推挽 + 复用输出，50MHz（CRH 引脚9，CNF=10, MODE=11 → 0b1011） */
    GPIOA_CRH = (GPIOA_CRH & ~(0x0Fu << 4)) | (0x0Bu << 4);

    /* 波特率：BRR = 8MHz / 115200 */
    brr = (SYSTEM_CLOCK_HZ + (UART_BAUD / 2u)) / UART_BAUD;
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

/* ================= SysTick（系统节拍） ================= */
static void systick_init(void)
{
    /* 1ms 节拍：LOAD = (SYSTEM_CLOCK_HZ / RT_TICK_PER_SECOND) - 1 */
    SYSTICK_LOAD = (SYSTEM_CLOCK_HZ / RT_TICK_PER_SECOND) - 1u;
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
    systick_init();

#ifdef RT_USING_HEAP
    rt_system_heap_init(HEAP_BEGIN, HEAP_END);
#endif
}