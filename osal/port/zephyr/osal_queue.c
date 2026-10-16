/*
 * osal_queue.c - OSAL 消息队列接口 ↔ Zephyr 适配层
 *
 * 映射到 Zephyr 的 k_msgq。用 k_msgq_alloc_init()（内部 k_malloc 分配环形缓冲）
 * 实现动态创建；删除时 k_msgq_cleanup 释放缓冲再 k_free 对象本身。
 */
#include <stdint.h>

#include <zephyr/kernel.h>

#include <osal.h>

static k_timeout_t osal_to_timeout(uint32_t ms)
{
    if (ms == OSAL_WAIT_FOREVER)
        return K_FOREVER;
    if (ms == OSAL_NO_WAIT)
        return K_NO_WAIT;
    return K_MSEC(ms);
}

osal_err_t osal_queue_create(osal_queue_t *q, uint32_t item_size, uint32_t item_count)
{
    struct k_msgq *mq;

    if (q == OSAL_NULL || item_size == 0 || item_count == 0)
        return OSAL_EINVAL;

    mq = k_malloc(sizeof(*mq));
    if (mq == OSAL_NULL)
        return OSAL_ENOMEM;

    if (k_msgq_alloc_init(mq, item_size, item_count) != 0)
    {
        k_free(mq);
        return OSAL_ENOMEM;
    }

    *q = (osal_queue_t)mq;
    return OSAL_OK;
}

osal_err_t osal_queue_send(osal_queue_t q, const void *item, uint32_t timeout_ms)
{
    int rc;

    if (q == OSAL_NULL || item == OSAL_NULL)
        return OSAL_EINVAL;

    rc = k_msgq_put((struct k_msgq *)q, item, osal_to_timeout(timeout_ms));
    return (rc == 0) ? OSAL_OK : OSAL_ETIMEDOUT;
}

osal_err_t osal_queue_receive(osal_queue_t q, void *buf, uint32_t timeout_ms)
{
    int rc;

    if (q == OSAL_NULL || buf == OSAL_NULL)
        return OSAL_EINVAL;

    rc = k_msgq_get((struct k_msgq *)q, buf, osal_to_timeout(timeout_ms));
    return (rc == 0) ? OSAL_OK : OSAL_ETIMEDOUT;
}

void osal_queue_delete(osal_queue_t q)
{
    if (q != OSAL_NULL)
    {
        k_msgq_cleanup((struct k_msgq *)q);
        k_free(q);
    }
}