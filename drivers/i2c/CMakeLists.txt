# =============================================================================
# I2C 外设驱动组件：对外提供统一接口 i2c.h（用法对齐 Linux i2c_transfer）。
#
# 分层（详见 docs/drivers.md 的四维模型）：
#   i2c.h          接口，芯片 / OS 无关
#   i2c_core.c     通用逻辑：adapter 注册表、传输重试、寄存器读写
#   i2c_bit.c      通用 bit-bang 时序状态机（只依赖 i2c_bit_ops 回调）
#   i2c_config.h   板级事实：SCL/SDA 引脚、半周期延时
#   smbus.h/.c     严格 SMBus 协议层（Quick/Send/Receive/Byte/Word/Block/PEC）
#   stm32f1_i2c.c  芯片实现：STM32F1 寄存器原语 + DWT 微秒延时
#   rtthread_i2c.c OS 适配：总线互斥量 + i2c_scan/read/write 命令
#
# 除 rtthread_i2c.c 外都不含任何 OS 头。
# =============================================================================

set(I2C_SRCS
    i2c_core.c
    i2c_bit.c
    smbus.c
    stm32f1_i2c.c
)

if(RTOS STREQUAL "rt-thread")
    list(APPEND I2C_SRCS rtthread_i2c.c)
endif()

add_library(i2c OBJECT ${I2C_SRCS})

# 组件自身需要找到同目录的接口头 i2c.h / 板级事实 i2c_config.h
target_include_directories(i2c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 只有 OS 适配层需要内核头与内核符号（rt_mutex_* / rt_kprintf）
if(RTOS STREQUAL "rt-thread")
    target_link_libraries(i2c PRIVATE ${RTOS_KERNEL_TARGET})
endif()
