/*
 * osal_memory.c - OSAL 内存接口 ↔ Zephyr 适配层
 *
 * 映射到 Zephyr 的 k_malloc / k_free（使用系统堆，需 CONFIG_SYSTEM_HEAP_SIZE>0）。
 */
#include <stdint.h>

#include <zephyr/kernel.h>

#include <osal.h>

void *osal_malloc(size_t size)
{
    return k_malloc(size);
}

void osal_free(void *ptr)
{
    if (ptr != OSAL_NULL)
        k_free(ptr);
}

size_t osal_mem_available(void)
{
    /*
     * 精确统计需通过 z_mem_pool 的运行时统计接口，获取系统堆指针较繁琐，
     * 这里返回 0 表示"不支持/未知"（与 RT-Thread 后端语义一致）。
     */
    return 0;
}