# =============================================================================
# GPIO 后端 host 单元测试（独立工程，使用 host gcc）
# 用桩替换 RT-Thread / Zephyr 头，使两个"操作系统自带驱动"的适配实现
# 能在本机真实编译并运行，验证其与 gpio.h 接口的一致性（模块零改动）。
# 另对自研 stm32f1_gpio.c 做 -fsyntax-only 编译冒烟（host 上写寄存器不安全）。
# 用法: cmake -S tests/gpio -B build/tests … 或 python3 tools/devops.py test
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(gpio_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)

set(GPIO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/gpio")

# ---- RT-Thread 后端适配测试（桩替换 rt_pin_*） ----
add_executable(test_rtthread_gpio
    test_rtthread_gpio.c
    ${GPIO_SRC}/rtthread_gpio.c
    stubs/rt-thread/rt_pin_stub.c)
target_include_directories(test_rtthread_gpio PRIVATE
    ${GPIO_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
add_test(NAME gpio_rtthread_backend COMMAND test_rtthread_gpio)

# ---- Zephyr 后端适配测试（桩替换 gpio_pin_* / devicetree） ----
add_executable(test_zephyr_gpio
    test_zephyr_gpio.c
    ${GPIO_SRC}/zephyr_gpio.c
    stubs/zephyr/zephyr_gpio_stub.c)
target_include_directories(test_zephyr_gpio PRIVATE
    ${GPIO_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
add_test(NAME gpio_zephyr_backend COMMAND test_zephyr_gpio)

# ---- 自研实现：host 编译冒烟（不运行，避免写真实寄存器地址） ----
find_program(HOST_CC gcc)
if(HOST_CC)
    add_test(NAME gpio_stm32f1_compile_only
        COMMAND ${HOST_CC} -std=gnu11 -Wall -Wextra -fsyntax-only
                -I${GPIO_SRC} ${GPIO_SRC}/stm32f1_gpio.c)
endif()