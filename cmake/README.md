# 放置与 RTOS 后端相关的构建脚本。
# 目前由顶层 CMakeLists.txt 直接 add_subdirectory 各 rtos/<os> 目录，
# 并由其 CMakeLists.txt 定义 RTOS_KERNEL_TARGET；此目录预留其它复用脚本。