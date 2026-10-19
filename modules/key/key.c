/*
 * key.c - 板载按键模块实现
 *
 * 只依赖 OSAL（<osal.h>）与 GPIO 统一接口（gpio.h），与 RTOS / MCU 解耦。
 *
 * 硬件（战舰 STM32F103 V4，原理图 page4 KEY 块）：
 *   KEY0 → PE4   KEY1 → PE3   KEY2 → PE2 ：另一端公共接 GND，按下读到低电平；
 *   WK_UP → PA0                          ：另一端接 VCC3.3，按下读到高电平。
 *   这里统一打开 MCU 内部上/下拉（KEY0~2 上拉、WK_UP 下拉），
 *   不管板上有无外部上拉电阻，空闲态都是稳定的。
 *
 * 消抖：采样推进放在 key_poll_once() 里（纯软件状态机，不依赖 tick 接口）。
 *   - 正常运行时由 key_init() 起的扫描线程每 5ms 调一次；
 *   - 连续 KEY_DEBOUNCE_SAMPLES 次采到同一电平才认为状态真的改变，
 *     中间任何一次抖动都会把计数清零 —— 即「连续稳定」型消抖。
 */
#include <osal.h>

#include "gpio.h"
#include "key.h"

#define KEY_SCAN_STACK_SIZE    768u
#define KEY_SCAN_PRIORITY      15u
#define KEY_SCAN_PERIOD_MS     5u
#define KEY_DEBOUNCE_SAMPLES   4u      /* 4 × 5ms ≈ 20ms 消抖 */

/* 引脚映射（换板子时只改这张表 + s_key_active_high） */
static const gpio_pin_enc_t s_key_pin[KEY_COUNT] = {
    GPIO_PIN(GPIO_PORT_E, 4u),   /* KEY_0  */
    GPIO_PIN(GPIO_PORT_E, 3u),   /* KEY_1  */
    GPIO_PIN(GPIO_PORT_E, 2u),   /* KEY_2  */
    GPIO_PIN(GPIO_PORT_A, 0u),   /* KEY_UP */
};

/* 空闲时的内部上/下拉：只有 WK_UP 是「接 3.3V、按下为高」 */
static const gpio_pull_t s_key_pull[KEY_COUNT] = {
    GPIO_IN_PULLUP,     /* KEY_0  */
    GPIO_IN_PULLUP,     /* KEY_1  */
    GPIO_IN_PULLUP,     /* KEY_2  */
    GPIO_IN_PULLDOWN,   /* KEY_UP */
};

static const int s_key_active_high[KEY_COUNT] = {
    0, 0, 0,   /* KEY_0/1/2：按下为低 */
    1,         /* KEY_UP    ：按下为高 */
};

static volatile int s_key_down[KEY_COUNT];   /* 消抖后的稳定状态：1=按下 */
static volatile int s_key_edge[KEY_COUNT];   /* 「按下」边沿标志，读后清除 */

static osal_thread_t s_scan_thread = OSAL_NULL;

static void key_scan_entry(void *param)
{
    (void)param;

    for (;;)
    {
        key_poll_once();
        osal_thread_sleep_ms(KEY_SCAN_PERIOD_MS);
    }
}

/* 读一次引脚原始电平，并统一换算成「是否按下」 */
static int key_raw_down(key_id_t id)
{
    int lv = gpio_read(s_key_pin[id]) ? 1 : 0;

    return s_key_active_high[id] ? lv : !lv;
}

void key_init(void)
{
    int i;

    for (i = 0; i < (int)KEY_COUNT; i++)
    {
        gpio_input_init(s_key_pin[i], s_key_pull[i]);
        s_key_down[i] = key_raw_down((key_id_t)i);   /* 上电先取一次初值，避免误报边沿 */
        s_key_edge[i] = 0;
    }

    if (osal_thread_create(&s_scan_thread, "keyscan",
                           key_scan_entry, OSAL_NULL,
                           OSAL_NULL, KEY_SCAN_STACK_SIZE,
                           KEY_SCAN_PRIORITY) != OSAL_OK)
    {
        osal_printf("[key] create scan thread failed\n");
    }
}

void key_poll_once(void)
{
    static int s_cnt[KEY_COUNT];
    int i;

    for (i = 0; i < (int)KEY_COUNT; i++)
    {
        int raw = key_raw_down((key_id_t)i);

        if (raw == s_key_down[i])
        {
            s_cnt[i] = 0;          /* 与稳定状态一致 → 抖动计数清零 */
            continue;
        }

        if (++s_cnt[i] >= (int)KEY_DEBOUNCE_SAMPLES)
        {
            s_key_down[i] = raw;   /* 连续稳定到阈值 → 提交新状态 */
            s_cnt[i] = 0;
            if (raw)
                s_key_edge[i] = 1; /* 记录一次「按下」边沿 */
        }
    }
}

int key_is_down(key_id_t id)
{
    if ((int)id < 0 || id >= KEY_COUNT)
        return 0;

    return s_key_down[id] ? 1 : 0;
}

int key_pressed(key_id_t id)
{
    int v;

    if ((int)id < 0 || id >= KEY_COUNT)
        return 0;

    v = s_key_edge[id];
    s_key_edge[id] = 0;
    return v;
}
