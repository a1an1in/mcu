/*
 * osal_sem.c - OSAL 信号量接口 ↔ RT-Thread 适配层
 */
#include <stdint.h>

#include <osal.h>
#include <rtthread.h>

/* 毫秒超时 → RT-Thread tick 超时 */
static rt_tick_t osal_to_tick(uint32_t ms)
{
    if (ms == OSAL_WAIT_FOREVER)
        return (rt_tick_t)RT_WAITING_FOREVER;
    if (ms == OSAL_NO_WAIT)
        return (rt_tick_t)RT_WAITING_NO;
    return rt_tick_from_millisecond((rt_int32_t)ms);
}

osal_err_t osal_sem_create(osal_sem_t *s, uint32_t initial)
{
    rt_sem_t sem;

    if (s == RT_NULL)
        return OSAL_EINVAL;

    sem = rt_sem_create("osal_sem", (rt_uint32_t)initial, RT_IPC_FLAG_FIFO);
    if (sem == RT_NULL)
        return OSAL_ENOMEM;

    *s = (osal_sem_t)sem;
    return OSAL_OK;
}

void osal_sem_give(osal_sem_t s)
{
    if (s != OSAL_NULL)
        rt_sem_release((rt_sem_t)s);
}

osal_err_t osal_sem_take(osal_sem_t s, uint32_t timeout_ms)
{
    rt_err_t rc;

    if (s == OSAL_NULL)
        return OSAL_EINVAL;

    rc = rt_sem_take((rt_sem_t)s, osal_to_tick(timeout_ms));
    if (rc == RT_EOK)
        return OSAL_OK;
    if (rc == -RT_ETIMEOUT)
        return OSAL_ETIMEDOUT;
    return OSAL_EIO;
}

void osal_sem_delete(osal_sem_t s)
{
    if (s != OSAL_NULL)
        rt_sem_delete((rt_sem_t)s);
}