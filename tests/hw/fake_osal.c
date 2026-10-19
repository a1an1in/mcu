/*
 * fake_osal.c - host 单测用的 OSAL 假实现
 *
 * 只实现 modules/led、modules/key 真正用到的三个接口：
 *   osal_thread_create / osal_thread_sleep_ms / osal_printf
 *
 * 关键点：osal_thread_create() 不真的起线程（宿主上没有 RTOS 调度器），
 * 直接返回成功即可 —— 模块里需要被驱动的那部分逻辑（key_poll_once()）
 * 由测试自己循环调用，从而不依赖任何 RTOS 也能测时序与消抖。
 */
#include <osal.h>

#include <stdarg.h>
#include <stdio.h>

osal_err_t osal_thread_create(osal_thread_t *t, const char *name,
                              osal_thread_entry entry, void *param,
                              void *stack, size_t stack_size, uint32_t priority)
{
    (void)name;
    (void)entry;
    (void)param;
    (void)stack;
    (void)stack_size;
    (void)priority;

    if (t != OSAL_NULL)
        *t = OSAL_NULL;

    return OSAL_OK;
}

void osal_thread_delete(osal_thread_t t)
{
    (void)t;
}

void osal_thread_yield(void)
{
}

void osal_thread_sleep_ms(uint32_t ms)
{
    (void)ms;
}

osal_thread_t osal_thread_self(void)
{
    return OSAL_NULL;
}

void osal_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
