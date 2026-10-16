# 依赖维护与扩展策略（外部模块 + 补丁，不改上游）

**决策：RT-Thread 以官方 submodule 原样使用（当前固定 v5.2.2），不 fork、不直接改其源码树。** 对它的改动按两条途径落地：

1. **能外部化的，一律外部化（首选）**：内核配置在 `bsp/rtconfig.h`、板级初始化在
   `bsp/stm32f103/board.c`、驱动在 `drivers/gpio/`（接口 `gpio.h`、实现 `stm32f1_gpio.c`）、
   内核可选源文件通过 `rtos/rt-thread/CMakeLists.txt` 选编引用——这些都不碰官方源码一行。
2. **确实要改官方源码时用补丁**：把 diff 固化为 `patches/*.patch`，`tools/devops.sh build`
   每次构建前由 `tools/apply-patches.sh` 自动应用（幂等：已应用跳过、冲突即报错）。
   详见 `patches/README.md`。

> 因为 submodule 锁在 v5.2.2 快照，补丁不会因上游漂移失效；协作者
> `git clone --recursive` 后跑一次 `./tools/devops.sh build` 即可复现全部改动。