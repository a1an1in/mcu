/*
 * osal_queue.c - OSAL 消息队列接口 ↔ RT-Thread 适配层
 */
#include <stdint.h>
#include <string.h>

#include <osal.h>
#include <rtthread.h>

static rt_tick_t osal_to_tick(uint32_t ms)
{
    if (ms == OSAL_WAIT_FOREVER)
        return (rt_tick_t)RT_WAITING_FOREVER;
    if (ms == OSAL_NO_WAIT)
        return (rt_tick_t)RT_WAITING_NO;
    return rt_tick_from_millisecond((rt_int32_t)ms);
}

osal_err_t osal_queue_create(osal_queue_t *q, uint32_t item_size, uint32_t item_count)
{
    rt_mq_t mq;

    if (q == RT_NULL || item_size == 0 || item_count == 0)
        return OSAL_EINVAL;

    mq = rt_mq_create("osal_mq", (rt_size_t)item_size, (rt_size_t)item_count,
                      RT_IPC_FLAG_FIFO);
    if (mq == RT_NULL)
        return OSAL_ENOMEM;

    *q = (osal_queue_t)mq;
    return OSAL_OK;
}

osal_err_t osal_queue_send(osal_queue_t q, const void *item, uint32_t timeout_ms)
{
    rt_err_t rc;

    if (q == OSAL_NULL || item == OSAL_NULL)
        return OSAL_EINVAL;

    /* rt_mq_send 在队列满时直接返回，不阻塞；此处用 rt_mq_send_wait 以支持超时 */
    rc = rt_mq_send_wait((rt_mq_t)q, item, 0, osal_to_tick(timeout_ms));
    if (rc == RT_EOK)
        return OSAL_OK;
    if (rc == -RT_ETIMEOUT || rc == -RT_EFULL)
        return OSAL_ETIMEDOUT;
    return OSAL_EIO;
}

osal_err_t osal_queue_receive(osal_queue_t q, void *buf, uint32_t timeout_ms)
{
    rt_err_t rc;
    rt_size_t size;

    if (q == OSAL_NULL || buf == OSAL_NULL)
        return OSAL_EINVAL;

    size = 0;
    rc = rt_mq_recv((rt_mq_t)q, buf, 0, osal_to_tick(timeout_ms));
    if (rc == RT_EOK)
        return OSAL_OK;
    if (rc == -RT_ETIMEOUT)
        return OSAL_ETIMEDOUT;
    return OSAL_EIO;
}

void osal_queue_delete(osal_queue_t q)
{
    if (q != OSAL_NULL)
        rt_mq_delete((rt_mq_t)q);
}