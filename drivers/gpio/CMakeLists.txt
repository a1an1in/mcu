# =============================================================================
# GPIO 外设驱动组件：对外提供统一接口 gpio.h（上层 module 只依赖此接口）。
# 实现可选，通过 -DGPIO_BACKEND=<backend> 在构建期切换，切换时 module 零改动：
#   stm32f1   - 自研寄存器实现 stm32f1_gpio.c（默认，不依赖操作系统）
#   rtthread  - RT-Thread 自带 PIN 驱动适配 rtthread_gpio.c（需 RT-Thread PIN 设备栈）
#   zephyr    - Zephyr 自带 GPIO 驱动适配 zephyr_gpio.c（需 Zephyr 构建体系与 dts）
# =============================================================================

set(GPIO_BACKEND "stm32f1" CACHE STRING
    "GPIO 实现后端: stm32f1(自研寄存器) / rtthread(RT-Thread PIN) / zephyr(Zephyr GPIO)")

set(GPIO_SRCS)
if(GPIO_BACKEND STREQUAL "rtthread")
    list(APPEND GPIO_SRCS rtthread_gpio.c)
elseif(GPIO_BACKEND STREQUAL "zephyr")
    list(APPEND GPIO_SRCS zephyr_gpio.c)
    message(WARNING "GPIO_BACKEND=zephyr 需要 Zephyr 构建体系与板级 dts（当前主构建不编 Zephyr），正式使用请改走 Zephyr 工程")
else()
    list(APPEND GPIO_SRCS stm32f1_gpio.c)
    set(GPIO_BACKEND "stm32f1")
endif()

message(STATUS "[stm32f103] GPIO 实现后端 = ${GPIO_BACKEND}")

add_library(gpio OBJECT ${GPIO_SRCS})

# 组件自身需要找到同目录的接口头 gpio.h
target_include_directories(gpio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# rtthread 后端需要继承 RT-Thread 内核的全部 include 路径（含 libcpu/arch 头）
if(GPIO_BACKEND STREQUAL "rtthread")
    target_link_libraries(gpio PRIVATE ${RTOS_KERNEL_TARGET})
endif()