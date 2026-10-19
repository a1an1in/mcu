/*
 * led.h - 板载 LED 模块接口
 *
 * 只依赖 GPIO 统一接口（drivers/gpio/gpio.h），与 RTOS / MCU 解耦。
 */
#ifndef MODULE_LED_H__
#define MODULE_LED_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 板载 LED（战舰 STM32F103 V4）：
 *   LED_DS0 → PB5（红）   LED_DS1 → PE5（绿）
 *   两个都是「低电平点亮」（3.3V —510R— LED — MCU 引脚） */
typedef enum {
    LED_DS0 = 0,
    LED_DS1,
    LED_COUNT
} led_id_t;

/* 初始化所有 LED 引脚并置为「熄灭」（不启动任何线程） */
void led_init(void);

/* 设置指定 LED：on 非 0 = 点亮（内部已按「低电平点亮」换算） */
void led_set(led_id_t id, int on);

/* 点亮 / 熄灭 / 翻转 */
void led_on(led_id_t id);
void led_off(led_id_t id);
void led_toggle(led_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_LED_H__ */
