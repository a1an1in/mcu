/*
 * adc_config.h - 本板 ADC 事实 <drivers/adc/>
 *
 * 战舰 V4：内部温度传感器接在 ADC1_IN16，适合做低频监测；
 * 需要看“采样率真的在跑”时可以加 PA1（ADC1_IN1，板载电位器）做第二路。
 */
#ifndef DRIVERS_ADC_CONFIG_H__
#define DRIVERS_ADC_CONFIG_H__

#include "adc.h"

/* 内部温度传感器：数据手册要求采样时间 ≥ 17.1µs（239.5 周期 @12MHz） */
#define ADC_TEMP_CHANNEL        16u
#define ADC_TEMP_SAMPLE_TIME    ADC_SAMPLE_239_5

/* 默认：1kHz 触发，32 点循环（32ms 窗口） */
#define ADC_DEFAULT_RATE_HZ     1000u
#define ADC_DEFAULT_DEPTH       32u

#endif /* DRIVERS_ADC_CONFIG_H__ */
