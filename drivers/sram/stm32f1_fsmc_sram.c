/*
 * stm32f1_fsmc_sram.c - FSMC 并口 SRAM 实现 <drivers/sram/>
 *
 * 战舰 V4 用 IS62WV51216（1MB，16 位总线），FSMC Bank1 NE3：
 *   基地址 0x68000000，A0~A18 寻址，读写就是普通内存访问。
 *
 * 引脚（F103 FSMC Bank1，16 位）：
 *   D0~D15 : PD14 PD15 PD0 PD1 PE7~PE15 PD8 PD9 PD10
 *   A0~A18 : PF0~PF5 PF12~PF15 PG0~PG5 PD11 PD12 PD13
 *   NBL0/1 : PE0 PE1
 *   NOE/NWE: PD4 PD5
 *   NE3    : PG10
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "sram.h"
#include "sram_config.h"

#define RCC_BASE            (0x40021000u)
#define RCC_AHBENR          (*(volatile uint32_t *)(RCC_BASE + 0x14u))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x18u))

#define GPIOA_BASE          (0x40010800u)
#define GPIOB_BASE          (0x40010C00u)
#define GPIOC_BASE          (0x40011000u)
#define GPIOD_BASE          (0x40011400u)
#define GPIOE_BASE          (0x40011800u)
#define GPIOF_BASE          (0x40011C00u)
#define GPIOG_BASE          (0x40012000u)

#define FSMC_BASE           (0xA0000000u)
#define FSMC_BCR3           (*(volatile uint32_t *)(FSMC_BASE + 0x10u))
#define FSMC_BTR3           (*(volatile uint32_t *)(FSMC_BASE + 0x14u))

#define RCC_AHBENR_FSMC     (1u << 8)
#define RCC_APB2ENR_IOPD    (1u << 5)
#define RCC_APB2ENR_IOPE    (1u << 6)
#define RCC_APB2ENR_IOPF    (1u << 7)
#define RCC_APB2ENR_IOPG    (1u << 8)

static struct sram_dev *s_default;

/* 复用推挽输出，50MHz：MODE=11, CNF=10 → 0xB */
static void pin_af(uint32_t port, uint8_t pin)
{
    volatile uint32_t *reg;
    uint32_t shift;

    if (pin < 8u)
    {
        reg = (volatile uint32_t *)(port + 0x00u);
        shift = (uint32_t)pin * 4u;
    }
    else
    {
        reg = (volatile uint32_t *)(port + 0x04u);
        shift = ((uint32_t)pin - 8u) * 4u;
    }

    *reg = (*reg & ~(0xFu << shift)) | (0xBu << shift);
}

static void sram_pins_init(void)
{
    static const uint8_t pd[] = { 0, 1, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t pe[] = { 0, 1, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t pf[] = { 0, 1, 2, 3, 4, 5, 12, 13, 14, 15 };
    static const uint8_t pg[] = { 0, 1, 2, 3, 4, 5, 10 };
    uint32_t i;

    for (i = 0; i < sizeof(pd); i++)
    {
        pin_af(GPIOD_BASE, pd[i]);
    }
    for (i = 0; i < sizeof(pe); i++)
    {
        pin_af(GPIOE_BASE, pe[i]);
    }
    for (i = 0; i < sizeof(pf); i++)
    {
        pin_af(GPIOF_BASE, pf[i]);
    }
    for (i = 0; i < sizeof(pg); i++)
    {
        pin_af(GPIOG_BASE, pg[i]);
    }
}

int sram_bind(struct sram_dev *dev, uint32_t base, uint32_t size)
{
    if ((dev == NULL) || (base == 0u) || (size < 1024u))
    {
        return SRAM_ERR_PARAM;
    }

    dev->base = base;
    dev->size = size;
    dev->ready = 0u;
    return SRAM_OK;
}

int sram_init(struct sram_dev *dev)
{
    if (dev == NULL)
    {
        return SRAM_ERR_PARAM;
    }

    RCC_AHBENR |= RCC_AHBENR_FSMC;
    RCC_APB2ENR |= RCC_APB2ENR_IOPD | RCC_APB2ENR_IOPE |
                   RCC_APB2ENR_IOPF | RCC_APB2ENR_IOPG;

    sram_pins_init();

    /* Bank1 NE3：SRAM、16 位、写使能、异步模式 A */
    FSMC_BCR3 = SRAM_FSMC_BCR;
    FSMC_BTR3 = SRAM_FSMC_BTR;

    dev->ready = 1u;
    return SRAM_OK;
}

int sram_selftest(struct sram_dev *dev)
{
    volatile uint16_t *p;
    uint32_t i;

    if ((dev == NULL) || (dev->ready == 0u))
    {
        return SRAM_ERR_NODEV;
    }

    p = (volatile uint16_t *)dev->base;

    /* 低地址 128 个半字 + 末尾 128 个半字，覆盖地址线 */
    for (i = 0; i < 128u; i++)
    {
        p[i] = (uint16_t)(0xA5A5u ^ (uint16_t)i);
    }
    for (i = 0; i < 128u; i++)
    {
        volatile uint16_t *q = (volatile uint16_t *)(dev->base + dev->size - 256u);

        q[i] = (uint16_t)(0x5A5Au ^ (uint16_t)i);
    }

    for (i = 0; i < 128u; i++)
    {
        if (p[i] != (uint16_t)(0xA5A5u ^ (uint16_t)i))
        {
            return SRAM_ERR_VERIFY;
        }
    }
    for (i = 0; i < 128u; i++)
    {
        volatile uint16_t *q = (volatile uint16_t *)(dev->base + dev->size - 256u);

        if (q[i] != (uint16_t)(0x5A5Au ^ (uint16_t)i))
        {
            return SRAM_ERR_VERIFY;
        }
    }

    return SRAM_OK;
}

void sram_write(struct sram_dev *dev, uint32_t off, const void *src, uint32_t len)
{
    if ((dev == NULL) || (src == NULL) || (off + len > dev->size))
    {
        return;
    }
    memcpy((void *)(dev->base + off), src, len);
}

void sram_read(struct sram_dev *dev, uint32_t off, void *dst, uint32_t len)
{
    if ((dev == NULL) || (dst == NULL) || (off + len > dev->size))
    {
        return;
    }
    memcpy(dst, (const void *)(dev->base + off), len);
}

void sram_set_default(struct sram_dev *dev)
{
    s_default = dev;
}

struct sram_dev *sram_get_default(void)
{
    return s_default;
}
