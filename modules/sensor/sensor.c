/*
 * sensor.c - 温度采样业务 <modules/sensor/>
 *
 * 只调 drivers/adc 的接口：启动定时器触发 + 循环 DMA，然后对最近一批
 * 样本求平均、换算成温度。不碰任何寄存器。
 */
#include <stdint.h>
#include <stddef.h>

#include "adc.h"
#include "sensor.h"

#define SENSOR_DEPTH            32u
#define SENSOR_DEFAULT_RATE     1000u

#define SENSOR_VREF_MV          3300u   /* 参考电压 3.3V */
#define SENSOR_V25_MV           1430    /* 25°C 时的传感器电压 */
#define SENSOR_SLOPE_MV10       43      /* 4.3mV/°C = 43/10 */

/* DMA 会直接写这块内存，软件从不写它 —— 必须 volatile，
 * 否则 -O2 下 sum += s_buf[i] 会被优化成常量 0。 */
static volatile uint16_t s_buf[SENSOR_DEPTH];
static uint32_t s_rate;
static int s_offset_x10;

static struct adc_dev *sensor_adc(void)
{
    return adc_get_default();
}

int sensor_start(uint32_t rate_hz)
{
    struct adc_dev *dev = sensor_adc();

    if (dev == NULL)
    {
        return -1;
    }
    if (rate_hz == 0u)
    {
        rate_hz = SENSOR_DEFAULT_RATE;
    }
    if (adc_start(dev, rate_hz, (uint16_t *)s_buf, SENSOR_DEPTH) != ADC_OK)
    {
        return -1;
    }
    s_rate = rate_hz;
    return 0;
}

int sensor_stop(void)
{
    struct adc_dev *dev = sensor_adc();

    s_rate = 0u;
    return (dev != NULL) ? ((adc_stop(dev) == ADC_OK) ? 0 : -1) : -1;
}

int sensor_is_running(void)
{
    return adc_is_running(sensor_adc());
}

uint32_t sensor_rate(void)
{
    return s_rate;
}

/* 最近一批样本的平均原始值 */
int sensor_read_raw(uint16_t *raw)
{
    uint32_t sum = 0u;
    uint32_t i;

    if ((raw == NULL) || (s_rate == 0u))
    {
        return -1;
    }
    for (i = 0u; i < SENSOR_DEPTH; i++)
    {
        sum += s_buf[i];
    }
    *raw = (uint16_t)(sum / SENSOR_DEPTH);
    return 0;
}

int sensor_read_temp_x10(int *tenths)
{
    uint16_t raw;
    uint32_t mv;

    if (tenths == NULL)
    {
        return -1;
    }
    if (sensor_read_raw(&raw) != 0)
    {
        return -1;
    }

    /* T = 25 + (V25 - Vsense) / 4.3mV，结果换成 0.1°C */
    mv = ((uint32_t)raw * SENSOR_VREF_MV) / 4096u;
    *tenths = 250 + ((SENSOR_V25_MV - (int)mv) * 100) / SENSOR_SLOPE_MV10
              + s_offset_x10;
    return 0;
}

int sensor_calibrate(int actual_x10)
{
    int now = 0;

    if (sensor_read_temp_x10(&now) != 0)
    {
        return -1;
    }
    s_offset_x10 += actual_x10 - now;
    return 0;
}

int sensor_offset_x10(void)
{
    return s_offset_x10;
}
