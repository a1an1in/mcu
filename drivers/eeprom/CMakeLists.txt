# =============================================================================
# EEPROM 器件驱动组件：AT24C02（I2C，256B / 8 字节每页）
#
# 分层（详见 docs/drivers.md 的四维模型）：
#   at24c02.h          器件接口，芯片 / OS 无关
#   at24c02.c          器件协议（按页切分写 + ACK 轮询）+ at24c02_register()
#   at24c02_config.h   板级事实：7 位地址、轮询次数
#   eeprom.h           字节级访问抽象（struct eeprom_ops + eeprom_dev），芯片无关
#   eeprom.c           通用核心：参数 / 越界校验 + 分发（不含任何器件知识）
#
# 新增一颗器件（24C64 / FRAM / 内部 Flash / 掉电保存区）：
#   在它自己的 .c 里加一个 eeprom_ops 表 + 一个 xxx_register()（照抄
#   at24c02.c 末尾那段），把 .c 加进下面的源文件列表即可 ——
#   eeprom.c / eeprom.h 与所有上层模块都不用改。
#
# 整个组件不含任何 OS 头（msh 命令在 modules/msh/），两个 RTOS 后端都能编。
# 没有 SoC 层：总线已被 i2c 组件抽象成 i2c_transfer，本器件不直接碰寄存器。
# =============================================================================

add_library(eeprom OBJECT
    at24c02.c
    eeprom.c
)

# 组件自身需要找到同目录的接口头 at24c02.h / eeprom.h
# 以及板级事实 at24c02_config.h
target_include_directories(eeprom PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
