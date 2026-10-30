# =============================================================================
# modules/storage host 单元测试
#
# eeprom_storage 只依赖 <osal.h> + "eeprom.h"，适配层与器件驱动都是真实代码
# （eeprom.c 通用核心 + at24c02.c 的页切分/轮询/ops/register），只把 i2c 层
# 换成 256 字节 RAM，于是「module → 适配层 → 器件」整条链路都能在 host 上
# 真跑一遍（页切分 / 缓存 / 落盘布局 / CRC / 掉电重载）：
#
#   fake_at24c02.c      假 I2C 总线 + 256 字节 RAM，可注入坏数据、统计页写次数
#   fake_osal_storage.c malloc/free + 互斥量，并检查锁是否成对
# =============================================================================

add_executable(test_eeprom_storage
    test_eeprom_storage.c
    fake_at24c02.c
    fake_osal_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/storage/eeprom_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/eeprom/eeprom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/eeprom/at24c02.c)

target_include_directories(test_eeprom_storage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/i2c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/eeprom
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/storage)

add_test(NAME storage_eeprom_storage COMMAND test_eeprom_storage)
