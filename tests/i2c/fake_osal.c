/*
 * fake_osal.c - host 单测用的 OSAL 假实现（只做互斥量）
 *
 * i2c_core.c 的总线锁走 OSAL（osal_mutex_*）。本测试关心的是线上时序，
 * 不是加解锁行为，所以这里给最小实现：能建、能加、能解，但没有阻塞语义。
 * （锁的行为由 tests/spi 的 fake_osal.c 负责验证。）
 */
#include <osal.h>

static char s_obj;

osal_err_t osal_mutex_create(osal_mutex_t *m)
{
    if (m == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    *m = (osal_mutex_t)&s_obj;
    return OSAL_OK;
}

osal_err_t osal_mutex_lock(osal_mutex_t m, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (m != OSAL_NULL) ? OSAL_OK : OSAL_EINVAL;
}

void osal_mutex_unlock(osal_mutex_t m)
{
    (void)m;
}

void osal_mutex_delete(osal_mutex_t m)
{
    (void)m;
}
