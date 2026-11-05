/*
 * buf_alloc.h - 可移植的大块缓冲分配 <modules/mem/>
 *
 * 有外部 SRAM 时优先把大块放外部，没有 SRAM 或分配失败时
 * 自动回退到 OSAL 内部堆。业务只关心 buf_alloc/buf_free。
 */
#ifndef MODULES_MEM_BUF_ALLOC_H__
#define MODULES_MEM_BUF_ALLOC_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *buf_alloc(size_t size);
void buf_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* MODULES_MEM_BUF_ALLOC_H__ */
