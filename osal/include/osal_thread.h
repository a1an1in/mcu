/*
 * osal_thread.h - 线程 / 任务抽象接口
 *
 * 应用层（applications/、modules/）只调用本接口，不直接依赖任何 RTOS 头文件，
 * 从而在 RT-Thread / Zephyr / FreeRTOS 之间迁移时实现"零代码改动"。
 */
#ifndef OSAL_THREAD_H__
#define OSAL_THREAD_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 线程句柄（不透明指针） */
typedef void *osal_thread_t;

/* 线程入口函数原型 */
typedef void (*osal_thread_entry)(void *param);

/*
 * 创建一个线程并启动。
 *
 * @param name        线程名（可 NULL）
 * @param entry       入口函数
 * @param param       传给入口函数的参数
 * @param stack       线程栈指针；传 NULL 表示由 OS 自动分配
 * @param stack_size  栈大小（字节）
 * @param priority    线程优先级（数值越小优先级越高）
 * @param t           输出线程句柄
 * @return OSAL_OK 或错误码
 */
osal_err_t osal_thread_create(osal_thread_t *t, const char *name,
                              osal_thread_entry entry, void *param,
                              void *stack, size_t stack_size, uint32_t priority);

/* 删除一个线程（释放其资源）。ROS 空转计时的场景应改为通知线程自行退出 */
void osal_thread_delete(osal_thread_t t);

/* 让出 CPU 给同优先级的其它就绪线程 */
void osal_thread_yield(void);

/* 当前线程睡眠指定毫秒 */
void osal_thread_sleep_ms(uint32_t ms);

/* 返回当前线程句柄 */
osal_thread_t osal_thread_self(void);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_THREAD_H__ */