/*
 * osal_timer.c - OSAL 软件定时器接口 ↔ RT-Thread 适配层
 */
#include <stdint.h>

#include <osal.h>
#include <rtthread.h>

osal_err_t osal_timer_create(osal_timer_t *t, const char *name,
                             osal_timer_cb cb, void *param,
                             uint32_t period_ms, bool periodic)
{
    rt_timer_t timer;
    rt_uint8_t flag;

    if (t == RT_NULL || cb == RT_NULL)
        return OSAL_EINVAL;

    flag = periodic ? (rt_uint8_t)RT_TIMER_FLAG_PERIODIC
                    : (rt_uint8_t)RT_TIMER_FLAG_ONE_SHOT;

    timer = rt_timer_create(name, (void (*)(void *))cb, param,
                            rt_tick_from_millisecond((rt_int32_t)period_ms),
                            flag);
    if (timer == RT_NULL)
        return OSAL_ENOMEM;

    *t = (osal_timer_t)timer;
    return OSAL_OK;
}

osal_err_t osal_timer_start(osal_timer_t t)
{
    if (t == OSAL_NULL)
        return OSAL_EINVAL;
    if (rt_timer_start((rt_timer_t)t) != RT_EOK)
        return OSAL_EIO;
    return OSAL_OK;
}

osal_err_t osal_timer_stop(osal_timer_t t)
{
    if (t == OSAL_NULL)
        return OSAL_EINVAL;
    if (rt_timer_stop((rt_timer_t)t) != RT_EOK)
        return OSAL_EIO;
    return OSAL_OK;
}

void osal_timer_delete(osal_timer_t t)
{
    if (t != OSAL_NULL)
        rt_timer_delete((rt_timer_t)t);
}