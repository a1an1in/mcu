/*
 * osal_memory.h - 动态内存抽象接口
 */
#ifndef OSAL_MEMORY_H__
#define OSAL_MEMORY_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从堆分配 size 字节内存，失败返回 NULL */
void *osal_malloc(size_t size);

/* 释放由 osal_malloc 分配的内存 */
void osal_free(void *ptr);

/* 返回本线程可用内存块的大小（可选实现，返回 0 表示不支持） */
size_t osal_mem_available(void);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_MEMORY_H__ */