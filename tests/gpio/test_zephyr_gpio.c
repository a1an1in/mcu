/*
 * test_zephyr_gpio.c - Zephyr 后端适配的 host 单元测试。
 * 用桩替换 gpio_pin_* / device-tree，验证 gpio.h 接口被正确转发（设备、引脚、配置、电平）。
 */
#include <stdio.h>

#include "gpio.h"
#include <zephyr/drivers/gpio.h>

/* 桩观测接口（zephyr_gpio_stub.c） */
gpio_flags_t     zgpio_stub_conf(const struct device *dev, gpio_pin_t pin);
int              zgpio_stub_value(const struct device *dev, gpio_pin_t pin);
const struct device *zgpio_stub_dev(int port);

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
    const struct device *dev_c = zgpio_stub_dev(GPIO_PORT_C);

    CHECK(dev_c != NULL);

    /* gpio_output_init -> gpio_pin_configure(dev, 13, GPIO_OUTPUT) */
    gpio_output_init(led);
    CHECK(zgpio_stub_conf(dev_c, 13u) == GPIO_OUTPUT);

    /* gpio_write(高) -> gpio_pin_set(dev, 13, 1) */
    gpio_write(led, 1);
    CHECK(zgpio_stub_value(dev_c, 13u) == 1);

    /* gpio_write(低) -> gpio_pin_set(dev, 13, 0) */
    gpio_write(led, 0);
    CHECK(zgpio_stub_value(dev_c, 13u) == 0);

    /* gpio_read -> gpio_pin_get */
    CHECK(gpio_read(led) == 0);

    /* 其它端口映射 sanity：PA0 -> 设备 A, PB15 -> 设备 B */
    gpio_output_init(GPIO_PIN(GPIO_PORT_A, 0u));
    CHECK(zgpio_stub_conf(zgpio_stub_dev(GPIO_PORT_A), 0u) == GPIO_OUTPUT);
    gpio_write(GPIO_PIN(GPIO_PORT_B, 15u), 1);
    CHECK(zgpio_stub_value(zgpio_stub_dev(GPIO_PORT_B), 15u) == 1);

    if (failures == 0)
    {
        printf("PASS: zephyr_gpio 后端×gpio.h 接口一致\n");
        return 0;
    }
    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}