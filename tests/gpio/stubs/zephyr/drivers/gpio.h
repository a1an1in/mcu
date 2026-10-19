/*
 * zephyr/drivers/gpio.h - Zephyr <zephyr/drivers/gpio.h> 的 host 测试桩。
 * 仅提供 zephyr_gpio.c 所需类型 / 宏 / 原型。include 名与真实 Zephyr 一致。
 */
#ifndef STUB_ZEPHYR_DRIVERS_GPIO_H__
#define STUB_ZEPHYR_DRIVERS_GPIO_H__

#include <stdint.h>

#include <zephyr/device.h>

typedef uint8_t  gpio_pin_t;
typedef uint32_t gpio_flags_t;

/* 与 Zephyr include/zephyr/drivers/gpio.h 的取值保持一致 */
#define GPIO_PULL_UP   (1u << 4)
#define GPIO_PULL_DOWN (1u << 5)
#define GPIO_INPUT     (1u << 16)
#define GPIO_OUTPUT    (1u << 17)

/* 5 个 GPIO 端口的桩设备对象（由 zephyr_gpio_stub.c 定义） */
extern const struct device __gpio_dev_gpioa;
extern const struct device __gpio_dev_gpiob;
extern const struct device __gpio_dev_gpioc;
extern const struct device __gpio_dev_gpiod;
extern const struct device __gpio_dev_gpioe;

int gpio_pin_configure(const struct device *port, gpio_pin_t pin, gpio_flags_t flags);
int gpio_pin_set(const struct device *port, gpio_pin_t pin, int value);
int gpio_pin_get(const struct device *port, gpio_pin_t pin);

#endif /* STUB_ZEPHYR_DRIVERS_GPIO_H__ */