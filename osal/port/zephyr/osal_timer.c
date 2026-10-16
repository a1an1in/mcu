/*
 * osal_timer.c - OSAL 软件定时器接口 ↔ Zephyr 适配层
 *
 * 映射到 Zephyr 的 k_timer。因为 OSAL 把"周期"放 create、把"启动"放 start，
 * 而 k_timer 是 start 时传 duration/period，故用一个运行时 ctx(struct) 记录回调、
 * 参数与周期，删除时一并释放。
 */
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>

#include <osal.h>

/* 定时器运行时上下文：存 OSAL 回调、参数与启动参数 */
typedef struct
{
    osal_timer_cb  cb;
    void          *param;
    k_timeout_t    duration;
    k_timeout_t    period;   /* 周期定时：K_MSEC(p)；一次性：K_NO_WAIT */
} zos_timer_ctx_t;

/* Zephir 到期回调(struct k_timer*) → OSAL 回调(param) */
static void zos_timer_expiry(struct k_timer *timer)
{
    zos_timer_ctx_t *ctx = (zos_timer_ctx_t *)k_timer_user_data_get(timer);

    if (ctx != OSAL_NULL && ctx->cb != OSAL_NULL)
        ctx->cb(ctx->param);
}

osal_err_t osal_timer_create(osal_timer_t *t, const char *name,
                             osal_timer_cb cb, void *param,
                             uint32_t period_ms, bool periodic)
{
    struct k_timer *timer;
    zos_timer_ctx_t *ctx;

    (void)name;

    if (t == OSAL_NULL || cb == OSAL_NULL || period_ms == 0)
        return OSAL_EINVAL;

    timer = k_malloc(sizeof(*timer));
    if (timer == OSAL_NULL)
        return OSAL_ENOMEM;

    ctx = k_malloc(sizeof(*ctx));
    if (ctx == OSAL_NULL)
    {
        k_free(timer);
        return OSAL_ENOMEM;
    }

    ctx->cb       = cb;
    ctx->param    = param;
    ctx->duration = K_MSEC(period_ms);
    ctx->period   = periodic ? K_MSEC(period_ms) : K_NO_WAIT;

    k_timer_init(timer, zos_timer_expiry, NULL);
    k_timer_user_data_set(timer, ctx);

    *t = (osal_timer_t)timer;
    return OSAL_OK;
}

osal_err_t osal_timer_start(osal_timer_t t)
{
    struct k_timer *timer = (struct k_timer *)t;
    zos_timer_ctx_t *ctx;

    if (t == OSAL_NULL)
        return OSAL_EINVAL;

    ctx = (zos_timer_ctx_t *)k_timer_user_data_get(timer);
    k_timer_start(timer, ctx->duration, ctx->period);
    return OSAL_OK;
}

osal_err_t osal_timer_stop(osal_timer_t t)
{
    if (t == OSAL_NULL)
        return OSAL_EINVAL;

    k_timer_stop((struct k_timer *)t);
    return OSAL_OK;
}

void osal_timer_delete(osal_timer_t t)
{
    struct k_timer *timer = (struct k_timer *)t;

    if (t != OSAL_NULL)
    {
        k_timer_stop(timer);
        k_free(k_timer_user_data_get(timer));
        k_free(timer);
    }
}