/*
 * rt_pin_stub.c - RT-Thread rt_pin_* 的 host 桩实现：记录模式/电平，供测试断言。
 */
#include <drivers/pin.h>

#define STUB_PIN_MAX 64

static int s_mode[STUB_PIN_MAX];
static int s_val[STUB_PIN_MAX];

static int within(rt_base_t pin)
{
    return (pin >= 0) && (pin < STUB_PIN_MAX);
}

rt_base_t rt_pin_mode(rt_base_t pin, int mode)
{
    if (within(pin))
        s_mode[pin] = mode;
    return 0;
}

void rt_pin_write(rt_base_t pin, int value)
{
    if (within(pin))
        s_val[pin] = value;
}

int rt_pin_read(rt_base_t pin)
{
    return within(pin) ? s_val[pin] : 0;
}

/* ---- 测试观测接口 ---- */
int rt_pin_stub_mode(rt_base_t pin)
{
    return within(pin) ? s_mode[pin] : -1;
}

int rt_pin_stub_value(rt_base_t pin)
{
    return within(pin) ? s_val[pin] : -1;
}