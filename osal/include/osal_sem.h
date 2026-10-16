/*
 * osal_sem.h - 信号量抽象接口
 */
#ifndef OSAL_SEM_H__
#define OSAL_SEM_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 信号量句柄 */
typedef void *osal_sem_t;

/* 创建信号量，初始计数为 initial */
osal_err_t osal_sem_create(osal_sem_t *s, uint32_t initial);

/* 释放信号量，使计数加一 */
void osal_sem_give(osal_sem_t s);

/*
 * 获取信号量。timeout_ms 为阻塞等待毫秒数，可用
 * OSAL_NO_WAIT / OSAL_WAIT_FOREVER。
 * @return OSAL_OK / OSAL_ETIMEDOUT / OSAL_EINVAL
 */
osal_err_t osal_sem_take(osal_sem_t s, uint32_t timeout_ms);

/* 删除信号量 */
void osal_sem_delete(osal_sem_t s);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_SEM_H__ */