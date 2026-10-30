# =============================================================================
# I2C 外设驱动组件：对外提供统一接口 i2c.h（用法对齐 Linux i2c_transfer）
#
# 分层（详见 docs/drivers.md 的四维模型）：
#   i2c.h          接口，芯片 / OS 无关
#   i2c_core.c     通用逻辑：adapter 注册表、总线锁、传输重试、寄存器读写
#   i2c_bit.c      通用 bit-bang 时序状态机（只依赖 i2c_bit_ops 回调）
#   i2c_config.h   板级事实：SCL/SDA 引脚、半周期延时
#   smbus.h/.c     严格 SMBus 协议层（Quick/Send/Receive/Byte/Word/Block/PEC）
#   stm32f1_i2c.c  芯片实现：STM32F1 寄存器原语 + DWT 微秒延时
#
# 没有 OS 适配层：总线互斥量直接用 OSAL 的 osal_mutex_* 实现（见 i2c_core.c），
# 组件里不含任何 RTOS 头，换 OS 只换 osal 后端。
# msh 命令在 modules/msh/i2c_cmd.c。
# =============================================================================

add_library(i2c OBJECT
    i2c_core.c
    i2c_bit.c
    smbus.c
    stm32f1_i2c.c
)

# 组件自身需要找到同目录的接口头 i2c.h / 板级事实 i2c_config.h
target_include_directories(i2c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 总线互斥量走 osal（同时把 osal/include 传播进来）
target_link_libraries(i2c PRIVATE osal)
