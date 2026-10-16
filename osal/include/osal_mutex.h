/*
 * osal_mutex.h - 互斥量抽象接口（支持优先级继承的锁）
 */
#ifndef OSAL_MUTEX_H__
#define OSAL_MUTEX_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 互斥量句柄 */
typedef void *osal_mutex_t;

/* 创建互斥量（初始未锁定） */
osal_err_t osal_mutex_create(osal_mutex_t *m);

/* 锁定互斥量，timeout_ms 为等待时间 */
osal_err_t osal_mutex_lock(osal_mutex_t m, uint32_t timeout_ms);

/* 尝试锁定（非阻塞） */
osal_err_t osal_mutex_trylock(osal_mutex_t m);

/* 解锁互斥量 */
void osal_mutex_unlock(osal_mutex_t m);

/* 删除互斥量 */
void osal_mutex_delete(osal_mutex_t m);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_MUTEX_H__ */