/*
 * stm32f1_i2c.c - STM32F1 的 I2C 引脚原语实现 <drivers/i2c/>
 *
 * 只做三件事：
 *   1) 把 GPIOB 的 SCL / SDA 配成推挽 / 开漏输出
 *   2) 提供 set_sda / set_scl / get_sda 三个回调（纯寄存器操作）
 *   3) 用 DWT 周期计数器提供精确 µs 延时
 *
 * 这里不 include 任何 RTOS 头，也不 include board.h：
 * 主频由外层通过 stm32f1_i2c_gpio_init(hclk_hz) 注入。
 */
#include <stdint.h>

#include "i2c.h"
#include "stm32f1_i2c.h"
#include "i2c_config.h"

/* ================= 寄存器 ================= */
#define RCC_BASE            (0x40021000u)
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x18u))

#define GPIO_CRL(base)      (*(volatile uint32_t *)((base) + 0x00u))
#define GPIO_CRH(base)      (*(volatile uint32_t *)((base) + 0x04u))
#define GPIO_IDR(base)      (*(volatile uint32_t *)((base) + 0x08u))
#define GPIO_BSRR(base)     (*(volatile uint32_t *)((base) + 0x10u))

/* GPIOA..GPIOE 基址，下标即端口编号（0=A 1=B 2=C 3=D 4=E） */
static const uint32_t s_gpio_base[5] =
{
    0x40010800u,    /* GPIOA */
    0x40010C00u,    /* GPIOB */
    0x40011000u,    /* GPIOC */
    0x40011400u,    /* GPIOD */
    0x40011800u,    /* GPIOE */
};

/* ================= DWT（Data Watchpoint and Trace）=================
 * 用它当 µs 计时源：不占 SysTick（RT-Thread 的 1ms 节拍在用），
 * 也不需要额外定时器，Cortex-M3 全系列都有。
 */
#define DEMCR               (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL            (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT          (*(volatile uint32_t *)0xE0001004u)

#define DEMCR_TRCENA        (1u << 24)
#define DWT_CTRL_CYCCNTENA  (1u << 0)

/* 每 µs 的 CPU 周期数，0 表示 CYCCNT 不可用 */
static uint32_t s_cycles_per_us = 8u;

/* ================= 引脚配置 ================= */

/* 开某端口时钟（APB2ENR 的 IOPxEN = bit(2 + 端口号)） */
static void port_clk_enable(uint32_t port)
{
    if (port < 5u)
    {
        RCC_APB2ENR |= (1u << (2u + port));
    }
}

/* 按 CRL/CRH 的 4bit 粒度配置一个引脚（调用前必须先使能端口时钟） */
static void pin_config(uint32_t port, uint32_t pin, uint32_t nibble)
{
    volatile uint32_t *cr;
    uint32_t shift;

    if (port > 4u || pin > 15u)
    {
        return;
    }

    cr = (pin < 8u) ? &GPIO_CRL(s_gpio_base[port]) : &GPIO_CRH(s_gpio_base[port]);
    shift = (pin & 0x07u) * 4u;
    *cr = (*cr & ~(0xFu << shift)) | ((nibble & 0xFu) << shift);
}

/* ================= 引脚原语（i2c_bit_ops 回调） ================= */

static void stm32f1_set_sda(void *ctx, int level)
{
    uint32_t base = s_gpio_base[I2C_SDA_PORT_IDX];

    (void)ctx;
    /* SDA 是开漏输出：写 1 = 关断输出管 = 释放（靠上拉变高），写 0 = 主动拉低 */
    if (level)
    {
        GPIO_BSRR(base) = (1u << I2C_SDA_PIN);
    }
    else
    {
        GPIO_BSRR(base) = (1u << (I2C_SDA_PIN + 16u));
    }
}

