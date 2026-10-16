/*
 * test_rtthread_gpio.c - RT-Thread 后端适配的 host 单元测试。
 * 用桩替换 rt_pin_*，验证 gpio.h 接口被正确转发（引脚编号、模式、电平）。
 */
#include <stdio.h>

#include <drivers/pin.h>   /* 桩头：提供 PIN_MODE_OUTPUT/PIN_HIGH/PIN_LOW/rt_pin_* */
#include "gpio.h"

/* 桩观测接口（rt_pin_stub.c） */
int rt_pin_stub_mode(rt_base_t pin);
int rt_pin_stub_value(rt_base_t pin);

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main(void)
{
    gpio_pin_enc_t led = GPIO_PIN(GPIO_PORT_C, 13u);
    rt_base_t  rt_pin = 2 * 16 + 13;   /* PC13 -> RT-Thread 线性引脚号 45 */

    /* gpio_output_init -> rt_pin_mode(PIN_MODE_OUTPUT) */
    gpio_output_init(led);
    CHECK(rt_pin_stub_mode(rt_pin) == PIN_MODE_OUTPUT);

    /* gpio_write(高) -> rt_pin_write(PIN_HIGH) */
    gpio_write(led, 1);
    CHECK(rt_pin_stub_value(rt_pin) == PIN_HIGH);

    /* gpio_write(低) -> rt_pin_write(PIN_LOW) */
    gpio_write(led, 0);
    CHECK(rt_pin_stub_value(rt_pin) == PIN_LOW);

    /* gpio_read -> rt_pin_read 返回值 */
    CHECK(gpio_read(led) == 0);

    /* 其它端口映射 sanity：PA0 / PB15 */
    gpio_write(GPIO_PIN(GPIO_PORT_A, 0u), 1);
    CHECK(rt_pin_stub_value(0) == PIN_HIGH);
    gpio_write(GPIO_PIN(GPIO_PORT_B, 15u), 1);
    CHECK(rt_pin_stub_value(31) == PIN_HIGH);

    if (failures == 0)
    {
        printf("PASS: rtthread_gpio 后端×gpio.h 接口一致\n");
        return 0;
    }
    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}