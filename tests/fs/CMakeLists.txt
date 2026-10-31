# =============================================================================
# 块设备 + FatFs 的 host 单元测试
#
# 被测代码直接以源文件加入（和 tests/sd、tests/flash 的做法一致）：
#   drivers/block/blkdev.c    块设备注册表
#   modules/fs/diskio.c       FatFs 的 disk 层
#   modules/fs/fs_util.c      递归删除
#   modules/fs/fatfs/ff.c     FatFs 本体（第三方，屏蔽 warning）
#
# 介质是 test_fatfs.c 里的 256KB RAM 盘，不需要任何硬件。
# =============================================================================

add_executable(test_fatfs
    test_fatfs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/block/blkdev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/fs/diskio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/fs/fs_util.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/fs/fatfs/ff.c
)

target_include_directories(test_fatfs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/block
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/fs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/fs/fatfs
)

# FatFs 是第三方源码，不按本工程的 -Wall 标准要求它
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/fs/fatfs/ff.c
    PROPERTIES COMPILE_OPTIONS "-w")

add_test(NAME fatfs COMMAND test_fatfs)
