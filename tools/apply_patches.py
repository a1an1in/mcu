#!/usr/bin/env python3
# apply_patches.py - 应用 patches/<RTOS>/ 下的补丁到对应的 submodule（rtos/<rtos>）。
#
# 设计原则：submodule 本身保持官方干净源码（不 fork、不改上游），
# 需要改动官方代码时，把 diff 固化为补丁文件放本目录，构建前自动应用。
# 补丁按操作系统分类存放，这里只应用当前 RTOS（环境变量 RTOS，由 devops 传入）对应的那一份。
# 幂等：已应用过的补丁会被检测并跳过。
#
# 纯 Python 3 实现，跨平台（Linux/macOS/Windows）。
import os
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parent.parent

# 当前 RTOS：由 devops 的 -e RTOS=... 注入；缺省按构建默认值 rt-thread
RTOS = os.environ.get("RTOS", "rt-thread")

RTOS_SUBDIR = {
    "rt-thread": "rtos/rt-thread",
    "zephyr": "rtos/zephyr",
    "freertos": "rtos/freertos",
}


def log(msg: str) -> None:
    print(msg, flush=True)


def fail(msg: str) -> "NoReturn":
    print(msg, file=sys.stderr, flush=True)
    sys.exit(1)


def main() -> int:
    rtt_sub = RTOS_SUBDIR.get(RTOS)
    if rtt_sub is None:
        log(f"[patch] 未知 RTOS={RTOS}，跳过补丁应用")
        return 0

    # 允许跳过：APPLY_PATCHES=0 时不做（例如需要干净源码调试）
    if os.environ.get("APPLY_PATCHES", "1") == "0":
        log("[patch] APPLY_PATCHES=0，跳过补丁应用")
        return 0

    rtt = ROOT / rtt_sub
    patch_dir = ROOT / "patches" / RTOS

    patches = sorted(patch_dir.glob("*.patch"))
    if not patches:
        log(f"[patch] patches/{RTOS}/ 下没有 .patch，跳过")
        return 0

    git_file = rtt / ".git"
    if not git_file.exists():
        # git submodule 的 .git 是文件（指向 .git/modules/...），普通克隆是目录；两者都算 git 仓库
        fail(f"[patch] {rtt} 不是 git 仓库，无法应用补丁")

    for p in patches:
        # 已应用 -> skip；未应用且可 clean apply -> 应用；冲突 -> 报错
        rev = subprocess.run(
            ["git", "-C", str(rtt), "apply", "--reverse", "--check", str(p)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if rev.returncode == 0:
            log(f"[patch][{RTOS}] 已应用，跳过: {p.name}")
            continue

        chk = subprocess.run(
            ["git", "-C", str(rtt), "apply", "--check", str(p)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if chk.returncode == 0:
            log(f"[patch][{RTOS}] 应用: {p.name}")
            subprocess.run(["git", "-C", str(rtt), "apply", str(p)], check=True)
        else:
            fail(f"[patch][{RTOS}] 失败(与当前源码冲突): {p}")

    # 隐藏补丁改动：对补丁涉及的文件打 --skip-worktree 标记，使 submodule 内部 git status
    # 也不显示这些改动（改动仍真实保留在工作区供编译，只是 git 不再把它当作待处理修改）。
    patched = []
    for p in patches:
        num = subprocess.run(
            ["git", "apply", "--numstat", str(p)],
            capture_output=True, text=True, cwd=str(rtt),
        )
        for line in (num.stdout or "").splitlines():
            parts = line.split("\t")
            if len(parts) == 3 and parts[2].strip():
                patched.append(parts[2].strip())
    if patched:
        subprocess.run(
            ["git", "-C", str(rtt), "update-index", "--skip-worktree", *patched],
            check=True,
        )
        log(f"[patch][{RTOS}] 已对 {len(set(patched))} 个补丁文件打 --skip-worktree 标记")

    log(f"[patch][{RTOS}] 补丁应用完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())