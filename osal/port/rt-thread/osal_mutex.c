/*
 * osal_mutex.c - OSAL 互斥量接口 ↔ RT-Thread 适配层
 */
#include <stdint.h>

#include <osal.h>
#include <rtthread.h>

/* 内核对象名上限是 RT_NAME_MAX - 1（还要留 1 字节给 '\0'），名字超长会在
 * rt_object_allocate() 里触发断言直接死机 —— 那要等烧到板子上才看得见，
 * 这里在编译期就挡住。RT_NAME_MAX 由 bsp/<board>/rtconfig.h 决定。 */
_Static_assert(sizeof("osal_mutex") - 1u <= RT_NAME_MAX - 1u,
               "RT_NAME_MAX 太小：osal_mutex 名字超出内核对象名上限（obj_name_len <= RT_NAME_MAX - 1）");

static rt_tick_t osal_to_tick(uint32_t ms)
{
    if (ms == OSAL_WAIT_FOREVER)
        return (rt_tick_t)RT_WAITING_FOREVER;
    if (ms == OSAL_NO_WAIT)
        return (rt_tick_t)RT_WAITING_NO;
    return rt_tick_from_millisecond((rt_int32_t)ms);
}

osal_err_t osal_mutex_create(osal_mutex_t *m)
{
    rt_mutex_t mutex;

    if (m == RT_NULL)
        return OSAL_EINVAL;

    mutex = rt_mutex_create("osal_mutex", RT_IPC_FLAG_FIFO);
    if (mutex == RT_NULL)
        return OSAL_ENOMEM;

    *m = (osal_mutex_t)mutex;
    return OSAL_OK;
}

osal_err_t osal_mutex_lock(osal_mutex_t m, uint32_t timeout_ms)
{
    rt_err_t rc;

    if (m == OSAL_NULL)
        return OSAL_EINVAL;

    rc = rt_mutex_take((rt_mutex_t)m, osal_to_tick(timeout_ms));
    if (rc == RT_EOK)
        return OSAL_OK;
    if (rc == -RT_ETIMEOUT)
        return OSAL_ETIMEDOUT;
    return OSAL_EIO;
}

osal_err_t osal_mutex_trylock(osal_mutex_t m)
{
    return osal_mutex_lock(m, OSAL_NO_WAIT);
}

void osal_mutex_unlock(osal_mutex_t m)
{
    if (m != OSAL_NULL)
        rt_mutex_release((rt_mutex_t)m);
}

void osal_mutex_delete(osal_mutex_t m)
{
    if (m != OSAL_NULL)
        rt_mutex_delete((rt_mutex_t)m);
}