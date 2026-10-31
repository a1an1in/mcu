# =============================================================================
# 业务模块（module）聚合
# 每个模块对应一个独立功能单元，只依赖 OSAL 与 drivers 接口，
# 与 RTOS / MCU 解耦，便于移植与复用。
# 注：新模块只需在此加入对应源文件。
# =============================================================================

add_library(modules OBJECT
    hello/hello.c
    led/led.c
    key/key.c
    storage/eeprom_storage.c
    fs/diskio.c
    fs/fs_util.c
    fs/fatfs/ff.c
    music/music.c
    recorder/recorder.c
    recorder/wav.c
    ai/ai.c
    ai/http_post.c
    voice/voice.c
)

# FatFs 是第三方源码（R0.14b），不参与本工程的 -Wall 洁癖。
set_source_files_properties(fs/fatfs/ff.c PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-but-set-variable")

# msh 命令属于「给 shell 用的适配」，只在有 shell 的 RTOS 后端编译。
# 命令本身只调 module / driver 的接口，不碰寄存器（见各文件头注释）：
#   msh/storage_cmd.c   eeprom <get|set|save|load>
#   msh/i2c_cmd.c       i2c <scan|read|write|smbtest>
#   msh/spi_cmd.c       spi <list|write|read|loop>
#   msh/flash_cmd.c     flash <info|read|erase|write|test>
if(RTOS STREQUAL "rt-thread")
    target_sources(modules PRIVATE
        msh/storage_cmd.c
        msh/i2c_cmd.c
        msh/spi_cmd.c
        msh/flash_cmd.c
        msh/sd_cmd.c
        msh/fs_cmd.c
        msh/mplayer_cmd.c
        msh/record_cmd.c
        msh/chat_cmd.c
        msh/voice_cmd.c
        msh/test_cmd.c
    )
endif()

# 模块需要 OSAL 与 driver 头文件；msh/ 还会用到 drivers/i2c 的 i2c.h / smbus.h、
# drivers/spi 的 spi.h
target_include_directories(modules PUBLIC
    ${CMAKE_SOURCE_DIR}/osal/include
    ${CMAKE_SOURCE_DIR}/drivers
    ${CMAKE_SOURCE_DIR}/drivers/i2c
    ${CMAKE_SOURCE_DIR}/drivers/spi
    ${CMAKE_SOURCE_DIR}/drivers/flash
    ${CMAKE_SOURCE_DIR}/drivers/sd
    ${CMAKE_SOURCE_DIR}/drivers/block
    ${CMAKE_SOURCE_DIR}/modules/storage
    ${CMAKE_SOURCE_DIR}/modules/fs
    ${CMAKE_SOURCE_DIR}/modules/fs/fatfs
    ${CMAKE_SOURCE_DIR}/modules/music
    ${CMAKE_SOURCE_DIR}/modules/recorder
    ${CMAKE_SOURCE_DIR}/modules/ai
    ${CMAKE_SOURCE_DIR}/modules/voice
    ${CMAKE_SOURCE_DIR}/modules/key
    ${CMAKE_SOURCE_DIR}/drivers/audio
)

# 让模块编译期获得 rtt_kernel 的 include（本工程模块虽不直接 include RT-Thread，
# 但保留此依赖以兼容某些需要内核宏的模块）
target_link_libraries(modules PRIVATE osal ${RTOS_KERNEL_TARGET})