# =============================================================================
# host 单元测试总入口（宿主 gcc 编译/运行，不需要任何真硬件）
#
#   gpio/  — GPIO 三后端（自研寄存器 / RT-Thread PIN / Zephyr GPIO）的接口一致性
#   hw/    — 业务模块行为，例：modules/key + modules/led 的「按 KEY1 点亮 DS0」
#   storage/ — modules/storage：EEPROM key-value 落盘布局与坏数据兜底
#   i2c/   — drivers/i2c：bit-bang 时序（线上字节序列 / ACK / 重复 START）
#
# 用法：
#   python3 tools/devops.py build --host   # 编译固件 + 编译 host 单测（只编译）
#   python3 tools/devops.py test           # 运行已编译的 host 单测（ctest）
# 手工等价：
#   cmake -S tests -B build/tests -G Ninja
#   cmake --build build/tests
#   ctest --test-dir build/tests --output-on-failure
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(stm32f103_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)

add_subdirectory(gpio)
add_subdirectory(hw)
add_subdirectory(storage)
add_subdirectory(i2c)
