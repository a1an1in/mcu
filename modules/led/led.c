/*
 * led.c - 板载 LED 模块实现
 *
 * 只依赖 GPIO 统一接口（gpio.h），不直接接触任何 RTOS / MCU 寄存器，
 * 因此换 RTOS 或换 GPIO 后端（-DGPIO_BACKEND=）时代码零改动。
 *
 * 硬件（战舰 STM32F103 V4，原理图 LED 块）：
 *   DS0（红） → PB5    DS1（绿） → PE5
 *   两个 LED 的阳极经 510R 接 VCC3.3、阴极接 MCU 引脚，
 *   所以是「低电平点亮」，写 0 亮、写 1 灭。
 */
#include "gpio.h"
#include "led.h"

/* 引脚映射（换板子时只改这张表 + LED_ACTIVE_LOW） */
static const gpio_pin_enc_t s_led_pin[LED_COUNT] = {
    GPIO_PIN(GPIO_PORT_B, 5u),   /* LED_DS0 */
    GPIO_PIN(GPIO_PORT_E, 5u),   /* LED_DS1 */
};

/* 战舰 V4 的 DS0/DS1 为低电平点亮 */
#define LED_ACTIVE_LOW   1

/* 软件记录的亮灭状态（供 toggle、也便于与硬件电平对照） */
static int s_led_on[LED_COUNT];

static void led_hw_write(led_id_t id, int on)
{
#if LED_ACTIVE_LOW
    gpio_write(s_led_pin[id], on ? 0 : 1);
#else
    gpio_write(s_led_pin[id], on ? 1 : 0);
#endif
}

void led_init(void)
{
    int i;

    for (i = 0; i < (int)LED_COUNT; i++)
    {
        gpio_output_init(s_led_pin[i]);
        s_led_on[i] = 0;
        led_hw_write((led_id_t)i, 0);   /* 上电默认熄灭 */
    }
}

void led_set(led_id_t id, int on)
{
    if ((int)id < 0 || id >= LED_COUNT)
        return;

    s_led_on[id] = on ? 1 : 0;
    led_hw_write(id, s_led_on[id]);
}

void led_on(led_id_t id)
{
    led_set(id, 1);
}

void led_off(led_id_t id)
{
    led_set(id, 0);
}

void led_toggle(led_id_t id)
{
    if ((int)id < 0 || id >= LED_COUNT)
        return;

    led_set(id, !s_led_on[id]);
}
