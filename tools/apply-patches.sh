#!/usr/bin/env bash
# 应用 patches/ 下的补丁到 submodule（rtos/rt-thread）。
# 设计原则：submodule 本身保持官方干净源码（不 fork、不改上游），
# 需要改动官方代码时，把 diff 固化为补丁文件放这里，构建前自动应用。
# 幂等：已应用过的补丁会被检测并跳过。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$ROOT/patches"
RTT="$ROOT/rtos/rt-thread"

# 允许跳过：APPLY_PATCHES=0 时不做（例如需要干净源码调试）
if [ "${APPLY_PATCHES:-1}" = "0" ]; then
    echo "[patch] APPLY_PATCHES=0，跳过补丁应用"
    exit 0
fi

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
if [ "${#patches[@]}" -eq 0 ]; then
    echo "[patch] patches/ 下没有 .patch，跳过"
    exit 0
fi

for p in "${patches[@]}"; do
    # 已应用 -> skip；未应用且可 clean apply -> 应用；冲突 -> 报错
    if git -C "$RTT" apply --reverse --check "$p" 2>/dev/null; then
        echo "[patch] 已应用，跳过: $(basename "$p")"
    elif git -C "$RTT" apply --check "$p"; then
        echo "[patch] 应用: $(basename "$p")"
        git -C "$RTT" apply "$p"
    else
        echo "[patch] 失败(与当前源码冲突): $p" >&2
        exit 1
    fi
done
echo "[patch] 补丁应用完成"