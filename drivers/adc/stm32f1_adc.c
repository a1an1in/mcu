/*
 * stm32f1_adc.c - STM32F1 ADC1 + 定时器 TRGO + DMA 实现 <drivers/adc/>
 *
 * 采样链路：
 *   TIM3 更新事件（TRGO） → ADC1 外部触发 → 规则组转换 → DMA1_Channel1 → 用户缓冲
 *
 * 为什么选 TIM3：
 *   通用定时器，APB1 上但定时器时钟 = 72MHz；只用它的 TRGO，不占引脚。
 *   基本定时器 TIM6/7 也能做，但 TIM3 更通用，和书里第 21 章一致。
 *
 * DMA 用循环模式：缓冲区里始终是最近 depth×nch 个样本，业务层随时读，
 * 不需要中断，也不会因为没及时取走而停采。
 */
#include <stdint.h>
#include <stddef.h>

#include "adc.h"

/* ---------------- 寄存器 ---------------- */
#define RCC_BASE            (0x40021000u)
#define RCC_CFGR            (*(volatile uint32_t *)(RCC_BASE + 0x04u))
#define RCC_AHBENR          (*(volatile uint32_t *)(RCC_BASE + 0x14u))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x18u))
#define RCC_APB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x1Cu))

#define GPIOA_BASE          (0x40010800u)
#define GPIOB_BASE          (0x40010C00u)
#define GPIO_CRL(b)         (*(volatile uint32_t *)((b) + 0x00u))
#define GPIO_CRH(b)         (*(volatile uint32_t *)((b) + 0x04u))

#define ADC1_BASE           (0x40012400u)
#define ADC1_SR             (*(volatile uint32_t *)(ADC1_BASE + 0x00u))
#define ADC1_CR1            (*(volatile uint32_t *)(ADC1_BASE + 0x04u))
#define ADC1_CR2            (*(volatile uint32_t *)(ADC1_BASE + 0x08u))
#define ADC1_SMPR1          (*(volatile uint32_t *)(ADC1_BASE + 0x0Cu))
#define ADC1_SMPR2          (*(volatile uint32_t *)(ADC1_BASE + 0x10u))
#define ADC1_SQR1           (*(volatile uint32_t *)(ADC1_BASE + 0x2Cu))
#define ADC1_SQR2           (*(volatile uint32_t *)(ADC1_BASE + 0x30u))
#define ADC1_SQR3           (*(volatile uint32_t *)(ADC1_BASE + 0x34u))
#define ADC1_DR             (*(volatile uint32_t *)(ADC1_BASE + 0x4Cu))

#define DMA1_BASE           (0x40020000u)
#define DMA1_IFCR           (*(volatile uint32_t *)(DMA1_BASE + 0x04u))
#define DMA1_CCR1           (*(volatile uint32_t *)(DMA1_BASE + 0x08u))
#define DMA1_CNDTR1         (*(volatile uint32_t *)(DMA1_BASE + 0x0Cu))
#define DMA1_CPAR1          (*(volatile uint32_t *)(DMA1_BASE + 0x10u))
#define DMA1_CMAR1          (*(volatile uint32_t *)(DMA1_BASE + 0x14u))

#define TIM3_BASE           (0x40000400u)
#define TIM3_CR1            (*(volatile uint32_t *)(TIM3_BASE + 0x00u))
#define TIM3_CR2            (*(volatile uint32_t *)(TIM3_BASE + 0x04u))
#define TIM3_SR             (*(volatile uint32_t *)(TIM3_BASE + 0x10u))
#define TIM3_EGR            (*(volatile uint32_t *)(TIM3_BASE + 0x14u))
#define TIM3_PSC            (*(volatile uint32_t *)(TIM3_BASE + 0x28u))
#define TIM3_ARR            (*(volatile uint32_t *)(TIM3_BASE + 0x2Cu))

#define RCC_APB2ENR_ADC1EN  (1u << 9)
#define RCC_APB2ENR_IOPAEN  (1u << 2)
#define RCC_APB2ENR_IOPBEN  (1u << 3)
#define RCC_AHBENR_DMA1EN   (1u << 0)
#define RCC_APB1ENR_TIM3EN  (1u << 1)

#define ADC_CR2_ADON        (1u << 0)
#define ADC_CR2_CONT        (1u << 1)
#define ADC_CR2_CAL         (1u << 2)
#define ADC_CR2_RSTCAL      (1u << 3)
#define ADC_CR2_DMA         (1u << 8)
#define ADC_CR2_EXTTRIG     (1u << 20)
#define ADC_CR2_TSVREFE     (1u << 23)
#define ADC_CR2_EXTSEL_TIM3 (4u << 17)     /* TIM3_TRGO */

#define ADC_CR1_SCAN        (1u << 8)

