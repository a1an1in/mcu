/*
 * rtthread_gpio.c - GPIO 接口的 RT-Thread 实现（底层使用 RT-Thread 自带 PIN/GPIO 驱动）
 *
 * 本实现把通用接口（gpio.h）转发到 RT-Thread 的引脚框架（rt_pin_* 系列，
 * 由官方驱动 drv_gpio.c 注册到底层 ST 引脚操作）。
 *
 * 前提：RT-Thread 需启用 PIN 设备驱动并完成注册（rtconfig.h 打开
 *       RT_USING_DEVICE / RT_USING_PIN，并编译 HAL_Drivers/drv_gpio.c）。
 *
 * 引脚编号约定：本工程引脚编码为 (端口<<8 | 引脚)，RT-Thread/STM32 的引脚
 *       编号为线性序号 port*16 + pin（与 drv_gpio.h 的 GET_PIN 一致）。
 */
#include <drivers/pin.h>

#include "gpio.h"

/* 通用引脚编码 (port<<8|pin) -> RT-Thread 线性引脚号 (port*16+pin) */
static rt_base_t to_rt_pin(gpio_pin_enc_t pin)
{
    uint32_t port = (pin >> 8) & 0xFFu;
    uint32_t p    = pin & 0xFFu;

    if (port > 4u)
        port = 0u;

    return (rt_base_t)(port * 16u + p);
}

void gpio_init(void)
{
    /* RT-Thread PIN 框架在驱动注册时已使能端口时钟，无需额外动作 */
}

void gpio_output_init(gpio_pin_enc_t pin)
{
    rt_pin_mode(to_rt_pin(pin), PIN_MODE_OUTPUT);
}

void gpio_write(gpio_pin_enc_t pin, int val)
{
    rt_pin_write(to_rt_pin(pin), val ? PIN_HIGH : PIN_LOW);
}

void gpio_input_init(gpio_pin_enc_t pin, gpio_pull_t pull)
{
    rt_base_t p = to_rt_pin(pin);

    switch (pull)
    {
    case GPIO_IN_PULLUP:
        rt_pin_mode(p, PIN_MODE_INPUT_PULLUP);
        break;
    case GPIO_IN_PULLDOWN:
        rt_pin_mode(p, PIN_MODE_INPUT_PULLDOWN);
        break;
    default:
        rt_pin_mode(p, PIN_MODE_INPUT);
        break;
    }
}

int gpio_read(gpio_pin_enc_t pin)
{
    return rt_pin_read(to_rt_pin(pin)) ? 1 : 0;
}