/*
 * osal_sem.c - OSAL 信号量接口 ↔ Zephyr 适配层
 *
 * Zephyr 的 k_sem 是"原地初始化"的内核对象，为使 OSAL 获得动态创建/删除
 * 语义，用 k_malloc 分配 struct k_sem 后 k_sem_init。释放时直接 k_free。
 */
#include <stdint.h>

#include <zephyr/kernel.h>

#include <osal.h>

/* 毫秒超时 → Zephyr k_timeout_t */
static k_timeout_t osal_to_timeout(uint32_t ms)
{
    if (ms == OSAL_WAIT_FOREVER)
        return K_FOREVER;
    if (ms == OSAL_NO_WAIT)
        return K_NO_WAIT;
    return K_MSEC(ms);
}

osal_err_t osal_sem_create(osal_sem_t *s, uint32_t initial)
{
    struct k_sem *sem;

    if (s == OSAL_NULL)
        return OSAL_EINVAL;

    sem = k_malloc(sizeof(*sem));
    if (sem == OSAL_NULL)
        return OSAL_ENOMEM;

    if (k_sem_init(sem, initial, K_SEM_MAX_LIMIT) != 0)
    {
        k_free(sem);
        return OSAL_EIO;
    }

    *s = (osal_sem_t)sem;
    return OSAL_OK;
}

void osal_sem_give(osal_sem_t s)
{
    if (s != OSAL_NULL)
        k_sem_give((struct k_sem *)s);
}

osal_err_t osal_sem_take(osal_sem_t s, uint32_t timeout_ms)
{
    int rc;

    if (s == OSAL_NULL)
        return OSAL_EINVAL;

    rc = k_sem_take((struct k_sem *)s, osal_to_timeout(timeout_ms));
    return (rc == 0) ? OSAL_OK : OSAL_ETIMEDOUT;
}

void osal_sem_delete(osal_sem_t s)
{
    if (s != OSAL_NULL)
        k_free(s);
}