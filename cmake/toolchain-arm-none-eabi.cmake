# CMake 交叉编译工具链：ARM 裸机（none-eabi）
# 用法：cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake
# 需要预先安装 GCC ARM 工具链（apt 安装：gcc-arm-none-eabi 或从 ARM 官网下载）。

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 工具链前缀（默认使用 PATH 中的 arm-none-eabi-*）
set(TOOLCHAIN_PREFIX "arm-none-eabi-" CACHE STRING "GCC ARM 工具链前缀")
set(TOOLCHAIN_PATH "" CACHE STRING "GCC ARM 工具链所在目录（留空则由 find_program 在 PATH 中查找）")

if(TOOLCHAIN_PATH)
    set(CMAKE_C_COMPILER "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}gcc")
    set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}gcc")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}g++")
else()
    set(CMAKE_C_COMPILER "${TOOLCHAIN_PREFIX}gcc")
    set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
endif()

# 强制使用指定编译器（嵌入式环境无原生编译器探测）
set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)