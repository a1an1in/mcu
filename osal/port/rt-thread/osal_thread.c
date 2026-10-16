/*
 * osal_thread.c - OSAL 线程接口 ↔ RT-Thread 适配层
 */
#include <stdint.h>
#include <string.h>

#include <osal.h>
#include <rtthread.h>

osal_err_t osal_thread_create(osal_thread_t *t, const char *name,
                              osal_thread_entry entry, void *param,
                              void *stack, size_t stack_size, uint32_t priority)
{
    rt_thread_t th;

    (void)stack; /* RT-Thread 动态线程由内核管理栈，此处忽略外部传入的栈指针 */

    th = rt_thread_create(name, (void (*)(void *))entry, param,
                          (rt_uint32_t)stack_size,
                          (rt_uint8_t)priority,
                          10 /* tick */);
    if (th == RT_NULL)
        return OSAL_ENOMEM;

    /* v5.2.2 中 rt_thread_create 只创建不启动，需显式 startup */
    if (rt_thread_startup(th) != RT_EOK)
    {
        rt_thread_delete(th);
        return OSAL_EIO;
    }

    if (t != RT_NULL)
        *t = (osal_thread_t)th;

    return OSAL_OK;
}

void osal_thread_delete(osal_thread_t t)
{
    if (t != OSAL_NULL)
        rt_thread_delete((rt_thread_t)t);
}

void osal_thread_yield(void)
{
    rt_thread_yield();
}

void osal_thread_sleep_ms(uint32_t ms)
{
    rt_thread_mdelay(ms);
}

osal_thread_t osal_thread_self(void)
{
    return (osal_thread_t)rt_thread_self();
}