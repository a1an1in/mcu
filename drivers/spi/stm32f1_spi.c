/*
 * stm32f1_spi.c - STM32F1 的 SPI 控制器实现 <drivers/spi/>
 *
 * 只做三件事：
 *   1) 把 SCK / MOSI 配成复用推挽、MISO 配成上拉输入，打开对应 APB 时钟；
 *   2) 用 CR1 的 CPOL/CPHA/BR/LSBFIRST 实现 Linux 的 mode / 速率 / 位序；
 *   3) 逐字节全双工搬运（等 TXE → 写 DR → 等 RXNE → 读 DR），带超时。
 *
 * 不 include 任何 RTOS 头，也不 include board.h —— 引脚与 APB 时钟都由
 * 外层通过 stm32f1_spi_init() 注入，换板 / 换主频不用改本文件。
 *
 * CS 也在本文件处理：片选是「设备 + 板子怎么接」的事实，位置由
 * struct spi_device.cs_pin 给出，极性由 mode 的 SPI_CS_HIGH 决定。
 */
#include <stdint.h>
#include <stddef.h>

#include "spi.h"
#include "stm32f1_spi.h"

/* ================= 寄存器 ================= */
#define RCC_BASE            (0x40021000u)
#define RCC_APB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x1Cu))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x18u))

#define GPIO_CRL(base)      (*(volatile uint32_t *)((base) + 0x00u))
#define GPIO_CRH(base)      (*(volatile uint32_t *)((base) + 0x04u))
#define GPIO_BSRR(base)     (*(volatile uint32_t *)((base) + 0x10u))

#define SPI_CR1(b)          (*(volatile uint32_t *)((b) + 0x00u))
#define SPI_SR(b)           (*(volatile uint32_t *)((b) + 0x08u))
#define SPI_DR(b)           (*(volatile uint32_t *)((b) + 0x0Cu))

/* GPIOA..GPIOG 基址，下标即端口编号 0=A 1=B 2=C 3=D 4=E 5=F 6=G */
static const uint32_t s_gpio_base[7] =
{
    0x40010800u,    /* GPIOA */
    0x40010C00u,    /* GPIOB */
    0x40011000u,    /* GPIOC */
    0x40011400u,    /* GPIOD */
    0x40011800u,    /* GPIOE */
    0x40011C00u,    /* GPIOF */
    0x40012000u,    /* GPIOG */
};

/* 控制器名字：下标 = spi_hw。spi_get_controller_by_name() 用它来区分
 * 同一颗 MCU 上的三路 SPI（CH395 在 SPI1、25Q128 在 SPI2）。 */
static const char *const s_spi_name[4] =
{
    "", "stm32f1_spi1", "stm32f1_spi2", "stm32f1_spi3"
};

/* SPI1 在 APB2，SPI2 / SPI3 在 APB1 */
static const uint32_t s_spi_base[4] =
{
    0u,
    0x40013000u,    /* SPI1 */
    0x40003800u,    /* SPI2 */
    0x40003C00u,    /* SPI3 */
};

/* SPI_CR1 位（RM0008 §25.5.1） */
#define SPI_CR1_CPHA        (1u << 0)
#define SPI_CR1_CPOL        (1u << 1)
#define SPI_CR1_MSTR        (1u << 2)
#define SPI_CR1_SPE         (1u << 6)
#define SPI_CR1_LSBFIRST    (1u << 7)
#define SPI_CR1_SSI         (1u << 8)
#define SPI_CR1_SSM         (1u << 9)

/* SPI_SR 位 */
#define SPI_SR_RXNE         (1u << 0)
#define SPI_SR_TXE          (1u << 1)
#define SPI_SR_BSY          (1u << 7)

/* CRL / CRH 每个引脚 4bit = CNF[1:0] + MODE[1:0]（RM0008 Table 20）
 *   0x3 = 推挽输出 50MHz（片选）
 *   0x8 = 上拉/下拉输入（MISO：没挂器件时也有确定电平）
 *   0xB = 复用推挽输出 50MHz（SCK / MOSI） */
#define PIN_NIBBLE_OUT_PP       0x3u
#define PIN_NIBBLE_IN_PULL      0x8u
#define PIN_NIBBLE_AF_PP        0xBu

/* 等 TXE / RXNE / BSY 的最大自旋次数。8MHz HCLK 下约 50ms，
 * 足够覆盖 s_speed 最低时（PCLK/256）一个字节的移出时间。 */
#define SPI_SPIN_MAX            200000u

/* ================= DWT 微秒延时（段间 delay_usecs 用） ================= */
#define DEMCR               (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL            (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT          (*(volatile uint32_t *)0xE0001004u)

#define DEMCR_TRCENA        (1u << 24)
#define DWT_CTRL_CYCCNTENA  (1u << 0)

static uint32_t s_cycles_per_us;

