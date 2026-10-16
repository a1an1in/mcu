# 当前候选待办

- 内核配置保持"最小可运行集"（见 `bsp/rtconfig.h` 注释），后续可按需从
  `rtos/rt-thread/bsp/stm32/stm32f103-blue-pill/rtconfig.h` 合并 FINSH/MSH/Device/SERIAL 等组件。
- `middleware/`、`drivers/` 目录内容按项目需要逐步填充。
- （可选清理）内核 `rt_sched_*` 隐式声明告警的根因核查。