static void stm32f1_set_scl(void *ctx, int level)
{
    uint32_t base = s_gpio_base[I2C_SCL_PORT_IDX];

    (void)ctx;
    if (level)
    {
        GPIO_BSRR(base) = (1u << I2C_SCL_PIN);
    }
    else
    {
        GPIO_BSRR(base) = (1u << (I2C_SCL_PIN + 16u));
    }
}

static int stm32f1_get_sda(void *ctx)
{
    uint32_t base = s_gpio_base[I2C_SDA_PORT_IDX];

    (void)ctx;
    return (GPIO_IDR(base) & (1u << I2C_SDA_PIN)) ? 1 : 0;
}

static int stm32f1_get_scl(void *ctx)
{
    uint32_t base = s_gpio_base[I2C_SCL_PORT_IDX];

    (void)ctx;
    return (GPIO_IDR(base) & (1u << I2C_SCL_PIN)) ? 1 : 0;
}

/* µs 忙等。DWT 计数是自由运行的，用差值比较，天然处理回绕。 */
static void stm32f1_delay_us(void *ctx, unsigned int us)
{
    uint32_t start;
    uint32_t ticks;

    (void)ctx;
    if (us == 0u)
    {
        return;
    }

    ticks = (uint32_t)us * s_cycles_per_us;
    start = DWT_CYCCNT;
    while ((DWT_CYCCNT - start) < ticks)
    {
        /* 等待 */
    }
}

/* ================= adapter 实例 ================= */

static struct i2c_bit_ops s_bit_ops =
{
    .ctx                = 0,
    .set_sda            = stm32f1_set_sda,
    .set_scl            = stm32f1_set_scl,
    .get_sda            = stm32f1_get_sda,
    .get_scl            = stm32f1_get_scl,
    .delay_us           = stm32f1_delay_us,
    .half_period_us     = I2C_DELAY_US,
    .stretch_timeout_us = I2C_STRETCH_TIMEOUT_US,
};

static struct i2c_adapter s_i2c0 =
{
    .name      = "i2c0",
    .algo      = &i2c_bit_algorithm,
    .algo_data = &s_bit_ops,
    .retries   = 3,
};

/* ================= 对外初始化 =================
 * hclk_hz：AHB 时钟（µs 延时用），由 BSP 注入，驱动内不写死主频。
 */
struct i2c_adapter *stm32f1_i2c_gpio_init(uint32_t hclk_hz)
{
    port_clk_enable(I2C_SCL_PORT_IDX);
    port_clk_enable(I2C_SDA_PORT_IDX);

    /* 先把输出寄存器置 1（此刻引脚还是复位后的输入浮空态，写 ODR 无副作用），
     * 再切成输出模式 —— 否则开漏输出刚生效时 ODR=0，会把 SDA/SCL 瞬间拉低，
     * 总线上出现一个毛刺。 */
    stm32f1_set_sda(&s_bit_ops, 1);
    stm32f1_set_scl(&s_bit_ops, 1);

    pin_config(I2C_SCL_PORT_IDX, I2C_SCL_PIN, I2C_SCL_CFG_NIBBLE);
    pin_config(I2C_SDA_PORT_IDX, I2C_SDA_PIN, I2C_SDA_CFG_NIBBLE);

    /* 进入空闲态：SCL = 1，SDA = 1（释放） */
    stm32f1_set_sda(&s_bit_ops, 1);
    stm32f1_set_scl(&s_bit_ops, 1);

    /* 打开 DWT 周期计数器 */
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0u;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;

    s_cycles_per_us = hclk_hz / 1000000u;
    if (s_cycles_per_us == 0u)
    {
        s_cycles_per_us = 8u;           /* 兜底：按 8MHz 算 */
    }

    /* 上电瞬间从机若把 SDA 拉死，先打时钟恢复，避免后续全部 NACK */
    (void)i2c_bit_recover(&s_i2c0);

    (void)i2c_add_adapter(&s_i2c0);
    return &s_i2c0;
}
