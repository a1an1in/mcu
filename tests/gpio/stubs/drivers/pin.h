/*
 * drivers/pin.h - RT-Thread PIN 框架头的 host 测试桩
 * 仅提供 rtthread_gpio.c 所需的类型 / 宏 / 原型，避免在 host 引入完整 RT-Thread。
 * include 名与真实 RT-Thread 一致（<drivers/pin.h>）。
 */
#ifndef STUB_RT_THREAD_DRIVERS_PIN_H__
#define STUB_RT_THREAD_DRIVERS_PIN_H__

typedef int rt_base_t;

#define PIN_LOW                 0x00
#define PIN_HIGH                0x01
#define PIN_MODE_OUTPUT         0x00

rt_base_t rt_pin_mode(rt_base_t pin, int mode);
void      rt_pin_write(rt_base_t pin, int value);
int       rt_pin_read(rt_base_t pin);

#endif /* STUB_RT_THREAD_DRIVERS_PIN_H__ */