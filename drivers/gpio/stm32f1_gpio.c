/*
 * stm32f1_gpio.c - STM32F103 GPIO 寄存器级实现（在不引入完整 HAL 库的前提下）
 *
 * 参考 STM32F1 参考手册（RM0008）：
 *   - GPIOA..GPIOE 基地址 0x40010800 ... 0x40011800
 *   - RCC_APB2ENR @ 0x40021018，IOPAEN=bit2, IOPBEN=bit3, ... IOPEEN=bit6
 */
#include <stdint.h>

#include "gpio.h"

/* ---- 寄存器地址 ---- */
#define RCC_BASE        0x40021000u
#define RCC_APB2ENR     (*(volatile uint32_t *)(RCC_BASE + 0x18u))

static const uint32_t s_gpio_base[5] = {
    0x40010800u, /* A */
    0x40010C00u, /* B */
    0x40011000u, /* C */
    0x40011400u, /* D */
    0x40011800u  /* E */
};
/* APB2ENR 中对应端口的使能位 */
static const uint32_t s_port_clk[5] = {
    2u, /* IOPAEN */
    3u, /* IOPBEN */
    4u, /* IOPCEN */
    5u, /* IOPDEN */
    6u  /* IOPEN */
};

#define GPIO_CRL(base)  (*(volatile uint32_t *)((base) + 0x00u)) /* 低 8 脚 */
#define GPIO_CRH(base)  (*(volatile uint32_t *)((base) + 0x04u)) /* 高 8 脚 */
#define GPIO_BSRR(base) (*(volatile uint32_t *)((base) + 0x10u))
#define GPIO_IDR(base)  (*(volatile uint32_t *)((base) + 0x08u))

void gpio_init(void)
{
    /* 全部 GPIO 端口时钟使能（板级资源较小，一次性开启） */
    RCC_APB2ENR |= ((1u << s_port_clk[0]) | (1u << s_port_clk[1]) |
                    (1u << s_port_clk[2]) | (1u << s_port_clk[3]) |
                    (1u << s_port_clk[4]));
}

void gpio_output_init(gpio_pin_enc_t pin)
{
    uint32_t port = (pin >> 8) & 0xFFu;
    uint32_t p    = pin & 0xFFu;
    volatile uint32_t *cfg;
    uint32_t bitoff, mask;

    if (port > 4u)
        return;

    RCC_APB2ENR |= (1u << s_port_clk[port]);

    cfg    = (p < 8) ? (&GPIO_CRL(s_gpio_base[port])) : (&GPIO_CRH(s_gpio_base[port]));
    bitoff = (p & 0x07u) * 4u;
    mask   = 0xFu << bitoff;

    /* MODE=11(50MHz), CNF=00(推挽输出) → 0x3 */
    *cfg = (*cfg & ~mask) | (0x3u << bitoff);
}

void gpio_write(gpio_pin_enc_t pin, int val)
{
    uint32_t port = (pin >> 8) & 0xFFu;
    uint32_t p    = pin & 0xFFu;

    if (port > 4u)
        return;

    if (val)
        GPIO_BSRR(s_gpio_base[port]) = (1u << p);       /* BSx 置位 */
    else
        GPIO_BSRR(s_gpio_base[port]) = (1u << (p + 16u)); /* BRx 复位 */
}

int gpio_read(gpio_pin_enc_t pin)
{
    uint32_t port = (pin >> 8) & 0xFFu;
    uint32_t p    = pin & 0xFFu;

    if (port > 4u)
        return 0;

    return (GPIO_IDR(s_gpio_base[port]) & (1u << p)) ? 1 : 0;
}