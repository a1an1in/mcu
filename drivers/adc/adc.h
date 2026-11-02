/*
 * adc.h - 通用 ADC 采样接口（定时器触发 + DMA） <drivers/adc/>
 *
 * 分层（与其它驱动一致）：
 *   adc.h          接口，芯片 / OS 无关
 *   adc_config.h   板级事实：用哪个通道、采样时间、默认采样率
 *   stm32f1_adc.c  芯片实现：ADC1 + TIMx TRGO + DMA1
 *
 * 工作方式：
 *   定时器按 rate_hz 产生 TRGO → ADC 每次触发转换一个通道 → DMA 把结果
 *   搬进调用方的缓冲区。DMA 用循环模式，缓冲区里始终是“最近 depth 个样本”，
 *   业务层随时读，不需要中断。
 *
 * 为什么用循环 DMA：
 *   - 采样率由硬件决定，CPU 不参与；
 *   - 读的时候可能有撕裂（读一半时 DMA 写了另一半），对温度这种慢变量
 *     影响可以忽略；要严格一致可以停一下再读。
 *
 * 典型用法：
 *     adc_bind(&adc, channels, 1, ADC_SAMPLE_239_5);
 *     adc_start(&adc, 1000, buf, 32);     // 1kHz，32 点循环
 *     ... 随时读 buf ...
 *     adc_stop(&adc);
 */
#ifndef DRIVERS_ADC_H__
#define DRIVERS_ADC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_OK              0
#define ADC_ERR_PARAM       (-1)
#define ADC_ERR_NODEV       (-2)
#define ADC_ERR_IO          (-3)

/* 采样时间档位（对应 STM32F1 的 SMPx：1.5 ~ 239.5 个 ADC 周期） */
#define ADC_SAMPLE_1_5      0u
#define ADC_SAMPLE_7_5      1u
#define ADC_SAMPLE_13_5     2u
#define ADC_SAMPLE_28_5     3u
#define ADC_SAMPLE_41_5     4u
#define ADC_SAMPLE_55_5     5u
#define ADC_SAMPLE_71_5     6u
#define ADC_SAMPLE_239_5    7u

#define ADC_MAX_CHANNELS    4u

struct adc_dev {
    const uint8_t *channels;    /* 通道号列表：0~9 对应 PA0~PA7/PB0~PB1，16 = 内部温度 */
    uint8_t nch;
    uint8_t sample_time;
    uint16_t *buf;              /* DMA 目标：depth × nch 个 16 位样本 */
    uint16_t depth;
    uint32_t rate_hz;
    uint8_t bound;
    uint8_t running;
};

/* 绑定通道列表（只保存指针，数组须长期有效） */
int adc_bind(struct adc_dev *dev, const uint8_t *channels, uint8_t nch,
             uint8_t sample_time);

/* 启动：定时器按 rate_hz 触发，DMA 循环写入 buf（depth × nch 个样本） */
int adc_start(struct adc_dev *dev, uint32_t rate_hz, uint16_t *buf, uint16_t depth);

int adc_stop(struct adc_dev *dev);
int adc_is_running(struct adc_dev *dev);

/* 板级注册的默认实例，业务模块用 adc_get_default() 取 */
void adc_set_default(struct adc_dev *dev);
struct adc_dev *adc_get_default(void);

/* 调试：抓一份关键寄存器快照（排查“一直是 0”时用） */
struct adc_regs {
    uint32_t rcc_cfgr;
    uint32_t rcc_apb2enr;
    uint32_t rcc_apb1enr;
    uint32_t rcc_ahbenr;
    uint32_t adc_cr1;
    uint32_t adc_cr2;
    uint32_t adc_sr;
    uint32_t adc_sqr1;
    uint32_t adc_sqr3;
    uint32_t adc_smpr2;
    uint32_t adc_dr;
    uint32_t tim_cr1;
    uint32_t tim_cr2;
    uint32_t tim_cnt;
    uint32_t tim_psc;
    uint32_t tim_arr;
    uint32_t dma_ccr;
    uint32_t dma_cndtr;
    uint32_t dma_cpar;
    uint32_t dma_cmar;
    uint16_t sample[4];
};

void adc_debug_regs(struct adc_regs *out);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_ADC_H__ */
