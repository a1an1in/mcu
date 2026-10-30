/*
 * fake_osal_storage.c - host 单测用的 OSAL 假实现（内存 + 互斥量 + 打印）
 *
 * 与 tests/hw/fake_osal.c 不同的是：这里要测的模块真的会加锁、真的会 malloc，
 * 所以互斥量不能是空实现 —— 它顺便充当「锁使用检查器」：
 *
 *   - 重入：RT-Thread 的互斥量是非递归的，同一线程二次 lock 会死锁。
 *     这里直接记一次违规并返回 OSAL_EBUSY，让测试立刻暴露这种写法；
 *   - 解锁不配对同样记违规。
 *
 * 测试结尾用 fake_osal_violations() / fake_osal_lock_depth() 断言收尾。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <osal.h>

/* ---------------- 内存 ---------------- */

void *osal_malloc(size_t size)
{
    return malloc(size);
}

void osal_free(void *ptr)
{
    free(ptr);
}

size_t osal_mem_available(void)
{
    return 0;
}

/* ---------------- 打印 ---------------- */

void osal_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---------------- 互斥量 ---------------- */

static int s_depth;         /* 0 = 未加锁，1 = 已加锁 */
static int s_violations;

/* 本 fake 只发放一个句柄，凡不是它的一律视为野句柄。
 * NULL 按「非法参数」处理但不记违规，保持与原来一样静默返回。 */
static osal_err_t fake_check_handle(osal_mutex_t m)
{
    if (m == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    if (m != (osal_mutex_t)&s_depth)
    {
        s_violations++;         /* 未初始化 / 已删除 / 别人家的句柄 */
        return OSAL_EINVAL;
    }
    return OSAL_OK;
}

osal_err_t osal_mutex_create(osal_mutex_t *m)
{
    if (m == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    *m = (osal_mutex_t)&s_depth;
    return OSAL_OK;
}

osal_err_t osal_mutex_lock(osal_mutex_t m, uint32_t timeout_ms)
{
    (void)timeout_ms;

    if (fake_check_handle(m) != OSAL_OK)
    {
        return OSAL_EINVAL;
    }
    if (s_depth != 0)
    {
        s_violations++;         /* 重入 = 真机上会死锁 */
        return OSAL_EBUSY;
    }

    s_depth = 1;
    return OSAL_OK;
}

osal_err_t osal_mutex_trylock(osal_mutex_t m)
{
    if (fake_check_handle(m) != OSAL_OK)
    {
        return OSAL_EINVAL;
    }
    /* 与 lock 的关键区别：锁被别人持有是 trylock 的合法结果，
     * 调用方「忙就跳过」是正确的写法，这里只返回 EBUSY，不记违规。 */
    if (s_depth != 0)
    {
        return OSAL_EBUSY;
    }

    s_depth = 1;
    return OSAL_OK;
}

void osal_mutex_unlock(osal_mutex_t m)
{
    if (fake_check_handle(m) != OSAL_OK)
    {
        return;
    }
    if (s_depth != 1)
    {
        s_violations++;         /* 解锁不配对 */
        return;
    }
    s_depth = 0;
}

void osal_mutex_delete(osal_mutex_t m)
{
    (void)fake_check_handle(m); /* 野句柄记违规；NULL 静默忽略 */
}

/* ---------------- 给测试用的观测接口 ---------------- */

int fake_osal_violations(void)
{
    return s_violations;
}

int fake_osal_lock_depth(void)
{
    return s_depth;
}
