/*
 * sensor.h - 温度采样业务 <modules/sensor/>
 *
 * 只依赖 drivers/adc 的接口，不认识 ADC 寄存器：
 *   启动定时器触发 + DMA → 读最近一批样本 → 换算成 0.1°C
 *
 * 内部温度传感器：V25=1.43V，斜率 4.3mV/°C，参考 3.3V / 12bit。
 * 精度约 ±1.5°C，噪声大，靠多次平均（默认 32 点）。
 */
#ifndef MODULES_SENSOR_SENSOR_H__
#define MODULES_SENSOR_SENSOR_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* rate_hz：定时器触发频率；0 = 默认 1000Hz */
int sensor_start(uint32_t rate_hz);
int sensor_stop(void);
int sensor_is_running(void);
uint32_t sensor_rate(void);

/* 返回 0.1°C（例如 284 = 28.4°C），成功返回 0 */
int sensor_read_temp_x10(int *tenths);

/* 最近一批样本的平均原始值（0~4095） */
int sensor_read_raw(uint16_t *raw);

/* 单点校准：告诉模块当前真实温度（0.1°C），之后显示值加上这个偏移。
 * 内部温度传感器个体差异很大，不校准可能差几十度。 */
int sensor_calibrate(int actual_x10);
int sensor_offset_x10(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_SENSOR_SENSOR_H__ */