#define DMA_CCR_EN          (1u << 0)
/* DIR=0：外设 → 内存（1 是内存 → 外设，别写反） */
#define DMA_CCR_CIRC        (1u << 5)
#define DMA_CCR_MINC        (1u << 7)
#define DMA_CCR_PSIZE_16    (1u << 8)
#define DMA_CCR_MSIZE_16    (1u << 10)

/* 定时器时钟：APB1=36MHz，定时器 x2 = 72MHz */
#define TIM_CLK_HZ          72000000u
#define TIM_BASE_HZ         1000000u       /* PSC 分到 1MHz，ARR 直接算周期 */

#define ADC_CLK_HZ          12000000u      /* PCLK2/6 */

static const uint16_t s_sample_cycles_x2[8] =
{
    3u, 15u, 27u, 57u, 83u, 111u, 143u, 479u   /* 1.5 ~ 239.5 周期 ×2 */
};

static uint8_t s_channels[ADC_MAX_CHANNELS];
static struct adc_dev *s_default;

static void adc_delay(volatile uint32_t n)
{
    while (n-- != 0u)
    {
        __asm volatile ("nop");
    }
}

/* 通道 → 端口/引脚：0~7 = PA0~PA7，8~9 = PB0~PB1；16/17 是内部通道 */
static void adc_pin_analog(uint8_t ch)
{
    uint32_t base;
    uint32_t shift;
    uint32_t v;

    if (ch <= 7u)
    {
        base = GPIOA_BASE;
        shift = (uint32_t)ch * 4u;
    }
    else if (ch <= 9u)
    {
        base = GPIOB_BASE;
        shift = ((uint32_t)ch - 8u) * 4u;
    }
    else
    {
        return;                 /* 内部温度 / Vref，不需要引脚 */
    }

    if (shift < 32u)
    {
        v = GPIO_CRL(base);
        v &= ~(0xFu << shift);
        GPIO_CRL(base) = v;     /* MODE=00, CNF=00 → 模拟输入 */
    }
    else
    {
        shift -= 32u;
        v = GPIO_CRH(base);
        v &= ~(0xFu << shift);
        GPIO_CRH(base) = v;
    }
}

static void adc_hw_off(void)
{
    TIM3_CR1 = 0u;
    DMA1_CCR1 = 0u;
    ADC1_CR2 &= ~(ADC_CR2_EXTTRIG);
}

static void adc_config(uint8_t nch, uint8_t sample_time)
{
    uint32_t smp;
    uint32_t smpr1 = 0u;
    uint32_t smpr2 = 0u;
    uint8_t i;

    ADC1_CR1 = ADC_CR1_SCAN;
    ADC1_CR2 = ADC_CR2_TSVREFE;        /* 打开温度传感器 */

    for (i = 0; i < nch; i++)
    {
        uint8_t ch = s_channels[i];

        smp = (uint32_t)(sample_time & 0x7u) << ((ch % 10u) * 3u);
        if (ch < 10u)
        {
            smpr1 |= smp;
        }
        else
        {
            smpr2 |= smp;
        }
    }
    ADC1_SMPR1 = smpr1;
    ADC1_SMPR2 = smpr2;

    ADC1_SQR1 = (uint32_t)(nch - 1u) << 20;
    ADC1_SQR3 = (uint32_t)s_channels[0];
    if (nch > 1u) ADC1_SQR3 |= (uint32_t)s_channels[1] << 5;
    if (nch > 2u) ADC1_SQR3 |= (uint32_t)s_channels[2] << 10;
    if (nch > 3u) ADC1_SQR3 |= (uint32_t)s_channels[3] << 15;
    ADC1_SQR2 = 0u;

    /* 上电 + 校准（校准要求 ADON=1 且不触发转换） */
    ADC1_CR2 |= ADC_CR2_ADON;
    adc_delay(2000u);

    ADC1_CR2 |= ADC_CR2_RSTCAL;
    while ((ADC1_CR2 & ADC_CR2_RSTCAL) != 0u)
    {
    }
    ADC1_CR2 |= ADC_CR2_CAL;
    while ((ADC1_CR2 & ADC_CR2_CAL) != 0u)
    {
    }
}

int adc_bind(struct adc_dev *dev, const uint8_t *channels, uint8_t nch,
             uint8_t sample_time)
{
    uint8_t i;

    if ((dev == NULL) || (channels == NULL) || (nch == 0u) ||
        (nch > ADC_MAX_CHANNELS) || (sample_time > ADC_SAMPLE_239_5))
    {
        return ADC_ERR_PARAM;
    }

    for (i = 0; i < nch; i++)
    {
        if (channels[i] > 17u)
        {
            return ADC_ERR_PARAM;
        }
        s_channels[i] = channels[i];
    }

    dev->channels = s_channels;
    dev->nch = nch;
    dev->sample_time = sample_time;
    dev->bound = 1u;
    dev->running = 0u;
    return ADC_OK;
}

