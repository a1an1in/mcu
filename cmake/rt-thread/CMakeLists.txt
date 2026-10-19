# =============================================================================
# RT-Thread 内核的 CMake 集成（纯 CMake，不依赖 scons）
# 说明：RT-Thread 官方以 scons 构建，此处手写 CMake 以接入本工程的多 RTOS 体系。
# 覆盖的内核源码 + libcpu(arm/cortex-m3) + klibc，编译宏由 bsp/rtconfig.h 控制。
# =============================================================================

set(RTT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(RTT_ARCH_DIR ${RTT_DIR}/libcpu/arm/cortex-m3)
set(RTT_COMMON_DIR ${RTT_DIR}/libcpu/arm/common)

# -----------------------------------------------------------------------------
# 内核核心源码（单核, !RT_USING_SMP）
#   注意：当 bsp/rtconfig.h 变更（如启用 SMP / SLAB）时，需同步调整下面列表。
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
    # libcpu arm 公共部分：原子操作。（注：不编入 arm/common/divsi3.S —— 它是
    # 供 ARM7/9 用的纯 ARM 态汇编，而 CMake 实际以 -mthumb 编译且 Cortex-M3 禁止
    # ARM 态，故软件除法交由编译器自带 libgcc 的 __aeabi_idiv 提供。）
    ${RTT_COMMON_DIR}/atomic_arm.c
    ${RTT_COMMON_DIR}/div0.c
)

# 创建内核静态库（必须先 add_library，才能对其设置 include 路径）
add_library(rtt_kernel STATIC ${RTT_KERNEL_SRCS})

# -----------------------------------------------------------------------------
# 内核 include 路径
# -----------------------------------------------------------------------------
target_include_directories(rtt_kernel PUBLIC
    ${RTT_DIR}/include
    ${RTT_ARCH_DIR}
    ${RTT_COMMON_DIR}
    ${CMAKE_SOURCE_DIR}/bsp          # 提供 rtconfig.h
)

# 对外导出，供链接时使用
set(RTOS_KERNEL_TARGET rtt_kernel PARENT_SCOPE)