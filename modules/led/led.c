/*
 * led.c - 示例模块实现：LED 闪烁
 *
 * 通过 OSAL 起一个线程周期性翻转驱动的 GPIO（drivers/gpio/ 组件：gpio.h + 实现）。
 * 默认板载 LED 接在 PC13（低电平点亮，常见于 Blue Pill / 多数 F103 开发板），
 * 若你的硬件不同，修改 LED_PIN 即可。
 */
#include <osal.h>
#include "gpio.h"

#include "led.h"

#define LED_STACK_SIZE     1024u
#define LED_PRIORITY       20u
#define LED_PERIOD_MS      500u

/* 蓝板(Blue Pill) LED 通常为 PC13 */
#define LED_PIN            GPIO_PIN(GPIO_PORT_C, 13u)

static osal_thread_t s_led_thread = OSAL_NULL;

static void led_entry(void *param)
{
    int on = 0;

    (void)param;

    for (;;)
    {
        on = !on;
        gpio_write(LED_PIN, on);      /* 低电平点亮 */
        osal_thread_sleep_ms(LED_PERIOD_MS);
    }
}

void led_init(void)
{
    gpio_output_init(LED_PIN);
    gpio_write(LED_PIN, 1);           /* 默认熄灭（低电平点亮） */

    if (osal_thread_create(&s_led_thread, "led",
                           led_entry, OSAL_NULL,
                           OSAL_NULL, LED_STACK_SIZE, LED_PRIORITY) != OSAL_OK)
    {
        osal_printf("[led] create thread failed\n");
    }
}