int adc_start(struct adc_dev *dev, uint32_t rate_hz, uint16_t *buf, uint16_t depth)
{
    uint32_t max_rate;
    uint32_t arr;

    if ((dev == NULL) || (buf == NULL) || (depth == 0u))
    {
        return ADC_ERR_PARAM;
    }
    if (dev->bound == 0u)
    {
        return ADC_ERR_NODEV;
    }

    /* 每秒最多能转换多少次：ADC 时钟 / (采样 + 12.5 个转换周期)
     * 用「半周期」做整数运算，避免浮点 */
    max_rate = (ADC_CLK_HZ * 2u) /
               ((uint32_t)s_sample_cycles_x2[dev->sample_time & 0x7u] + 25u);
    if ((rate_hz < 16u) || (rate_hz > max_rate))
    {
        return ADC_ERR_PARAM;
    }

    RCC_APB2ENR |= RCC_APB2ENR_ADC1EN;
    RCC_AHBENR |= RCC_AHBENR_DMA1EN;
    RCC_APB1ENR |= RCC_APB1ENR_TIM3EN;

    adc_hw_off();

    /* ADCCLK = PCLK2 / 6 = 12MHz（≤14MHz） */
    RCC_CFGR = (RCC_CFGR & ~(3u << 14)) | (2u << 14);

    {
        uint8_t i;

        for (i = 0; i < dev->nch; i++)
        {
            adc_pin_analog(s_channels[i]);
        }
    }

    s_default = dev;
    dev->buf = buf;
    dev->depth = depth;
    dev->rate_hz = rate_hz;

    adc_config(dev->nch, dev->sample_time);

    /* DMA1_Channel1：ADC1_DR → buf，16 位，循环 */
    DMA1_IFCR = 0x0Fu;
    DMA1_CPAR1 = (uint32_t)(uintptr_t)&ADC1_DR;
    DMA1_CMAR1 = (uint32_t)(uintptr_t)buf;
    DMA1_CNDTR1 = (uint32_t)depth * dev->nch;
    DMA1_CCR1 = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PSIZE_16 |
                DMA_CCR_MSIZE_16 | DMA_CCR_EN;

    ADC1_CR2 |= ADC_CR2_DMA | ADC_CR2_EXTTRIG | ADC_CR2_EXTSEL_TIM3;

    /* TIM3：1MHz 计数，ARR 决定触发频率；MMS=010（更新事件做 TRGO） */
    arr = (TIM_BASE_HZ / rate_hz) - 1u;
    TIM3_PSC = (TIM_CLK_HZ / TIM_BASE_HZ) - 1u;
    TIM3_ARR = arr;
    TIM3_CR2 = (2u << 4);
    TIM3_EGR = 1u;
    TIM3_SR = 0u;
    TIM3_CR1 = 1u;

    dev->running = 1u;
    return ADC_OK;
}

int adc_stop(struct adc_dev *dev)
{
    if (dev == NULL)
    {
        return ADC_ERR_PARAM;
    }

    adc_hw_off();
    dev->running = 0u;
    return ADC_OK;
}

int adc_is_running(struct adc_dev *dev)
{
    return ((dev != NULL) && (dev->running != 0u)) ? 1 : 0;
}

void adc_set_default(struct adc_dev *dev)
{
    s_default = dev;
}

struct adc_dev *adc_get_default(void)
{
    return s_default;
}

void adc_debug_regs(struct adc_regs *out)
{
    if (out == NULL)
    {
        return;
    }

    out->rcc_cfgr = RCC_CFGR;
    out->rcc_apb2enr = RCC_APB2ENR;
    out->rcc_apb1enr = RCC_APB1ENR;
    out->rcc_ahbenr = RCC_AHBENR;

    out->adc_cr1 = ADC1_CR1;
    out->adc_cr2 = ADC1_CR2;
    out->adc_sr = ADC1_SR;
    out->adc_sqr1 = ADC1_SQR1;
    out->adc_sqr3 = ADC1_SQR3;
    out->adc_smpr2 = ADC1_SMPR2;

    out->tim_cr1 = TIM3_CR1;
    out->tim_cr2 = TIM3_CR2;
    out->tim_cnt = *(volatile uint32_t *)(TIM3_BASE + 0x24u);
    out->tim_psc = TIM3_PSC;
    out->tim_arr = TIM3_ARR;

    out->dma_ccr = DMA1_CCR1;
    out->dma_cndtr = DMA1_CNDTR1;
    out->dma_cpar = DMA1_CPAR1;
    out->dma_cmar = DMA1_CMAR1;
    out->adc_dr = ADC1_DR;              /* 读 DR 会清 EOC，仅调试用 */

    if ((s_default != NULL) && (s_default->buf != NULL))
    {
        out->sample[0] = s_default->buf[0];
        out->sample[1] = s_default->buf[1];
        out->sample[2] = s_default->buf[2];
        out->sample[3] = s_default->buf[3];
    }
    else
    {
        out->sample[0] = out->sample[1] = out->sample[2] = out->sample[3] = 0u;
    }
}
