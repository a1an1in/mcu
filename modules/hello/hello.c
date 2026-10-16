/*
 * hello.c - 示例模块实现
 *
 * 本模块只依赖 OSAL（<osal.h>），不 include 任何 RT-Thread 头文件，
 * 从而验证应用层与 RTOS 完全解耦，可无缝迁移到 Zephyr / FreeRTOS。
 */
#include <osal.h>

#include "hello.h"

#define HELLO_STACK_SIZE   2048u
#define HELLO_PRIORITY     10u
#define HELLO_PERIOD_MS    1000u

static osal_thread_t s_hello_thread = OSAL_NULL;

static void hello_entry(void *param)
{
    uint32_t count = 0u;

    (void)param;

    for (;;)
    {
        osal_printf("[hello] alive, count=%lu\n", (unsigned long)count++);
        osal_thread_sleep_ms(HELLO_PERIOD_MS);
    }
}

void hello_start(void)
{
    osal_err_t rc;

    rc = osal_thread_create(&s_hello_thread, "hello",
                            hello_entry, OSAL_NULL,
                            OSAL_NULL, HELLO_STACK_SIZE, HELLO_PRIORITY);
    if (rc != OSAL_OK)
        osal_printf("[hello] create thread failed: %d\n", (int)rc);
}