/* ================= 控制器私有数据 ================= */
struct stm32f1_spi_priv {
    uint32_t base;          /* SPIx 寄存器基址 */
    uint32_t pclk_hz;       /* 该 SPI 所在 APB 的时钟 */
    uint8_t  hw;            /* 1 / 2 / 3 */
};

static struct stm32f1_spi_priv s_priv[4];       /* 下标即 hw，0 不用 */
static struct spi_controller s_ctlr[4];

/* ================= 引脚 ================= */

static void pin_config(uint8_t port, uint8_t pin, uint32_t nibble)
{
    volatile uint32_t *cr;
    uint32_t shift;

    if ((port > 6u) || (pin > 15u))
    {
        return;
    }

    RCC_APB2ENR |= (1u << (2u + port));     /* IOPxEN，重复置位无害 */

    cr = (pin < 8u) ? &GPIO_CRL(s_gpio_base[port]) : &GPIO_CRH(s_gpio_base[port]);
    shift = 4u * (pin & 7u);

    *cr = (*cr & ~(0xFu << shift)) | ((nibble & 0xFu) << shift);
}

static void cs_pin_config(uint32_t cs_pin)
{
    uint8_t port = (uint8_t)((cs_pin >> 8) & 0xFFu);
    uint8_t pin  = (uint8_t)(cs_pin & 0xFFu);

    pin_config(port, pin, PIN_NIBBLE_OUT_PP);
}

/* ================= 片选 ================= */

static void stm32f1_spi_set_cs(struct spi_controller *ctlr, struct spi_device *spi,
                               int active)
{
    uint32_t port;
    uint32_t pin;
    uint32_t level;

    (void)ctlr;

    if (spi->cs_pin == SPI_CS_NONE)
    {
        return;
    }

    port = (spi->cs_pin >> 8) & 0xFFu;
    pin  = spi->cs_pin & 0xFFu;
    if ((port > 6u) || (pin > 15u))
    {
        return;
    }

    /* SPI_CS_HIGH：高有效；否则低有效（绝大多数器件） */
    level = ((spi->mode & SPI_CS_HIGH) != 0u) ? (uint32_t)active
                                              : (uint32_t)((active == 0) ? 1u : 0u);

    /* BSRR：低 16 位置 1、高 16 位置 0，单次写、无读改写 */
    GPIO_BSRR(s_gpio_base[port]) = (level != 0u) ? (1u << pin) : (1u << (pin + 16u));
}

/* ================= 速率 ================= */

/* 取 BR[2:0]：实际 SCLK = pclk / 2^(BR+1)。
 * 从最快的分频往上找，选「不超过器件上限」里最快的那个 —— 宁慢勿快。 */
static uint32_t stm32f1_spi_br(uint32_t pclk_hz, uint32_t speed_hz)
{
    uint32_t br;

    if (speed_hz == 0u)
    {
        return 7u;          /* 没要求就用最慢档，最保守 */
    }

    for (br = 0u; br < 7u; br++)
    {
        if ((pclk_hz / (2u << br)) <= speed_hz)
        {
            return br;
        }
    }
    return 7u;
}

/* ================= 算法 ================= */

static int stm32f1_spi_setup(struct spi_controller *ctlr, struct spi_device *spi)
{
    struct stm32f1_spi_priv *priv = (struct stm32f1_spi_priv *)ctlr->algo_data;
    uint32_t cr1;
    uint8_t bpw;

    bpw = (spi->bits_per_word != 0u) ? spi->bits_per_word : 8u;
    if (bpw != 8u)
    {
        return SPI_ERR_PARAM;       /* 本实现只做 8 位；16 位要动 DR 访问宽度 */
    }

    if (spi->cs_pin != SPI_CS_NONE)
    {
        cs_pin_config(spi->cs_pin); /* 幂等：每次消息前确认 CS 是输出 */
    }

    cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
    /* max_speed_hz == 0 时 stm32f1_spi_br() 返回最慢档 —— 不认识的器件宁慢勿快 */
    cr1 |= (stm32f1_spi_br(priv->pclk_hz, spi->max_speed_hz) << 3);

    if ((spi->mode & SPI_CPOL) != 0u)
    {
        cr1 |= SPI_CR1_CPOL;
    }
    if ((spi->mode & SPI_CPHA) != 0u)
    {
        cr1 |= SPI_CR1_CPHA;
    }
    if ((spi->mode & SPI_LSB_FIRST) != 0u)
    {
        cr1 |= SPI_CR1_LSBFIRST;
    }

    /* 先关 SPE 改配置，再开 —— 换器件（模式/速率不同）时不用二次初始化 */
    SPI_CR1(priv->base) = cr1;
    SPI_CR1(priv->base) = cr1 | SPI_CR1_SPE;

    return 0;
}

