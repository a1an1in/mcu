# =============================================================================
# drivers/i2c host 单元测试
#
# 被测代码是真实的 i2c_bit.c（bit-bang 时序）与 i2c_core.c（transfer / 重试 /
# 寄存器读写），只把引脚原语换成「假总线 + 假从机」，在宿主 gcc 上把线上电平
# 解码成字节序列后断言：
#
#   - I2C_M_NOSTART 的 msg 不得重发地址字节（曾导致整段数据错位一格、末尾丢字节）
#   - 读操作的重复 START，以及末字节必须回 NACK
#   - 器件 NACK 之后总线要回到空闲（STOP 收尾）
# =============================================================================

add_executable(test_i2c_bit
    test_i2c_bit.c
    fake_osal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/i2c/i2c_bit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/i2c/i2c_core.c)

target_include_directories(test_i2c_bit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/i2c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include)

add_test(NAME i2c_bit_host COMMAND test_i2c_bit)
