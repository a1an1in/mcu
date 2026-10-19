/*
 * test_key_led.c - 「按键 → 板载 LED」这条硬件链路的 host 单元测试
 *
 * 真机上走的链路是：
 *   KEY0(PE4) / KEY1(PE3)   按住接地，低有效
 *   KEY2(PE2)               按一下产生一个「按下」边沿，用来切换状态
 *     └─ modules/key: key_poll_once() 消抖 → key_is_down() / key_pressed()
 *          ├─ 应用层: led_set(LED_DS0, KEY0 按下) / led_set(LED_DS1, KEY1 按下)
 *          └─ 应用层: key_pressed(KEY2) 翻转「交替闪烁」开关
 *               └─ modules/led: DS0(PB5) / DS1(PE5)，低电平点亮
 *
 * 本测试把同一份 modules/led/led.c、modules/key/key.c 编译进来的同时，
 * 用 fake_gpio.c / fake_osal.c 顶掉真硬件与 RTOS，于是在宿主机上把这条
 * 链路真实执行一遍并断言。重点覆盖三类容易写错的地方：
 *
 *   1) 极性：DS0/DS1 是低电平点亮，KEY0~2 是按下接地、WK_UP 是按下接 3.3V；
 *   2) 消抖：抖动不能改变状态；一次带回弹的按下只能产生 1 个边沿 —— 对 KEY2
 *      这种「切换键」尤其致命，多一个边沿就会把开关连翻两次（看起来像没反应）；
 *   3) 边沿语义：按住不放不再产生边沿，边沿读一次即清除。
 *
 * 引脚常量与 modules/led、modules/key 里的映射表保持一致。
 */
#include <stdio.h>

#include "gpio.h"
#include "key.h"
#include "led.h"

/* fake_gpio.c 提供的观测 / 注入接口 */
void fake_gpio_set_level(gpio_pin_enc_t pin, int level);
int  fake_gpio_level(gpio_pin_enc_t pin);
int  fake_gpio_is_output(gpio_pin_enc_t pin);
int  fake_gpio_pull(gpio_pin_enc_t pin);
void fake_gpio_reset(void);

/* 战舰 STM32F103 V4 的板载引脚 */
#define PIN_DS0    GPIO_PIN(GPIO_PORT_B, 5u)   /* DS0（红） */
#define PIN_DS1    GPIO_PIN(GPIO_PORT_E, 5u)   /* DS1（绿） */
#define PIN_KEY0   GPIO_PIN(GPIO_PORT_E, 4u)
#define PIN_KEY1   GPIO_PIN(GPIO_PORT_E, 3u)
#define PIN_KEY2   GPIO_PIN(GPIO_PORT_E, 2u)
#define PIN_KEY_UP GPIO_PIN(GPIO_PORT_A, 0u)   /* 丝印 WK_UP */

/* 下标与 key_id_t 一一对应：KEY_0=0, KEY_1=1, KEY_2=2, KEY_UP=3 */
static const gpio_pin_enc_t s_key_pin[KEY_COUNT] = {
    PIN_KEY0, PIN_KEY1, PIN_KEY2, PIN_KEY_UP
};
/* 空闲电平 / 按下电平：KEY0~2 上拉故空闲为高、按下为低；WK_UP 相反 */
static const int s_key_idle[KEY_COUNT]   = { 1, 1, 1, 0 };
static const int s_key_down_lv[KEY_COUNT] = { 0, 0, 0, 1 };

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* 模拟外部电平：按下 / 松开 */
static void key_press(key_id_t id)   { fake_gpio_set_level(s_key_pin[id], s_key_down_lv[id]); }
static void key_release(key_id_t id) { fake_gpio_set_level(s_key_pin[id], s_key_idle[id]); }

static void all_release(void)
{
    int i;

    for (i = 0; i < (int)KEY_COUNT; i++)
        key_release((key_id_t)i);
}

/* 推进消抖状态机：真机上由 key 扫描线程每 5ms 调一次 key_poll_once() */
static void poll_once(int times)
{
    while (times-- > 0)
        key_poll_once();
}

/* 采样到稳定（4 次消抖阈值留足余量） */
static void settle(void)
{
    poll_once(10);
}

/* 应用层那条映射：KEY0 按住 → DS0 亮；KEY1 按住 → DS1 亮（手动优先） */
static void mirror_leds(void)
{
    led_set(LED_DS0, key_is_down(KEY_0));
    led_set(LED_DS1, key_is_down(KEY_1));
}

