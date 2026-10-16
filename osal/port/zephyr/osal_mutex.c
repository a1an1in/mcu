/*
 * osal_mutex.c - OSAL 互斥量接口 ↔ Zephyr 适配层
 *
 * 同信号量：动态分配 struct k_mutex + k_mutex_init 实现动态创建/删除。
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

osal_err_t osal_mutex_create(osal_mutex_t *m)
{
    struct k_mutex *mx;

    if (m == OSAL_NULL)
        return OSAL_EINVAL;

    mx = k_malloc(sizeof(*mx));
    if (mx == OSAL_NULL)
        return OSAL_ENOMEM;

    if (k_mutex_init(mx) != 0)
    {
        k_free(mx);
        return OSAL_EIO;
    }

    *m = (osal_mutex_t)mx;
    return OSAL_OK;
}

osal_err_t osal_mutex_lock(osal_mutex_t m, uint32_t timeout_ms)
{
    int rc;

    if (m == OSAL_NULL)
        return OSAL_EINVAL;

    rc = k_mutex_lock((struct k_mutex *)m, osal_to_timeout(timeout_ms));
    return (rc == 0) ? OSAL_OK : OSAL_ETIMEDOUT;
}

osal_err_t osal_mutex_trylock(osal_mutex_t m)
{
    return osal_mutex_lock(m, OSAL_NO_WAIT);
}

void osal_mutex_unlock(osal_mutex_t m)
{
    if (m != OSAL_NULL)
        k_mutex_unlock((struct k_mutex *)m);
}

void osal_mutex_delete(osal_mutex_t m)
{
    if (m != OSAL_NULL)
        k_free(m);
}