static int stm32f1_spi_transfer_one(struct spi_controller *ctlr,
                                    struct spi_device *spi,
                                    struct spi_transfer *xfer)
{
    struct stm32f1_spi_priv *priv = (struct stm32f1_spi_priv *)ctlr->algo_data;
    const uint8_t *tx = (const uint8_t *)xfer->tx_buf;
    uint8_t *rx = (uint8_t *)xfer->rx_buf;
    uint16_t i;

    (void)spi;

    for (i = 0; i < xfer->len; i++)
    {
        uint32_t spin;
        uint8_t in;

        /* 只收时不发有效数据：发 0xFF 让 MOSI 保持高电平（多数器件的习惯） */
        uint8_t out = (tx != 0) ? tx[i] : 0xFFu;

        spin = SPI_SPIN_MAX;
        while ((SPI_SR(priv->base) & SPI_SR_TXE) == 0u)
        {
            if (--spin == 0u)
            {
                return SPI_ERR_TIMEOUT;
            }
        }
        SPI_DR(priv->base) = out;

        spin = SPI_SPIN_MAX;
        while ((SPI_SR(priv->base) & SPI_SR_RXNE) == 0u)
        {
            if (--spin == 0u)
            {
                return SPI_ERR_TIMEOUT;
            }
        }
        in = (uint8_t)SPI_DR(priv->base);   /* 读 DR 同时清 RXNE */

        if (rx != 0)
        {
            rx[i] = in;
        }
    }

    /* 等最后一个字节真正移出总线，否则紧接着拉 CS 会截断最后几位 */
    {
        uint32_t spin = SPI_SPIN_MAX;

        while ((SPI_SR(priv->base) & SPI_SR_BSY) != 0u)
        {
            if (--spin == 0u)
            {
                return SPI_ERR_TIMEOUT;
            }
        }
    }

    return 0;
}

static const struct spi_algorithm stm32f1_spi_algo =
{
    .setup       = stm32f1_spi_setup,
    .transfer_one = stm32f1_spi_transfer_one,
    .set_cs      = stm32f1_spi_set_cs,
};

/* ================= 初始化 ================= */

static void stm32f1_spi_delay_us(void *ctx, unsigned int us)
{
    uint32_t start;
    uint32_t ticks;

    (void)ctx;

    if (s_cycles_per_us == 0u)
    {
        return;
    }

    /* 与 stm32f1_i2c.c 同一套写法：靠无符号减法处理计数器回绕 */
    ticks = us * s_cycles_per_us;
    start = DWT_CYCCNT;
    while ((DWT_CYCCNT - start) < ticks)
    {
    }
}

struct spi_controller *stm32f1_spi_init(unsigned int spi_hw, uint32_t pclk_hz,
                                        uint32_t hclk_hz,
                                        const struct stm32f1_spi_pins *pins)
{
    struct stm32f1_spi_priv *priv;

    if ((spi_hw < 1u) || (spi_hw > 3u) || (pins == NULL) || (pclk_hz == 0u))
    {
        return NULL;
    }

    /* 幂等：同一路 SPI 重复调用直接返回已建好的控制器 */
    if (s_ctlr[spi_hw].algo != NULL)
    {
        return &s_ctlr[spi_hw];
    }

    /* SPI 外设时钟：SPI1 在 APB2ENR bit12，SPI2/3 在 APB1ENR bit14/15 */
    if (spi_hw == 1u)
    {
        RCC_APB2ENR |= (1u << 12);
    }
    else
    {
        RCC_APB1ENR |= (1u << (12u + spi_hw));
    }

    pin_config(pins->sck_port, pins->sck_pin, PIN_NIBBLE_AF_PP);
    pin_config(pins->mosi_port, pins->mosi_pin, PIN_NIBBLE_AF_PP);
    pin_config(pins->miso_port, pins->miso_pin, PIN_NIBBLE_IN_PULL);

    /* DWT 周期计数器：段间延时的时基（不占 SysTick） */
    if (hclk_hz != 0u)
    {
        s_cycles_per_us = hclk_hz / 1000000u;
        DEMCR |= DEMCR_TRCENA;
        DWT_CYCCNT = 0u;
        DWT_CTRL |= DWT_CTRL_CYCCNTENA;
    }

    priv = &s_priv[spi_hw];
    priv->base = s_spi_base[spi_hw];
    priv->pclk_hz = pclk_hz;
    priv->hw = (uint8_t)spi_hw;

    s_ctlr[spi_hw].name = s_spi_name[spi_hw];
    s_ctlr[spi_hw].algo = &stm32f1_spi_algo;
    s_ctlr[spi_hw].algo_data = priv;
    s_ctlr[spi_hw].max_speed_hz = pclk_hz / 2u;
    s_ctlr[spi_hw].delay_us = stm32f1_spi_delay_us;
    s_ctlr[spi_hw].delay_ctx = NULL;

    if (spi_add_controller(&s_ctlr[spi_hw]) < 0)
    {
        s_ctlr[spi_hw].algo = NULL;
        return NULL;
    }

    return &s_ctlr[spi_hw];
}
