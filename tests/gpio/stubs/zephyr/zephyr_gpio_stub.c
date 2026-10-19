/*
 * zephyr_gpio_stub.c - Zephyr gpio_* 的 host 桩实现。
 * 记录每个 (port,pin) 的配置标志与电平，供测试断言。
 */
#include <zephyr/drivers/gpio.h>

#include <stddef.h>   /* NULL */

#define STUB_GPIO_PORTS 5
#define STUB_GPIO_MAX_PIN 16

const struct device __gpio_dev_gpioa = {0};
const struct device __gpio_dev_gpiob = {0};
const struct device __gpio_dev_gpioc = {0};
const struct device __gpio_dev_gpiod = {0};
const struct device __gpio_dev_gpioe = {0};

static gpio_flags_t s_conf[STUB_GPIO_PORTS][STUB_GPIO_MAX_PIN];
static int          s_val[STUB_GPIO_PORTS][STUB_GPIO_MAX_PIN];

static int index_of(const struct device *dev)
{
    if (dev == &__gpio_dev_gpioa) return 0;
    if (dev == &__gpio_dev_gpiob) return 1;
    if (dev == &__gpio_dev_gpioc) return 2;
    if (dev == &__gpio_dev_gpiod) return 3;
    if (dev == &__gpio_dev_gpioe) return 4;
    return -1;
}

int gpio_pin_configure(const struct device *port, gpio_pin_t pin, gpio_flags_t flags)
{
    int idx = index_of(port);
    if (idx < 0 || pin >= STUB_GPIO_MAX_PIN)
        return -1;
    s_conf[idx][pin] = flags;
    return 0;
}

int gpio_pin_set(const struct device *port, gpio_pin_t pin, int value)
{
    int idx = index_of(port);
    if (idx < 0 || pin >= STUB_GPIO_MAX_PIN)
        return -1;
    s_val[idx][pin] = value;
    return 0;
}

int gpio_pin_get(const struct device *port, gpio_pin_t pin)
{
    int idx = index_of(port);
    if (idx < 0 || pin >= STUB_GPIO_MAX_PIN)
        return -1;
    return s_val[idx][pin];
}

/* ---- 测试观测接口 ---- */
gpio_flags_t zgpio_stub_conf(const struct device *dev, gpio_pin_t pin)
{
    int idx = index_of(dev);
    if (idx < 0 || pin >= STUB_GPIO_MAX_PIN)
        return 0;
    return s_conf[idx][pin];
}

int zgpio_stub_value(const struct device *dev, gpio_pin_t pin)
{
    int idx = index_of(dev);
    if (idx < 0 || pin >= STUB_GPIO_MAX_PIN)
        return -1;
    return s_val[idx][pin];
}

/* 模拟外部把引脚拉高/拉低（输入场景：按键按下/松开） */
void zgpio_stub_set_value(const struct device *dev, gpio_pin_t pin, int value)
{
    int idx = index_of(dev);
    if (idx >= 0 && pin < STUB_GPIO_MAX_PIN)
        s_val[idx][pin] = value;
}

const struct device *zgpio_stub_dev(int port)
{
    static const struct device *const devs[STUB_GPIO_PORTS] = {
        &__gpio_dev_gpioa, &__gpio_dev_gpiob, &__gpio_dev_gpioc,
        &__gpio_dev_gpiod, &__gpio_dev_gpioe,
    };
    if (port < 0 || port >= STUB_GPIO_PORTS)
        return NULL;
    return devs[port];
}