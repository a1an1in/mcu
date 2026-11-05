/*
 * buf_alloc.c - 外部 SRAM / 内部堆的适配层 <modules/mem/>
 *
 * 分配策略：
 *   - 有外部 SRAM 且 size >= BUF_SRAM_MIN：先走 sram_alloc；
 *   - 外部 SRAM 不可用、分配失败或小块：回退 osal_malloc。
 *
 * 释放时按地址判断来源，业务不需要记录指针来自哪块内存。
 */
#include <osal.h>

#include "buf_alloc.h"
#include "sram.h"
#include "sram_pool.h"

#define BUF_SRAM_MIN    256u

void *buf_alloc(size_t size)
{
    struct sram_dev *dev;
    void *p;

    if (size == 0u)
    {
        return NULL;
    }

    dev = sram_get_default();
    if ((dev != NULL) && (dev->ready != 0u) && (size >= BUF_SRAM_MIN))
    {
        p = sram_alloc(size);
        if (p != NULL)
        {
            return p;
        }
    }

    return osal_malloc(size);
}

void buf_free(void *ptr)
{
    struct sram_dev *dev;
    uintptr_t a;

    if (ptr == NULL)
    {
        return;
    }

    dev = sram_get_default();
    a = (uintptr_t)ptr;

    if ((dev != NULL) &&
        (a >= (uintptr_t)dev->base) &&
        (a < ((uintptr_t)dev->base + dev->size)))
    {
        sram_free(ptr);
        return;
    }

    osal_free(ptr);
}
