/*
 * osal_thread.c - OSAL 线程接口 ↔ Zephyr 适配层
 *
 * 映射关系（基于 Zephyr 4.4 API）：
 *   osal_thread_create ↔ k_malloc{k_thread} + k_thread_stack_alloc +
 *                        k_thread_create(..., K_NO_WAIT)
 *   osal_thread_delete ↔ k_thread_abort + k_thread_stack_free + k_free
 *   osal_thread_yield  ↔ k_yield
 *   osal_thread_sleep_ms↔ k_msleep
 *   osal_thread_self   ↔ k_current_get
 *
 * Zephyr 的 k_thread_create 需要调用方提供栈。为支持 OSAL"stack 可传 NULL"
 * 的语义，本层用 k_thread_stack_alloc() 动态分配（需 CONFIG_THREAD_STACK_ALLOCATOR=y）。
 * 用零号包装结构(container-of)记录"栈归属"，便于 delete 时正确释放。
 */
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <osal.h>

/* 线程包装结构：以容器方式把 k_thread 与"栈是否自管"绑在一起 */
typedef struct
{
    struct k_thread     kth;
    k_thread_stack_t   *stack;   /* 当前使用的栈 */
    bool                owns_stack; /* true=本层动态分配，delete 时应归还 */
} zos_thread_t;

/* Zephyr 线程入口(p1,p2,p3) → OSAL 入口(param) */
static void zos_thread_entry(void *p1, void *p2, void *p3)
{
    osal_thread_entry entry;

    (void)p3;
    entry = (osal_thread_entry)(uintptr_t)p2;
    entry(p1);
}

/*
 * OSAL 优先级（数值越小优先级越高）→ Zephyr 抢占优先级。
 * Zephyr 4.4 抢占优先级为非负值：0 最高，NUM_PREEMPT_PRIORITIES-1 最低。
 */
static int zos_to_prio(uint32_t priority)
{
    int maxp = CONFIG_NUM_PREEMPT_PRIORITIES - 1;
    int np;

    np = (priority > (uint32_t)maxp) ? maxp : (int)priority;
    return K_PRIO_PREEMPT(np);
}

osal_err_t osal_thread_create(osal_thread_t *t, const char *name,
                              osal_thread_entry entry, void *param,
                              void *stack, size_t stack_size, uint32_t priority)
{
    zos_thread_t *zth;
    k_thread_stack_t *stk;

    (void)name; /* Zephyr k_thread_create 不接收名称；如需要在 debug 处登记 */

    if (t == OSAL_NULL || entry == OSAL_NULL || stack_size == 0)
        return OSAL_EINVAL;

    zth = k_malloc(sizeof(*zth));
    if (zth == OSAL_NULL)
        return OSAL_ENOMEM;

    if (stack != OSAL_NULL)
    {
        stk = (k_thread_stack_t *)stack;
        zth->owns_stack = false;
    }
    else
    {
        stk = k_thread_stack_alloc(stack_size, 0);
        if (stk == OSAL_NULL)
        {
            k_free(zth);
            return OSAL_ENOMEM;
        }
        zth->owns_stack = true;
    }
    zth->stack = stk;

    /* delay=K_NO_WAIT：创建后立即调度执行 */
    k_thread_create(&zth->kth, stk, stack_size, zos_thread_entry,
                    param, (void *)(uintptr_t)entry, OSAL_NULL,
                    zos_to_prio(priority), 0, K_NO_WAIT);

    *t = (osal_thread_t)&zth->kth;
    return OSAL_OK;
}

void osal_thread_delete(osal_thread_t t)
{
    zos_thread_t *zth;

    if (t == OSAL_NULL)
        return;

    zth = CONTAINER_OF((struct k_thread *)t, zos_thread_t, kth);
    k_thread_abort(&zth->kth);
    if (zth->owns_stack && zth->stack != OSAL_NULL)
        k_thread_stack_free(zth->stack);
    k_free(zth);
}

void osal_thread_yield(void)
{
    k_yield();
}

void osal_thread_sleep_ms(uint32_t ms)
{
    k_msleep(ms);
}

osal_thread_t osal_thread_self(void)
{
    return (osal_thread_t)k_current_get();
}