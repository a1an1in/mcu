/*
 * gpio.h - 通用 GPIO 驱动接口（与具体 MCU 无关） <drivers/gpio/>
 *
 * 本组件自包含接口与实现：接口见本文件；实现见同目录 stm32f1_gpio.c
 * （针对 STM32F1 使用直接寄存器操作，避免引入完整 HAL 库）。
 * 若未来使用其它 MCU，仅需在组件内增加对应芯片实现，接口保持不变。
 */
#ifndef DRIVERS_GPIO_H__
#define DRIVERS_GPIO_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GPIO_PORT_A = 0,
    GPIO_PORT_B,
    GPIO_PORT_C,
    GPIO_PORT_D,
    GPIO_PORT_E,
} gpio_port_t;

/* 引脚编码：高 8 位端口，低 8 位引脚号 */
typedef uint32_t gpio_pin_enc_t;

#define GPIO_PIN(port, pin)   ((((uint32_t)(port)) << 8) | ((pin) & 0xFFu))

/* 复位 / 使能各 GPIO 端口时钟（通常由板级或模块在首次使用前调用） */
void gpio_init(void);

/* 将引脚配置为推挽输出 */
void gpio_output_init(gpio_pin_enc_t pin);

/* 输出电平：val 非 0 输出高，0 输出低 */
void gpio_write(gpio_pin_enc_t pin, int val);

/* 读取引脚输入电平，返回非 0 为高 */
int  gpio_read(gpio_pin_enc_t pin);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_GPIO_H__ */