/*
 * osal.h - OS 抽象层统一入口
 *
 * 应用层应只 include 本文件（或需要的子头文件），
 * 使代码与具体 RTOS 完全解耦，便于未来迁移 Zephyr / FreeRTOS。
 */
#ifndef OSAL_H__
#define OSAL_H__

#include "osal_common.h"
#include "osal_thread.h"
#include "osal_sem.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_timer.h"
#include "osal_memory.h"
#include "osal_print.h"

#endif /* OSAL_H__ */