/*
 * osal_timer.h - 软件定时器抽象接口
 */
#ifndef OSAL_TIMER_H__
#define OSAL_TIMER_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 定时器句柄 */
typedef void *osal_timer_t;

/* 定时器回调原型（运行在定时器服务的上下文，回调内不应长时间阻塞） */
typedef void (*osal_timer_cb)(void *param);

/*
 * 创建一次性（oneshot）定时器，period_ms 后触发一次回调。
 * @param cb   回调函数
 * @param param 回调参数
 * @param periodic 1=周期触发，0=一次性
 * @param t 输出句柄
 */
osal_err_t osal_timer_create(osal_timer_t *t, const char *name,
                             osal_timer_cb cb, void *param,
                             uint32_t period_ms, bool periodic);

/* 启动定时器（从 0 重新计时） */
osal_err_t osal_timer_start(osal_timer_t t);

/* 停止定时器 */
osal_err_t osal_timer_stop(osal_timer_t t);

/* 删除定时器 */
void osal_timer_delete(osal_timer_t t);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_TIMER_H__ */