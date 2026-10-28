/*
 * lwip_glue.c — lwIP 与板级的胶水层（无 newlib/POSIX 环境）
 *
 * 本工程用 RT_USING_NANO（只有内核自带 klibc，没有 newlib 的 <errno.h>），
 * 因此 lwIP 配置为 LWIP_PROVIDE_ERRNO=1：错误码常量由 lwip/errno.h 提供，
 * 但 lwIP 只写了 `extern int errno;`，变量本身需要在这里定义一份。
 */
#include <rtthread.h>

int errno;
