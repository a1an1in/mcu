/*
 * led.h - 示例模块：控制板载 LED 以固定周期闪烁
 */
#ifndef MODULE_LED_H__
#define MODULE_LED_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LED 引脚并启动闪烁线程 */
void led_init(void);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_LED_H__ */