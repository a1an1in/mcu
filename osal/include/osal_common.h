/*
 * osal_common.h - OSAL 公共类型、错误码与等待常量
 *
 * 本文件与具体 RTOS 无关，供 osal/*.h 接口与各后端适配层共用。
 * 未来迁移到 Zephyr / FreeRTOS 时无需改动本文件。
 */
#ifndef OSAL_COMMON_H__
#define OSAL_COMMON_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 空指针（各 RTOS 的空指针语义统一化为 NULL） */
#define OSAL_NULL   ((void *)0)

/* 错误码（统一各 RTOS 的返回值语义） */
typedef enum {
    OSAL_OK         = 0,    /* 成功 */
    OSAL_ETIMEDOUT  = -1,   /* 超时 */
    OSAL_EINVAL     = -2,   /* 参数非法 */
    OSAL_ENOMEM     = -3,   /* 内存不足 */
    OSAL_EBUSY      = -4,   /* 资源忙 */
    OSAL_EDELETED   = -5,   /* 内核对象已被删除 */
    OSAL_EIO        = -6,   /* IO 错误 */
} osal_err_t;

/* 等待时间：无等待 / 永久等待 */
#define OSAL_NO_WAIT        ((uint32_t)0)
#define OSAL_WAIT_FOREVER   ((uint32_t)0xFFFFFFFFu)

#ifdef __cplusplus
}
#endif

#endif /* OSAL_COMMON_H__ */