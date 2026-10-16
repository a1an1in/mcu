/*
 * osal_print.c - OSAL 打印接口 ↔ Zephyr 适配层
 *
 * 将 osal_printf 映射到 Zephyr 的 printk（vprintk），最终输出到 console 后端
 * （UART/SEGGER 等，由板级设备树与驱动决定）。
 */
#include <stdarg.h>

#include <zephyr/sys/printk.h>

#include <osal.h>

void osal_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}