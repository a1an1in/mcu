# =============================================================================
# 块设备组件：与介质无关的 512B 块设备接口 + 注册表
#
#   blkdev.h   设备模型（struct blkdev / struct blkdev_ops）+ 访问接口
#   blkdev.c   静态注册表（不分配内存、不加锁、不碰寄存器）
#
# 适配层放在介质自己的目录里（drivers/sd/sd_blkdev.c），这样本组件不用知道
# 任何具体硬件。文件系统侧见 modules/fs/diskio.c。
# =============================================================================

add_library(block OBJECT
    blkdev.c
)

target_include_directories(block PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
