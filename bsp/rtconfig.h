/*
 * rtconfig.h - RT-Thread 内核配置（最小可运行集）
 *
 * 说明：本文件由本工程 bsp 维护，覆盖 RT-Thread 内核的编译开关。
 * 为保持骨架小巧、便于在未安装完整外设驱动时跑通，这里仅启用内核核心特性
 * （线程/IPC/经典内存堆/控制台），并使用 board.c 中寄存器级的 UART1 输出。
 * 后续按需从官方 bsp（rtos/rt-thread/bsp/stm32/stm32f103-blue-pill/rtconfig.h）
 * 合并更多外设/组件配置。
 */
#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* ===== SoC ===== */
#define SOC_STM32F103
#define SOC_FAMILY_STM32
#define SOC_SERIES_STM32F1

/* ===== 内核基础 ===== */
#define RT_NAME_MAX          8
#define RT_CPUS_NR           1
#define RT_ALIGN_SIZE        8
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX  32
#define RT_TICK_PER_SECOND   1000

#define RT_USING_HOOK
#define RT_HOOK_USING_FUNC_PTR
#define RT_USING_IDLE_HOOK
#define RT_IDLE_HOOK_LIST_SIZE   4
#define IDLE_THREAD_STACK_SIZE   256

/* ===== 内核扩展 ===== */
#define RT_USING_DEBUG
#define RT_DEBUGING_ASSERT

/* ===== 线程间通信 ===== */
#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE

/* ===== 内存管理（经典小内存堆） ===== */
#define RT_USING_HEAP

/* ===== 控制台：rt_kprintf 依赖 RT_USING_CONSOLE ===== */
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE      256

/* ===== libc 策略：使用内核自带 klibc，不依赖外部 libc =====
 * 定义 RT_USING_NANO：让 rttypes.h 跳过 <sys/types.h>/<sys/errno.h>，
 * 从而无需系统 newlib 头库。适用场景：容器/裸机 gcc-arm-none-eabi
 * （不带 newlib）即可编译本工程；rt_kprintf/mem* 由 src/klibc 提供。 */
#define RT_USING_NANO

/* ===== 用户主线程 ===== */
#define RT_USING_USER_MAIN
#define RT_MAIN_THREAD_STACK_SIZE  2048
#define RT_MAIN_THREAD_PRIORITY    10

/* ===== 版本 & 调测 ===== */
#define RT_VER_NUM               0x50202
#define RT_BACKTRACE_LEVEL_MAX_NR 16

/* ===== 处理器体系 ===== */
#define RT_USING_HW_ATOMIC
#define RT_USING_CPU_FFS
#define ARCH_ARM
#define ARCH_ARM_CORTEX_M
#define ARCH_ARM_CORTEX_M3

#endif /* RT_CONFIG_H__ */