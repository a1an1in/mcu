/*
 * key.h - 板载按键模块接口
 *
 * 只依赖 OSAL 与 GPIO 统一接口（drivers/gpio/gpio.h），与 RTOS / MCU 解耦。
 */
#ifndef MODULE_KEY_H__
#define MODULE_KEY_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 板载按键（战舰 STM32F103 V4，原理图 KEY 块）：
 *   KEY_0  → PE4   KEY_1  → PE3   KEY_2  → PE2 ：按下把引脚接到 GND（低有效）
 *   KEY_UP → PA0（丝印 WK_UP）                  ：按下把引脚接到 VCC3.3（高有效） */
typedef enum {
    KEY_0 = 0,
    KEY_1,
    KEY_2,
    KEY_UP,
    KEY_COUNT
} key_id_t;

/* 配置按键引脚并启动消抖扫描线程（扫描周期 5ms，连续 4 次同电平才认变，约 20ms 消抖） */
void key_init(void);

/* 手动驱动一次采样并推进消抖状态机。
 * 正常情况下由 key_init() 起的扫描线程调用；宿主机单元测试或无 OS 场景
 * 可自行周期性调用它，从而不依赖任何 RTOS。 */
void key_poll_once(void);

/* 消抖后的当前状态：非 0 = 按下。可在任意线程调用 */
int key_is_down(key_id_t id);

/* 事件接口：自上次调用以来发生过一次「按下」则返回 1，读取即清除（不阻塞） */
int key_pressed(key_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* MODULE_KEY_H__ */
