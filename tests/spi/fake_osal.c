/*
 * fake_osal.c - host 单测用的 OSAL 假实现（只做互斥量）
 *
 * spi_core.c 的总线锁走 OSAL（osal_mutex_create/lock/unlock），这里给它一个
 * 可控的实现：
 *   - 统计「建了几把锁、加了几次、解了几次」—— 用来断言 spi_sync 在成功、
 *     失败、setup 报错三条路径上都成对加解锁；
 *   - fake_osal_set_fail_lock() 可以模拟「拿不到锁」，验证 SPI_ERR_BUSY 分支。
 *
 * 故意不用空实现：如果锁根本没被调用，计数就是 0，测试会立刻发现。
 */
#include <osal.h>

#define FAKE_MAX_MUTEX  8

static char s_obj[FAKE_MAX_MUTEX];      /* 只为给每个句柄一个唯一地址 */
static int  s_obj_n;

static int s_creates;
static int s_locks;
static int s_unlocks;
static int s_fail_lock;

osal_err_t osal_mutex_create(osal_mutex_t *m)
{
    if (m == OSAL_NULL || s_obj_n >= FAKE_MAX_MUTEX)
    {
        return OSAL_EINVAL;
    }

    *m = (osal_mutex_t)&s_obj[s_obj_n++];
    s_creates++;
    return OSAL_OK;
}

osal_err_t osal_mutex_lock(osal_mutex_t m, uint32_t timeout_ms)
{
    (void)timeout_ms;

    if (m == OSAL_NULL)
    {
        return OSAL_EINVAL;
    }
    if (s_fail_lock != 0)
    {
        return OSAL_EBUSY;
    }
    s_locks++;
    return OSAL_OK;
}

void osal_mutex_unlock(osal_mutex_t m)
{
    if (m == OSAL_NULL)
    {
        return;
    }
    s_unlocks++;
}

void osal_mutex_delete(osal_mutex_t m)
{
    (void)m;
}

/* ---------------- 给测试用的观测 / 注入接口 ---------------- */

void fake_osal_reset_counters(void)
{
    s_locks = 0;
    s_unlocks = 0;
    s_fail_lock = 0;
}

int fake_osal_create_count(void)
{
    return s_creates;
}

int fake_osal_lock_count(void)
{
    return s_locks;
}

int fake_osal_unlock_count(void)
{
    return s_unlocks;
}

void fake_osal_set_fail_lock(int fail)
{
    s_fail_lock = fail;
}
