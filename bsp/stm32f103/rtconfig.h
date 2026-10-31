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
/* 内核对象名上限。RT-Thread 的断言是 obj_name_len <= RT_NAME_MAX - 1，
 * 而 OSAL 适配层给内核对象起的名字是 "osal_mutex"（10 字符，见
 * osal/port/rt-thread/osal_mutex.c），8 装不下，rt_object_allocate() 会直接断言。
 * 12 与 RT-Thread 官方 bsp 的常见取值一致：足够放到 "osal_mutex"，
 * 每个内核对象只多占 4 字节（name[] 由 8 变 12）。 */
#define RT_NAME_MAX          12
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
#define RT_DEBUGING_INIT           /* 打印每个 RT-Thread 组件自动初始化的名字 */

/* ===== 线程间通信 ===== */
#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE

/* ===== 内存管理（经典小内存堆） ===== */
#define RT_USING_HEAP
#define RT_USING_SMALL_MEM
#define RT_USING_SMALL_MEM_AS_HEAP

/* ===== 控制台：rt_kprintf 依赖 RT_USING_CONSOLE =====
 * RT_CONSOLEBUF_SIZE 是**单次** rt_kprintf 的输出上限（超出直接截断，
 * 连一半的多字节汉字都会被切断）。命令帮助、文件内容打印很容易超，
 * 所以放大到 512；长内容仍建议分次打印。 */
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE      512

/* ===== libc 策略：使用内核自带 klibc，不依赖外部 libc =====
 * 定义 RT_USING_NANO：让 rttypes.h 跳过 <sys/types.h>/<sys/errno.h>，
 * 从而无需系统 newlib 头库。适用场景：容器/裸机 gcc-arm-none-eabi
 * （不带 newlib）即可编译本工程；rt_kprintf/mem* 由 src/klibc 提供。 */
#define RT_USING_NANO

/* ===== 用户主线程 ===== */
#define RT_USING_USER_MAIN
#define RT_MAIN_THREAD_STACK_SIZE  4096
#define RT_MAIN_THREAD_PRIORITY    10

/* ===== FINSH Shell（msh 交互式命令行）===== */
#define RT_USING_COMPONENTS_INIT   /* 启用 INIT_EXPORT 自动初始化 */
#define RT_USING_FINSH             /* 启用 FINSH shell 组件 */
#define FINSH_USING_MSH            /* 使用 msh 模式（交互式） */
#define FINSH_USING_SYMTAB         /* 支持 MSH_CMD_EXPORT 导出命令表 */
#define FINSH_USING_HISTORY        /* 命令历史：终端 ↑/↓ 或板载按键都走它 */
#define FINSH_HISTORY_LINES     8  /* 保留最近 8 条（每条 FINSH_CMD_SIZE 字节） */

/* ===== 网络：lwIP v2.1.2 + netdev ===== */
#define RT_USING_LWIP              /* 启用 lwIP TCP/IP 协议栈 */
#define RT_USING_DEVICE            /* 让 rt_device_t 有定义（sys_arch.c/ethernetif.h 需要） */
#define RT_USING_LWIP_VER_NUM  0x20102  /* lwIP 版本 = v2.1.2 */
#define RT_LWIP_TCP                1
#define RT_LWIP_UDP                1
#define RT_LWIP_RAW                1
#define RT_LWIP_ICMP               1
#define RT_LWIP_DHCP               1
#define RT_LWIP_DNS                1
#define RT_USING_NETDEV            /* 启用 RT-Thread netdev 抽象 */
#define NETDEV_IPV4                1   /* netdev_ipaddr.h 靠这两个宏选 IPv4/IPv6 分支 */
#define NETDEV_IPV6                0
#define NETDEV_USING_IFCONFIG      /* 启用 ifconfig 命令 */
#define NETDEV_USING_PING          /* 启用 ping 命令 */
#define NETDEV_USING_NETSTAT       /* 启用 netstat 命令 */
#define RT_LWIP_PBUF_NUM           16
#define RT_LWIP_RAW_PCB_NUM        2
#define RT_LWIP_UDP_PCB_NUM        4
#define RT_LWIP_TCP_PCB_NUM        4
#define RT_LWIP_TCP_SND_BUF        (2 * TCP_MSS)
#define RT_LWIP_TCP_WND            (2 * TCP_MSS)
#define RT_MEMP_NUM_NETCONN        4
#define RT_LWIP_TCPTHREAD_PRIORITY 12
#define RT_LWIP_TCPTHREAD_MBOX_SIZE 8
#define RT_LWIP_TCPTHREAD_STACKSIZE 2048
/* erx/etx 线程（ethernetif.c 里创建）。不定义时 port 会退化成 0x90=144，
 * 超过 RT_THREAD_PRIORITY_MAX(32)，rt_thread_init 直接断言挂掉。
 * 取 13：比 tcpip(12) 略低，收到包先让协议栈跑。 */
#define RT_LWIP_ETHTHREAD_PRIORITY  13
/* 打开 netdev 的 ping 实现（否则 ping 命令表项是 NULL） */
#define RT_LWIP_USING_PING

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