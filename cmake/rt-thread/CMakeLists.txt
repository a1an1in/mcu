# =============================================================================
# RT-Thread 内核的 CMake 集成（纯 CMake，不依赖 scons）
# 说明：RT-Thread 官方以 scons 构建，此处手写 CMake 以接入本工程的多 RTOS 体系。
# 覆盖的内核源码 + libcpu(arm/cortex-m3) + klibc + FINSH + lwIP v2.1.2 + netdev，
# 编译宏由 bsp/rtconfig.h 控制。
# =============================================================================

set(RTT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(RTT_ARCH_DIR ${RTT_DIR}/libcpu/arm/cortex-m3)
set(RTT_COMMON_DIR ${RTT_DIR}/libcpu/arm/common)
set(RTT_FINSH_DIR ${RTT_DIR}/components/finsh)
set(RTT_DRV_DIR ${RTT_DIR}/components/drivers)

# lwIP v2.1.2 路径
set(RTT_LWIP_DIR ${RTT_DIR}/components/net/lwip)
set(RTT_LWIP_VER_DIR ${RTT_LWIP_DIR}/lwip-2.1.2)
set(RTT_LWIP_PORT_DIR ${RTT_LWIP_DIR}/port)
set(RTT_LWIP_NETIF_DIR ${RTT_LWIP_VER_DIR}/src/netif)
set(RTT_LWIP_CORE_DIR ${RTT_LWIP_VER_DIR}/src/core)
set(RTT_LWIP_API_DIR ${RTT_LWIP_VER_DIR}/src/api)
set(RTT_NETDEV_DIR ${RTT_DIR}/components/net/netdev)

# -----------------------------------------------------------------------------
# 内核核心源码（单核, !RT_USING_SMP）
# -----------------------------------------------------------------------------
set(RTT_KERNEL_SRCS
    ${RTT_DIR}/src/clock.c
    ${RTT_DIR}/src/components.c
    ${RTT_DIR}/src/cpu_up.c
    ${RTT_DIR}/src/defunct.c
    ${RTT_DIR}/src/idle.c
    ${RTT_DIR}/src/ipc.c
    ${RTT_DIR}/src/irq.c
    # klibc：内核自带最小 libc（rt_kprintf/mem* 等）
    ${RTT_DIR}/src/klibc/kerrno.c
    ${RTT_DIR}/src/klibc/kstdio.c
    ${RTT_DIR}/src/klibc/kstring.c
    ${RTT_DIR}/src/klibc/rt_vsnprintf_std.c
    ${RTT_DIR}/src/klibc/rt_vsscanf.c
    ${RTT_DIR}/src/kservice.c
    ${RTT_DIR}/src/mem.c
    ${RTT_DIR}/src/object.c
    ${RTT_DIR}/src/scheduler_comm.c
    ${RTT_DIR}/src/scheduler_up.c
    ${RTT_DIR}/src/signal.c
    ${RTT_DIR}/src/thread.c
    ${RTT_DIR}/src/timer.c
    # Cortex-M3 移植
    ${RTT_ARCH_DIR}/cpuport.c
    ${RTT_ARCH_DIR}/context_gcc.S
    ${RTT_COMMON_DIR}/atomic_arm.c
    ${RTT_COMMON_DIR}/div0.c
    # FINSH shell
    ${RTT_FINSH_DIR}/shell.c
    ${RTT_FINSH_DIR}/msh.c
    ${RTT_FINSH_DIR}/msh_parse.c
    ${RTT_FINSH_DIR}/msh_file.c
    ${RTT_FINSH_DIR}/cmd.c
    # 设备框架（RT_USING_DEVICE）：lwIP 的 eth_device/completion 依赖
    ${RTT_DRV_DIR}/core/device.c
    ${RTT_DRV_DIR}/ipc/completion_comm.c
    ${RTT_DRV_DIR}/ipc/completion_up.c
)

# -----------------------------------------------------------------------------
# lwIP v2.1.2 核心源码（RT_USING_LWIP）
# -----------------------------------------------------------------------------
set(RTT_LWIP_SRCS
    # core
    # 注意：不要编译 src/core/mem.c —— RT-Thread 的 port/sys_arch.c 已把
    # mem_malloc/mem_free/mem_calloc 映射到 rt_malloc/rt_free，两者会重复定义。
    ${RTT_LWIP_CORE_DIR}/memp.c
    ${RTT_LWIP_CORE_DIR}/pbuf.c
    ${RTT_LWIP_CORE_DIR}/netif.c
    ${RTT_LWIP_CORE_DIR}/timeouts.c
    ${RTT_LWIP_CORE_DIR}/tcp.c
    ${RTT_LWIP_CORE_DIR}/tcp_in.c
    ${RTT_LWIP_CORE_DIR}/tcp_out.c
    ${RTT_LWIP_CORE_DIR}/udp.c
    ${RTT_LWIP_CORE_DIR}/raw.c
    ${RTT_LWIP_CORE_DIR}/stats.c
    ${RTT_LWIP_CORE_DIR}/def.c
    ${RTT_LWIP_CORE_DIR}/sys.c
    ${RTT_LWIP_CORE_DIR}/init.c
    ${RTT_LWIP_CORE_DIR}/ip.c
    ${RTT_LWIP_CORE_DIR}/inet_chksum.c
    ${RTT_LWIP_CORE_DIR}/dns.c
    ${RTT_LWIP_CORE_DIR}/ipv4/ip4_addr.c
    ${RTT_LWIP_CORE_DIR}/ipv4/etharp.c
    ${RTT_LWIP_CORE_DIR}/ipv4/icmp.c
    ${RTT_LWIP_CORE_DIR}/ipv4/dhcp.c
    ${RTT_LWIP_CORE_DIR}/ipv4/autoip.c
    ${RTT_LWIP_CORE_DIR}/ipv4/igmp.c
    ${RTT_LWIP_CORE_DIR}/ipv4/ip4.c
    # api
    ${RTT_LWIP_API_DIR}/tcpip.c
    ${RTT_LWIP_API_DIR}/sockets.c
    ${RTT_LWIP_API_DIR}/netifapi.c
    ${RTT_LWIP_API_DIR}/netdb.c
    ${RTT_LWIP_API_DIR}/netbuf.c
    ${RTT_LWIP_API_DIR}/if_api.c
    ${RTT_LWIP_API_DIR}/err.c
    ${RTT_LWIP_API_DIR}/api_msg.c
    ${RTT_LWIP_API_DIR}/api_lib.c
    # netif
    ${RTT_LWIP_NETIF_DIR}/ethernet.c
    # apps：netdev 的 ping 命令要用 lwip_ping_send/lwip_ping_recv
    ${RTT_LWIP_VER_DIR}/src/apps/ping/ping.c
    # RT-Thread port 层
    ${RTT_LWIP_PORT_DIR}/sys_arch.c
    ${RTT_LWIP_PORT_DIR}/ethernetif.c
)

# -----------------------------------------------------------------------------
# RT-Thread netdev 抽象（RT_USING_NETDEV）
# -----------------------------------------------------------------------------
set(RTT_NETDEV_SRCS
    ${RTT_NETDEV_DIR}/src/netdev.c
    ${RTT_NETDEV_DIR}/src/netdev_ipaddr.c
)

# -----------------------------------------------------------------------------
# 合并所有源码，创建内核静态库
# -----------------------------------------------------------------------------
set(RTT_ALL_SRCS
    ${RTT_KERNEL_SRCS}
    ${RTT_LWIP_SRCS}
    ${RTT_NETDEV_SRCS}
)

add_library(rtt_kernel STATIC ${RTT_ALL_SRCS})

# -----------------------------------------------------------------------------
# 内核 include 路径
# -----------------------------------------------------------------------------
target_include_directories(rtt_kernel PUBLIC
    ${RTT_DIR}/include
    ${RTT_ARCH_DIR}
    ${RTT_COMMON_DIR}
    ${RTT_FINSH_DIR}
    # 设备框架头（ipc/completion.h、drivers/*.h）
    ${RTT_DRV_DIR}/include
    # lwIP v2.1.2 headers
    ${RTT_LWIP_VER_DIR}/src/include
    # lwIP port headers (sys_arch.h, lwipopts.h)
    ${RTT_LWIP_PORT_DIR}
    ${RTT_LWIP_PORT_DIR}/arch
    # netdev headers
    ${RTT_NETDEV_DIR}/include
    ${RTT_NETDEV_DIR}/include/arpa
    ${CMAKE_SOURCE_DIR}/bsp          # 提供 rtconfig.h
)

# lwIP 编译选项：避免某些 glibc 检查，我们用 klibc/nano
target_compile_definitions(rtt_kernel PRIVATE
    LWIP_NETIF_HOSTNAME=1
)

# 对外导出，供链接时使用
set(RTOS_KERNEL_TARGET rtt_kernel PARENT_SCOPE)
