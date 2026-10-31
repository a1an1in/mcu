/*
 * shell_history_keys.c - 用板载按键翻 msh 命令历史 <modules/msh/>
 *
 *   KEY_UP（WK_UP / PA0） → 上一条
 *   KEY1  （PE3）         → 下一条
 *
 * 实现方式：把终端方向键的转义序列（↑ = ESC [ A，↓ = ESC [ B）注入 UART
 * 接收缓冲，msh 的历史功能（FINSH_USING_HISTORY）会把它当成真的按了方向键。
 * 这样不用改 shell 的内部结构，终端键盘和板载按键走同一条路径。
 *
 * 注意：
 *   - 用「电平边沿」判断（key_is_down + 上次状态），不消耗 key_pressed 的
 *     事件队列，避免和 main 的自测/按键演示抢事件；
 *   - key_init() 是幂等的，这里调用只是保证消抖扫描线程已启动；
 *   - 线程由 INIT_APP_EXPORT 在 main 之前启动（此时调度器已运行）。
 */
#include <rtthread.h>

#include <osal.h>

#include "key.h"
#include "uart.h"

static const char s_key_up[]   = { 0x1B, '[', 'A' };   /* ↑ */
static const char s_key_down[] = { 0x1B, '[', 'B' };   /* ↓ */

static void shell_keys_entry(void *param)
{
    int up_last = 0;
    int down_last = 0;

    (void)param;

    for (;;)
    {
        int up = key_is_down(KEY_UP);
        int down = key_is_down(KEY_1);

        if ((up != 0) && (up_last == 0))
        {
            (void)uart_inject_rx(s_key_up, sizeof(s_key_up));
        }
        if ((down != 0) && (down_last == 0))
        {
            (void)uart_inject_rx(s_key_down, sizeof(s_key_down));
        }

        up_last = up;
        down_last = down;
        osal_thread_sleep_ms(20);
    }
}

static int shell_keys_start(void)
{
    key_init();     /* 幂等：确保按键扫描线程已经起来 */

    if (osal_thread_create(NULL, "histkey", shell_keys_entry, NULL,
                           NULL, 1024u, 25u) != OSAL_OK)
    {
        return -1;
    }
    return 0;
}
INIT_APP_EXPORT(shell_keys_start);
