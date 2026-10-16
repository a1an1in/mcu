# 当前候选待办

- 内核配置保持"最小可运行集"（见 `bsp/rtconfig.h` 注释）。
- 中间件 / 协议栈层待填充（规划归入 `modules/`，不再单列目录）。
- FreeRTOS 后端接入（顶层 CMake 的 `freertos` 分支仍为占位；接入方式与 RT-Thread
  同属"源码 add_subdirectory"系，参考 `rtos/rt-thread/CMakeLists.txt` 导出
  `RTOS_KERNEL_TARGET`）。
- Zephyr 编译链路的真机验证：`devops.sh build --rtos zephyr` 需环境具备
  拉取 `hal_stm32`/`cmsis` 模块（`ZEPHYR_MODULES` 按需模式）的网络；在具备该
  环境的主机/CI 上完成首次固件产出与烧录验证。

### 已完成
- Zephyr 接入（代码层）：`osal/port/zephyr/` 7 个 OSAL 适配文件、独立应用工程
  `applications/zephyr/`（`find_package(Zephyr)` + `prj.conf`）、`devops.sh --rtos zephyr`
  构建路由、Dockerfile 补 python/west 依赖、顶层 CMake zephyr 分支指引。