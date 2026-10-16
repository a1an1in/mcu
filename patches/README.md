# patches/ —— 针对 RT-Thread(及其他 submodule) 的补丁目录

## 用途与方法论

本工程采用「**只用官方 submodule，改动走补丁**」的策略，**不 fork、不改上游源码树**：

1. 业务/驱动等一切自研代码都放在工程自身目录（`drivers/gpio/`、`modules/`、`applications/`、`bsp/`）。
2. 只有当你确实需要改 RT-Thread 官方源码（如内核 bug 修复、stm32f103 特定裁剪）时，才制作补丁放本目录。
3. `tools/devops.sh build` 在每次构建前调用 `tools/apply-patches.sh` 自动应用（幂等：已应用的会跳过，冲突会报错）。可用 `APPLY_PATCHES=0` 跳过。

## 制作一个补丁

在 submodule 工作区改完代码后：

```bash
cd rtos/rt-thread
git diff > ../../patches/0001-description.patch
cd ../..
./tools/devops.sh build        # 会自动应用补丁后重新构建
```

## 注意
- submodule 必须固定在某个 tag/commit（当前 v5.2.2），补丁才不会因上游漂移而失效。
- 补丁只修改 submodule 工作区，**不会改动** `.gitmodules` 或 submodule 的 gitlink 记录；
  因此任何人 `git clone --recursive` 后跑一次 `./tools/devops.sh build` 即可复现你的改动。

## 命名约定
- 建议：`NNNN-简短描述.patch`（如 `0001-fix-sched-race.patch`）；多个补丁按依赖顺序编号。