/*
 * osal_memory.c - OSAL 内存接口 ↔ RT-Thread 适配层
 */
#include <stdint.h>

#include <osal.h>
#include <rtthread.h>

void *osal_malloc(size_t size)
{
    return rt_malloc((rt_size_t)size);
}

void osal_free(void *ptr)
{
    if (ptr != OSAL_NULL)
        rt_free(ptr);
}

size_t osal_mem_available(void)
{
    /*
     * RT-Thread 未提供简单的一行式"剩余堆大小"公开发接口，
     * 这里返回 0 表示“不支持/未知”。
     * 如需要精确统计，可在此遍历 rt_memheap 的 free_list 自行计算。
     */
    (void)0;
    return 0;
}