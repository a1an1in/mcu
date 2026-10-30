# =============================================================================
# 板级支持（bsp）
#  bsp/<BOARD>/        —— 这块板的全部东西：rtconfig.h（RT-Thread 内核配置）、
#                          lwipopts.h（lwIP 配置）、board.c/board.h、启动文件、
#                          链接脚本。按顶层 BOARD 缓存变量选择。
#  本文件只做分派：新增板子时加一个分支，不在这里放任何配置。
# =============================================================================

# —— 多板分派：新增板子时在此加分支（BOARD 见顶层 CMakeLists.txt）——
if(BOARD STREQUAL "stm32f103")
    set(BSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/stm32f103)

    add_library(board OBJECT ${BSP_DIR}/board.c)
    # board 需要内核头（rtthread.h、rthw.h）、rtconfig.h/lwipopts.h 与 board.h（均在本目录）；
    # 另外 board 是「装配根」，要把 storage module 与后端接起来，故还要看到 modules/storage
    target_include_directories(board PUBLIC
        ${BSP_DIR}
        ${CMAKE_SOURCE_DIR}/modules/storage
    )
    target_link_libraries(board PRIVATE ${RTOS_KERNEL_TARGET})

    # 芯片启动文件 + 链接脚本归入 bsp/<BOARD>/（与官方“板卡模板自带”一致），
    # 交由顶层可执行文件使用；用 PARENT_SCOPE 传回到链接层可见。
    set(MCU_STARTUP_FILE
        ${BSP_DIR}/startup_stm32f10x_md.S
        PARENT_SCOPE)
    set(BSP_LINK_SCRIPT
        ${BSP_DIR}/stm32f103.ld
        PARENT_SCOPE)
else()
    message(FATAL_ERROR
        "尚未提供 bsp/${BOARD} 支持；请在 bsp/CMakeLists.txt 增加该板分支")
endif()