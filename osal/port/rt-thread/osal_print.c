/*
 * osal_print.c - OSAL 打印接口 ↔ RT-Thread 适配层
 *
 * 将 osal_printf 映射到 RT-Thread 的控制台输出（rt_kprintf）。
 * 最终由 bsp/board.c 提供的 rt_hw_console_output 输出到 UART1。
 */
#include <stdarg.h>

#include <osal.h>
#include <rtthread.h>

/* RT-Thread 的 rt_vsnprintf 在公共头中无函数原型（由 klibc 提供），这里显式声明 */
extern int rt_vsnprintf(char *buf, rt_size_t size, const char *fmt, va_list args);

void osal_printf(const char *fmt, ...)
{
    va_list args;
    char buf[RT_CONSOLEBUF_SIZE];

    va_start(args, fmt);
    rt_vsnprintf(buf, sizeof(buf) - 1u, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1u] = '\0';

    rt_kprintf("%s", buf);
}