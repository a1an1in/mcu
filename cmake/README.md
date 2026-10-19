# 放置与 RTOS 后端相关的构建脚本。
# 目前由顶层 CMakeLists.txt 直接 add_subdirectory 各 rtos/<os> 目录，
# 并由其 CMakeLists.txt 定义 RTOS_KERNEL_TARGET；此目录预留其它复用脚本。

# 关于 RT-Thread 的 CMake 集成文件：
#   RT-Thread 官方仓库不含根 CMakeLists.txt，且其 .gitignore 会忽略 CMakeLists.txt，
#   因此无法把集成文件托管在 submodule 内。本工程将它作为受管文件放在
#   cmake/rt-thread/CMakeLists.txt，由 tools/devops.py 在构建/进入 shell 前自动
#   复制到 rtos/rt-thread/CMakeLists.txt（Linux/Windows 行为一致）。