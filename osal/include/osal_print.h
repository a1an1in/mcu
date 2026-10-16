/*
 * osal_print.h - 轮询/调试打印抽象接口
 */
#ifndef OSAL_PRINT_H__
#define OSAL_PRINT_H__

#include "osal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 格式化打印（类似 printf），由各 RTOS 后端映射到其控制台输出 */
void osal_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_PRINT_H__ */