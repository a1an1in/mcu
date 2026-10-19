/*
 * fake_gpio.c - gpio.h 接口的宿主机「假实现」（host 单测用，不碰真硬件）
 *
 * 用一张 (端口,引脚) 电平表代替真实寄存器 / OS 驱动：
 *   - gpio_write()         记录模块写出的电平（仅对已配置为输出的脚生效）；
 *   - gpio_input_init()    记录上/下拉，并按上/下拉给出空闲电平；
 *   - fake_gpio_set_level() 测试注入：模拟外部把引脚拉高/拉低（按键按下/松开）；
 *   - fake_gpio_level()     测试观测：读某个引脚当前电平。
 */
#include "gpio.h"

#define FAKE_PORTS  5
#define FAKE_PINS   16

static int s_level[FAKE_PORTS][FAKE_PINS];
static int s_is_output[FAKE_PORTS][FAKE_PINS];
static int s_pull[FAKE_PORTS][FAKE_PINS];

static int decode(gpio_pin_enc_t pin, int *port, int *num)
{
    int p = (int)((pin >> 8) & 0xFFu);
    int n = (int)(pin & 0xFFu);

    if (p < 0 || p >= FAKE_PORTS || n < 0 || n >= FAKE_PINS)
        return 0;

    *port = p;
    *num  = n;
    return 1;
}

void gpio_init(void)
{
}

void gpio_output_init(gpio_pin_enc_t pin)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return;

    s_is_output[port][num] = 1;
    s_pull[port][num]      = GPIO_IN_FLOAT;
}

void gpio_input_init(gpio_pin_enc_t pin, gpio_pull_t pull)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return;

    s_is_output[port][num] = 0;
    s_pull[port][num]      = (int)pull;

    /* 空闲电平由内部上/下拉决定；按键按下时的外部强拉由测试注入 */
    if (pull == GPIO_IN_PULLUP)
        s_level[port][num] = 1;
    else if (pull == GPIO_IN_PULLDOWN)
        s_level[port][num] = 0;
}

void gpio_write(gpio_pin_enc_t pin, int val)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return;

    if (!s_is_output[port][num])
        return;    /* 输入脚上的写只是选上/下拉，不改电平 */

    s_level[port][num] = val ? 1 : 0;
}

int gpio_read(gpio_pin_enc_t pin)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return 0;

    return s_level[port][num] ? 1 : 0;
}

/* ---------------- 测试观测 / 注入接口 ---------------- */

void fake_gpio_set_level(gpio_pin_enc_t pin, int level)
{
    int port, num;

    if (decode(pin, &port, &num))
        s_level[port][num] = level ? 1 : 0;
}

int fake_gpio_level(gpio_pin_enc_t pin)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return -1;

    return s_level[port][num] ? 1 : 0;
}

int fake_gpio_is_output(gpio_pin_enc_t pin)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return -1;

    return s_is_output[port][num];
}

int fake_gpio_pull(gpio_pin_enc_t pin)
{
    int port, num;

    if (!decode(pin, &port, &num))
        return -1;

    return s_pull[port][num];
}

void fake_gpio_reset(void)
{
    int i, j;

    for (i = 0; i < FAKE_PORTS; i++)
    {
        for (j = 0; j < FAKE_PINS; j++)
        {
            s_level[i][j]     = 0;
            s_is_output[i][j] = 0;
            s_pull[i][j]      = 0;
        }
    }
}
