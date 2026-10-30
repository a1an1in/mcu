/*
 * stm32f1_i2c.h - STM32F1 I2C 芯片实现的初始化入口 <drivers/i2c/>
 *
 * 为什么单独放一个芯片名后缀的头：
 *   i2c.h 是芯片无关的公共接口，不应出现 stm32f1_ 前缀的符号；
 *   而 BSP 需要调用「建一条 STM32F1 的 I2C 总线」，所以把这个入口单独声明。
 *   换成别的芯片时，只需换成 xxxx_i2c.h，i2c.h 与上层代码不动。
 */
#ifndef DRIVERS_I2C_STM32F1_I2C_H__
#define DRIVERS_I2C_STM32F1_I2C_H__

#include <stdint.h>

#include "i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化 STM32F1 的 bit-bang I2C 总线并注册为 adapter 0（名字 "i2c0"）。
 *
 * hclk_hz：AHB 时钟频率，用于 DWT 微秒延时。由 BSP 传 rcc_ahb_hz()，
 *          驱动内部不写死主频，换 HSE/PLL 也不用改驱动。
 *
 * 返回已注册的 adapter；重复调用是安全的（返回同一个实例）。
 */
struct i2c_adapter *stm32f1_i2c_gpio_init(uint32_t hclk_hz);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_I2C_STM32F1_I2C_H__ */
