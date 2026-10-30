# =============================================================================
# SPI 外设驱动组件：对外提供统一接口 spi.h（用法对齐 Linux spi_message / spi_sync）
#
# 分层（详见 docs/drivers.md 的四维模型）：
#   spi.h          接口，芯片 / OS 无关（spi_device / spi_transfer / spi_message）
#   spi_core.c     通用逻辑：控制器注册表、消息校验、CS 时隙编排、总线互斥量
#   spi_config.h   板级事实：本板用哪路 SPI、引脚
#   stm32f1_spi.c  芯片实现：STM32F1 的 SPI1/2/3 寄存器 + CS 引脚 + DWT 延时
#
# 没有 OS 适配层：总线互斥量直接用 OSAL 的 osal_mutex_* 实现，
# 组件里不含任何 RTOS 头，换 OS 只换 osal 后端。
# 也没有 bit-bang 层：SPI 不像 I2C 那样需要两根线模拟时序，硬件外设直接可用。
# =============================================================================

add_library(spi OBJECT
    spi_core.c
    stm32f1_spi.c
)

# 组件自身需要找到同目录的接口头 spi.h / 板级事实 spi_config.h
target_include_directories(spi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 总线互斥量走 osal（同时把 osal/include 传播进来）
target_link_libraries(spi PRIVATE osal)
