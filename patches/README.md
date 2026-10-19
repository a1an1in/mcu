# patches/<RTOS>/ —— 按操作系统分类的 submodule 补丁

## 用途与方法论

本工程采用「**只用官方 submodule，改动走补丁**」的策略，**不 fork、不改上游源码树**：

1. 业务/驱动等一切自研代码都放在工程自身目录（`drivers/gpio/`、`modules/`、`applications/`、`bsp/`）。
2. 只有当你确实需要改 RTOS 官方源码（如内核 bug 修复、stm32f103 特定裁剪）时，才制作补丁放本目录。
3. `python3 tools/devops.py build` 在每次构建前调用 `python3 tools/apply_patches.py` 自动应用（幂等：已应用的会跳过，冲突会报错）。可用 `APPLY_PATCHES=0` 跳过。

## 按操作系统分类

每个 RTOS 的补丁独立存放，构建时**只应用当前 `--rtos` 对应的那一个**（由 `apply_patches.py` 依据 `RTOS` 环境变量选择）：

| 目录               | 应用的 submodule    | 触发条件                     |
|-------------------|---------------------|------------------------------|
| `patches/rt-thread/` | `rtos/rt-thread`     | `--rtos rt-thread`（默认）   |
| `patches/zephyr/`    | `rtos/zephyr`        | `--rtos zephyr`              |
| `patches/freertos/`  | `rtos/freertos`      | `--rtos freertos`            |

- 空目录 / 无 `.patch` 会直接跳过，互不影响。
- 产物也按 RTOS 拆分为 `build/<BOARD>/<rtos>/`，补丁与产物一一对应。

## 制作一个补丁

在对应 submodule 工作区改完代码后：

```bash
# RT-Thread 示例
cd rtos/rt-thread
git diff > ../../patches/rt-thread/0001-description.patch
cd ../..
python3 tools/devops.py build --rtos rt-thread   # 自动应用 rt-thread 分类补丁后重新构建
```

## 注意
- submodule 必须固定在某个 tag/commit（当前 rt-thread `v2.0.0_beta-...` / zephyr `main`），补丁才不会因上游漂移而失效。
- 补丁只修改对应 submodule 工作区，**不会改动** `.gitmodules` 或 submodule 的 gitlink 记录；
  因此任何人 `git clone --recursive` 后跑一次 `python3 tools/devops.py build --rtos <rtos>` 即可复现你的改动。

## 命名约定
- 建议：`NNNN-简短描述.patch`（如 `0001-fix-sched-race.patch`）；多个补丁按依赖顺序编号。