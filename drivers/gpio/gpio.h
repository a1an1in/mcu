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

/* 输入模式的上/下拉选择。
 * 名字刻意避开 GPIO_PULL_* —— 那会和 Zephyr 的 GPIO_PULL_UP/GPIO_PULL_DOWN 宏撞名，
 * 而 zephyr_gpio.c 需要同时包含两边的头文件。 */
typedef enum {
    GPIO_IN_FLOAT = 0,    /* 浮空输入 */
    GPIO_IN_PULLUP,       /* 上拉输入（按键接地型：按下读到低） */
    GPIO_IN_PULLDOWN,     /* 下拉输入（按键接 3.3V 型：按下读到高） */
} gpio_pull_t;

/* 将引脚配置为输入模式，并选择内部上/下拉（按键、跳线等输入场景） */
void gpio_input_init(gpio_pin_enc_t pin, gpio_pull_t pull);

/* 输出电平：val 非 0 输出高，0 输出低 */
void gpio_write(gpio_pin_enc_t pin, int val);

/* 读取引脚输入电平，返回非 0 为高 */
int  gpio_read(gpio_pin_enc_t pin);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_GPIO_H__ */