int main(void)
{
    fake_gpio_reset();

    led_init();
    key_init();

    /* ---- 1) 引脚配置 ---- */
    CHECK(fake_gpio_is_output(PIN_DS0) == 1);
    CHECK(fake_gpio_is_output(PIN_DS1) == 1);
    CHECK(fake_gpio_is_output(PIN_KEY0) == 0);
    CHECK(fake_gpio_is_output(PIN_KEY1) == 0);
    CHECK(fake_gpio_is_output(PIN_KEY2) == 0);
    CHECK(fake_gpio_pull(PIN_KEY0) == GPIO_IN_PULLUP);
    CHECK(fake_gpio_pull(PIN_KEY1) == GPIO_IN_PULLUP);
    CHECK(fake_gpio_pull(PIN_KEY2) == GPIO_IN_PULLUP);
    CHECK(fake_gpio_pull(PIN_KEY_UP) == GPIO_IN_PULLDOWN);

    /* ---- 2) 上电默认全灭：低电平点亮 ⇒ 输出高 ---- */
    CHECK(fake_gpio_level(PIN_DS0) == 1);
    CHECK(fake_gpio_level(PIN_DS1) == 1);

    /* ---- 3) 全部松开：稳定在松开态，且没有任何按下事件 ---- */
    all_release();
    settle();
    CHECK(key_is_down(KEY_0) == 0);
    CHECK(key_is_down(KEY_1) == 0);
    CHECK(key_is_down(KEY_2) == 0);
    CHECK(key_is_down(KEY_UP) == 0);
    CHECK(key_pressed(KEY_0) == 0);
    CHECK(key_pressed(KEY_1) == 0);
    CHECK(key_pressed(KEY_2) == 0);

    /* ---- 4) KEY0 按住 → DS0 亮，DS1 不受影响 ---- */
    key_press(KEY_0);
    poll_once(4);
    CHECK(key_is_down(KEY_0) == 1);
    CHECK(key_is_down(KEY_1) == 0);
    mirror_leds();
    CHECK(fake_gpio_level(PIN_DS0) == 0);   /* DS0 点亮 */
    CHECK(fake_gpio_level(PIN_DS1) == 1);   /* DS1 不受影响 */

    /* 按住不放：只应有一个边沿，不能当成连击 */
    poll_once(20);
    CHECK(key_pressed(KEY_0) == 1);
    CHECK(key_pressed(KEY_0) == 0);         /* 读后清除 */

    /* ---- 5) KEY1 也按住 → 两个灯都亮（互不干扰） ---- */
    key_press(KEY_1);
    poll_once(4);
    mirror_leds();
    CHECK(fake_gpio_level(PIN_DS0) == 0);
    CHECK(fake_gpio_level(PIN_DS1) == 0);

    /* 只松开 KEY0 → DS0 灭，仍按着的 DS1 保持亮 */
    key_release(KEY_0);
    poll_once(4);
    mirror_leds();
    CHECK(fake_gpio_level(PIN_DS0) == 1);
    CHECK(fake_gpio_level(PIN_DS1) == 0);

    key_release(KEY_1);
    poll_once(4);
    mirror_leds();
    CHECK(fake_gpio_level(PIN_DS0) == 1);
    CHECK(fake_gpio_level(PIN_DS1) == 1);

    /* ---- 6) KEY2 是「切换键」：抖动绝不能变成多次按下 ---- */
    /* 一次带回弹的按下：中间抖了 1 次，最终只应产生 1 个边沿 */
    key_press(KEY_2);
    poll_once(1);
    key_release(KEY_2);          /* 回弹 */
    poll_once(1);
    key_press(KEY_2);
    poll_once(8);
    CHECK(key_is_down(KEY_2) == 1);
    CHECK(key_pressed(KEY_2) == 1);     /* 恰好 1 次 → 交替开关只翻一次 */
    CHECK(key_pressed(KEY_2) == 0);

    /* 一直按住：不应再产生边沿（否则交替开关会一直翻） */
    poll_once(50);
    CHECK(key_pressed(KEY_2) == 0);

    /* 松开 → 不产生「按下」边沿；再按一次 → 又是 1 个边沿 */
    key_release(KEY_2);
    poll_once(4);
    CHECK(key_is_down(KEY_2) == 0);
    CHECK(key_pressed(KEY_2) == 0);

    key_press(KEY_2);
    poll_once(4);
    CHECK(key_pressed(KEY_2) == 1);
    key_release(KEY_2);
    poll_once(4);

    /* ---- 7) 交替闪烁用的 LED 基本操作：极性 + 互不干扰 ---- */
    led_on(LED_DS0);
    led_off(LED_DS1);
    CHECK(fake_gpio_level(PIN_DS0) == 0);
    CHECK(fake_gpio_level(PIN_DS1) == 1);

    /* 模拟交替相位翻转一次：DS0 灭、DS1 亮 */
    led_toggle(LED_DS0);
    led_toggle(LED_DS1);
    CHECK(fake_gpio_level(PIN_DS0) == 1);
    CHECK(fake_gpio_level(PIN_DS1) == 0);

    /* 停止交替：两个都灭 */
    led_set(LED_DS0, 0);
    led_set(LED_DS1, 0);
    CHECK(fake_gpio_level(PIN_DS0) == 1);
    CHECK(fake_gpio_level(PIN_DS1) == 1);

    /* ---- 8) WK_UP 是高有效：同一个模块、相反的极性 ---- */
    key_release(KEY_UP);        /* 空闲=低 */
    settle();
    CHECK(key_is_down(KEY_UP) == 0);

    key_press(KEY_UP);          /* 按下=高 */
    settle();
    CHECK(key_is_down(KEY_UP) == 1);
    CHECK(key_pressed(KEY_UP) == 1);

    if (failures == 0)
    {
        printf("PASS: 按键->LED 链路断言全部通过"
               "（KEY0->DS0 / KEY1->DS1 / KEY2 切换交替；含消抖、边沿与低电平点亮极性）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
