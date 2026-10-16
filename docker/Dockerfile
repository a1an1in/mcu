# =============================================================================
# STM32F103 编译 + 测试一体镜像
# 提供可移植、可复现的完整开发环境：Ubuntu 24.04 + CMake + Ninja +
#   - GCC-ARM(None-EABI) + newlib  → 交叉编译固件（tools/devops.sh build）
#   - 宿主 gcc                     → 在容器内编译并运行 host 单元测试（tools/devops.sh tests）
# 编译与测试共用这一份镜像；宿主机只需安装 Docker（烧录另需 openocd）。
# =============================================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 固定工具链：
#   - host gcc：供 tests 在容器内编译/运行 x86 host 单测
#   - gcc-arm-none-eabi (13.2.1) 由 Ubuntu 24.04 仓库提供：交叉编译固件
#   - libnewlib-arm-none-eabi：补充 newlib 头/库，供 RT-Thread klibc 的
#     rt_vsnprintf_std.c / rt_vsscanf.c 使用的 <stdio.h>/<stdlib.h>/<ctype.h> 等编译。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        git \
        cmake \
        ninja-build \
        # Zephyr 构建硬性依赖：device tree compiler（解析 dts/dtsi）
        device-tree-compiler \
        gcc \
        libc6-dev \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        # --- Zephyr 依赖（--rtos zephyr 用；venv 里按 Zephyr
        #     scripts/requirements-base.txt 装 python 构建依赖） ---
        python3 \
        python3-pip \
        python3-venv \
        python3-dev \
    && rm -rf /var/lib/apt/lists/*

# 验证关键工具就位
RUN gcc --version | head -1 \
    && arm-none-eabi-gcc --version | head -1 \
    && arm-none-eabi-objcopy --version | head -1 \
    && cmake --version | head -1 \
    && ninja --version

WORKDIR /workspace