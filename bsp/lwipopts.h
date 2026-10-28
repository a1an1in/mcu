/*
 * lwipopts.h — 极简版 lwIP 配置（覆盖 RT-Thread 官方 port/lwipopts.h）
 *
 * 为什么覆盖：官方 port 的 lwipopts.h 强依赖 <limits.h>/<fcntl.h>/<sys/ioctl.h>/
 * <sys/time.h>/<sys/errno.h> 等 POSIX headers。我们工程用 RT_USING_NANO（无
 * newlib），只有 RT-Thread 自带 klibc，没有 POSIX 头。
 *
 * 策略：所有 POSIX-dependent 宏全关，手动定义 U16_F/S32_F 等格式串（避免
 * 需要 <inttypes.h> 的 PRIu16），让 lwip 用编译器内置 <stdint.h>。
 */
#ifndef __LWIPOPTS_H__
#define __LWIPOPTS_H__

#include <rtconfig.h>

/* ---------- errno：没有 newlib/POSIX，用 lwip 自带的定义 ----------
 * 注意不能写 LWIP_ERRNO_INCLUDE "lwip/errno.h"：lwip/errno.h 会反过来
 * include 这个宏，自我包含被头文件保护挡住，结果一个 errno 都没有。
 * LWIP_PROVIDE_ERRNO=1 时由 lwip/errno.h 定义整套常量（ENXIO/EAFNOSUPPORT…），
 * 并在 lwip_glue.c 里提供 `int errno;` 的定义。 */
#define LWIP_PROVIDE_ERRNO    1

/* ---------- 格式串宏 ---------- */
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

/* ---------- 告诉 lwip：不要用标准 POSIX headers ---------- */
/* stdint.h 由 arm-none-eabi-gcc 编译器内建提供，不需要 newlib */
#define LWIP_NO_CTYPE_H       1
#define LWIP_SOCKET_SELECT    0
#define LWIP_SOCKET_POLL      0

/* ---------- Basic ---------- */
#define LWIP_IPV4             1
#define LWIP_IPV6             0
#define NO_SYS                0
#define SYS_LIGHTWEIGHT_PROT  1
#define LWIP_SOCKET           1
#define LWIP_NETCONN          1
#define LWIP_HAVE_LOOPIF      0

/* ---------- Memory ---------- */
#define MEMCPY(dst,src,len)   rt_memcpy(dst,src,len)
#define SMEMCPY(dst,src,len)  rt_memcpy(dst,src,len)
#define MEM_ALIGNMENT         4
#define MEMP_MEM_MALLOC       0
#define MEMP_OVERFLOW_CHECK   0

#define MEMP_NUM_PBUF         8
#define MEMP_NUM_RAW_PCB      2
#define MEMP_NUM_UDP_PCB      4
#define MEMP_NUM_TCP_PCB      4
#define MEMP_NUM_TCP_SEG      8
#define MEMP_NUM_NETBUF       4
#define MEMP_NUM_NETCONN      4

/* ---------- Pbuf ----------
 * 每个 pbuf 约 1516B。64KB 的 RAM 里 16 个就是 24KB，太奢侈；
 * 收到一帧就尽快交给 lwIP，6 个足够（CH395 本身只缓存 1 帧）。 */
#define PBUF_POOL_SIZE        6
/* 必须 >= 1514（MACRAW 整帧），否则一帧要跨两个 pbuf，
 * 驱动就得先拷到临时缓冲再 pbuf_take，白白多一次内存拷贝 */
#define PBUF_POOL_BUFSIZE     1520
#define PBUF_LINK_HLEN        14

/* lwIP 自己的堆：TCP 发送/接收缓冲、PBUF_RAM 都从这里出 */
#define MEM_SIZE              (8 * 1024)

/* ---------- TCP ---------- */
#define LWIP_TCP              1
#define TCP_MSS               1460
#define TCP_SND_BUF           (TCP_MSS * 2)
#define TCP_SND_QUEUELEN      8
#define TCP_WND               (TCP_MSS * 2)
#define TCP_MAXRTX            6
#define TCP_SYNMAXRTX         3

/* ---------- ARP ---------- */
#define LWIP_ARP              1
#define ARP_TABLE_SIZE        10
#define ARP_QUEUEING          1

/* ---------- Checksum (软件) ---------- */
#define CHECKSUM_GEN_IP       1
#define CHECKSUM_GEN_UDP      1
#define CHECKSUM_GEN_TCP      1
#define CHECKSUM_GEN_ICMP     1
#define CHECKSUM_CHECK_IP     1
#define CHECKSUM_CHECK_UDP    1
#define CHECKSUM_CHECK_TCP    1
#define CHECKSUM_CHECK_ICMP   1

/* ---------- ICMP/DHCP/DNS/UDP/RAW ---------- */
#define LWIP_ICMP             1
#define LWIP_DHCP             1
#define LWIP_DNS              1
#define LWIP_UDP              1
#define LWIP_RAW              1

/* ---------- IP ---------- */
#define IP_FORWARD            0
#define IP_REASSEMBLY         0
#define IP_FRAG               0

/* ---------- Thread ---------- */
#define TCPIP_THREAD_NAME         "tcpip"
#define TCPIP_THREAD_PRIO         12
#define TCPIP_THREAD_STACKSIZE    2048
#define TCPIP_MBOX_SIZE           8
#define DEFAULT_TCP_RECVMBOX_SIZE 10
#define DEFAULT_UDP_RECVMBOX_SIZE 4
#define DEFAULT_RAW_RECVMBOX_SIZE 2
#define DEFAULT_ACCEPTMBOX_SIZE   8
#define MEMP_NUM_SYS_TIMEOUT      5

/* ---------- Netif ---------- */
#define LWIP_NETIF_HOSTNAME   1
#define LWIP_NETIF_API        1

/* netdev 的 ping 用 setsockopt(SO_RCVTIMEO) 控制超时。
 * lwIP 默认不支持这个选项，setsockopt 会失败 -> recvfrom 永远阻塞，
 * 目标不通时 msh 直接卡死。必须打开。 */
#define LWIP_SO_RCVTIMEO      1

/* ---------- Misc ---------- */
/* 没有 libc 的 rand()，用系统 tick 充数（只用于 DNS TxID 等随机数） */
#define LWIP_RAND()           ((u32_t)rt_tick_get())
#define LWIP_STATS            0
#define LWIP_DEBUG            0

#endif /* __LWIPOPTS_H__ */
