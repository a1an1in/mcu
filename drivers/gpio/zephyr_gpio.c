/*
 * zephyr_gpio.c - GPIO 接口的 Zephyr 实现（底层使用 Zephyr 自带 GPIO 驱动）
 *
 * 本实现把通用接口（gpio.h）转发到 Zephyr 的 GPIO API（gpio_pin_configure/
 * gpio_pin_set/gpio_pin_get），设备句柄通过 device-tree 节点 label（gpioa..e）
 * 获得，与 Zephyr 官方 STM32F1 SoC 定义一致。
 *
 * 前提：使用 Zephyr 构建体系（west/zephyr.cmake），且在板级 dts 中存在
 *       gpioa/gpiob/gpioc/gpiod/gpioe 节点 label（STM32F1 SoC 默认提供）。
 */
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <stddef.h>   /* NULL */

#include "gpio.h"

/* 每个 GPIO 端口在 Zephyr 中是独立设备，端口名 -> 设备句柄 */
static const struct device *to_dev(gpio_pin_enc_t pin)
{
    uint32_t port = (pin >> 8) & 0xFFu;

    switch (port)
    {
    case GPIO_PORT_A: return DEVICE_DT_GET(DT_NODELABEL(gpioa));
    case GPIO_PORT_B: return DEVICE_DT_GET(DT_NODELABEL(gpiob));
    case GPIO_PORT_C: return DEVICE_DT_GET(DT_NODELABEL(gpioc));
    case GPIO_PORT_D: return DEVICE_DT_GET(DT_NODELABEL(gpiod));
    case GPIO_PORT_E: return DEVICE_DT_GET(DT_NODELABEL(gpioe));
    default:          return NULL;
    }
}

void gpio_init(void)
{
    /* Zephyr 设备在 init 阶段已使能时钟，无需额外动作 */
}

void gpio_output_init(gpio_pin_enc_t pin)
{
    const struct device *dev = to_dev(pin);

    if (dev != NULL)
        gpio_pin_configure(dev, (gpio_pin_t)(pin & 0xFFu), GPIO_OUTPUT);
}

void gpio_write(gpio_pin_enc_t pin, int val)
{
    const struct device *dev = to_dev(pin);

    if (dev != NULL)
        gpio_pin_set(dev, (gpio_pin_t)(pin & 0xFFu), val ? 1 : 0);
}

int gpio_read(gpio_pin_enc_t pin)
{
    const struct device *dev = to_dev(pin);

    if (dev == NULL)
        return 0;

    return gpio_pin_get(dev, (gpio_pin_t)(pin & 0xFFu)) ? 1 : 0;
}