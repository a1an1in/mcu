# =============================================================================
# SD 卡驱动组件：SDIO 主机 + SD 协议
#
# 分层（详见 docs/drivers.md 的四维模型）：
#   sd.h           卡接口：sd_init / sd_read_blocks / sd_write_blocks（OS 无关）
#   sd.c           卡协议：上电识别状态机、CID/CSD 解析、块读写
#   sdio.h         主机控制器接口（SoC 无关）
#   sd_config.h    板级事实：CMD/CLK/D0~D3 引脚、时钟
#   stm32f1_sdio.c 芯片实现：STM32F1 SDIO 寄存器 + 引脚 + 轮询搬运
#
# 没有 OS 适配层：本组件不建线程、不加锁（一次块读写由调用方独占总线，
# 目前只有 msh 命令用；将来多线程访问再加 osal_mutex，见 spi/i2c 的做法）。
# =============================================================================

add_library(sd OBJECT
    sd.c
    stm32f1_sdio.c
)

target_include_directories(sd PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 总线互斥量走 osal（同时把 osal/include 传播进来）
target_link_libraries(sd PRIVATE osal)
