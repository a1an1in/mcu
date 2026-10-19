#!/usr/bin/env python3
# build_zephyr.py - 在容器/宿主机内构建 Zephyr 固件（--rtos zephyr 专用）。
#
# 精简模式：NOT `west update`（那会全量拉取 Zephyr manifest 里 80+ 个与 STM32
# 无关的厂商模块）。这里只按需 clone 真正需要的模块（默认 hal_stm32 + cmsis），
# 版本 revision 从 rtos/zephyr/west.yml 动态解析以保证对齐，再用 Zephyr 的
# ZEPHYR_MODULES 机制挂载进构建。
#
# 说明：Zephyr 是构建系统宿主，不走工程顶层 CMakeLists.txt。
# 纯 Python 3 实现，跨平台。
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ZEPHYR_BASE = Path(os.environ.get("ZEPHYR_BASE", str(ROOT / "rtos/zephyr")))
ZEPHYR_BOARD = os.environ.get("ZEPHYR_BOARD", "stm32f103_mini")
BUILD_DIR = Path(os.environ.get("BUILD_DIR", str(ROOT / "build/stm32f103/zephyr")))
# Zephyr 用户缓存（工具链能力探测 ToolchainCapabilityDatabase 等）重定向到**当次构建目录**，
# 避免落到 Zephyr 兜底目录 rtos/zephyr/.cache（那会污染 Zephyr 源码树）；
# 与 rt-thread 固件（build/<board>/rt-thread/）互不干扰：zephyr 全在 build/<board>/zephyr/ 内。
USER_CACHE = BUILD_DIR / ".cache"
GEN = os.environ.get("GEN", "Ninja")
APP = ROOT / "applications/zephyr"  # Zephyr 应用工程（置于仓库 applications/ 下，不落入 rtos/ 源码树）
VENV = BUILD_DIR / ".venv-zephyr"   # Python 工具链随构建目录收纳（集中非源码产物）；删 build 后需联网重建
# 只存放“按需拉取”的模块（原 west 全量目录已废弃）；放**构建目录**内，彻底脱离 rtos/ 源码树
MODDIR = BUILD_DIR / "external"

# 需要哪些第三方模块：(<name>, <相对 MODDIR 的子目录>, <git 仓库>, <west.yml 项目名>)
# stm32 必需：hal_stm32；cmsis 提供 ARM CMSIS 头（stm32 常用依赖）
# cmsis 需用 ARM CMSIS-6 源（Zephyr 4.4 的 SCB_Type 用 SHPR，仅 CMSIS-6 提供），west 项目名为 cmsis_6。
ZMODULES = [
    ("hal_stm32", "hal/stm32", "https://github.com/zephyrproject-rtos/hal_stm32.git", "hal_stm32"),
    ("cmsis", "hal/cmsis", "https://github.com/ARM-software/CMSIS_6.git", "cmsis_6"),
]

IS_WINDOWS = os.name == "nt"
VENV_PY = VENV / ("Scripts/python.exe" if IS_WINDOWS else "bin/python")
VENV_PIP = VENV / ("Scripts/pip.exe" if IS_WINDOWS else "bin/pip")


def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


# 从 rtos/zephyr/west.yml 读取模块 pinned revision（保证与当前 Zephyr 对齐）
def module_rev(module_name: str) -> str:
    code = (
        "import sys,yaml\n"
        "d=yaml.safe_load(open(sys.argv[1]))\n"
        "mod=sys.argv[2]\n"
        "for p in d['manifest']['projects']:\n"
        "    if p.get('name')==mod:\n"
        "        print(p['revision']); break\n"
    )
    out = subprocess.run(
        [str(VENV_PY), "-c", code, str(ZEPHYR_BASE / "west.yml"), module_name],
        capture_output=True,
        text=True,
        check=True,
    )
    return out.stdout.strip()


def _has_real_files(target: Path) -> bool:
    """工作树里是否还有除 .git 之外的实体文件（排除“仅剩 .git 的半成品”）。"""
    if not target.is_dir():
        return False
    return any(p.name != ".git" for p in target.iterdir())


def _has_valid_head(target: Path) -> bool:
    return subprocess.run(
        ["git", "-C", str(target), "rev-parse", "-q", "--verify", "HEAD"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


# 按需拉取单个模块（已存在且完整则跳过）
def ensure_module(spec) -> None:
    name, mod_subdir, url, west_name = spec
    target = MODDIR / mod_subdir

    # 完整性校验：需 .git、有效 HEAD、且工作树存在（排除“仅剩 .git 的半成品”）。
    if (target / ".git").is_dir() and _has_valid_head(target) and _has_real_files(target):
        log(f"[zephyr] 模块已存在：{name} ({target})")
        return

    if target.exists():
        shutil.rmtree(target)  # 清掉半成品/残留 .git，整体重建
    target.parent.mkdir(parents=True, exist_ok=True)
    rev = module_rev(west_name)
    log(f"[zephyr] 按需拉取模块 {name}@{rev[:12]} -> {target}")
    run(["git", "init", "-q", str(target)])
    run(["git", "-C", str(target), "remote", "add", "origin", url])
    run(["git", "-C", str(target), "fetch", "-q", "--depth", "1", "origin", rev])
    run(["git", "-C", str(target), "checkout", "-q", "FETCH_HEAD"])


def main() -> int:
    log(f"[zephyr] ZEPHYR_BASE={ZEPHYR_BASE}  BOARD={ZEPHYR_BOARD}")

    # 1) Python 工具链（Zephyr 构建脚本需要）。幂等：venv 缺失则创建，
    #    关键模块缺失则按 Zephyr 官方 requirements-base.txt 补装。
    if not VENV_PY.is_file():
        log("[zephyr] 创建 python venv ...")
        run([sys.executable, "-m", "venv", str(VENV)])
        run([str(VENV_PIP), "install", "--quiet", "--upgrade", "pip"])

    needed = ["jsonschema", "pykwalify", "elftools", "yaml", "packaging", "anytree", "intelhex"]
    missing = []
    for m in needed:
        r = subprocess.run(
            [str(VENV_PY), "-c", f"import {m}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if r.returncode != 0:
            missing.append(m)
    if missing:
        log(f"[zephyr] 补装 Zephyr 构建 python 依赖（缺失：{' '.join(missing)}）...")
        req_base = ZEPHYR_BASE / "scripts" / "requirements-base.txt"
        if req_base.is_file():
            run([str(VENV_PIP), "install", "--quiet", "-r", str(req_base)])
        else:
            run([str(VENV_PIP), "install", "--quiet"] + needed)

    # 2) 组装 ZEPHYR_MODULES 列表。用 `;` 分隔（CMake 原生列表分隔符）；
    #    `:` 会被当作普通字符串，导致 zephyr_module.py 把 "dir1:dir2" 当单个路径。
    zmod_list = []
    for spec in ZMODULES:
        ensure_module(spec)
        zmod_list.append(str(MODDIR / spec[1]))
    zmod_str = ";".join(zmod_list)
    log(f"[zephyr] ZEPHYR_MODULES={zmod_str.replace(';', ':')}")

    # 让 Zephyr/CMake 使用我们的 venv python（已装 yaml/jsonschema/elftools 等），
    # 而非容器系统 python3（缺这些依赖）
    os.environ["PATH"] = str(VENV / ("Scripts" if IS_WINDOWS else "bin")) + os.pathsep + os.environ["PATH"]

    # 3) cross-compile 工具链变体：Zephyr 4.4 已移除 generic，改为 cross-compile。
    os.environ["ZEPHYR_BASE"] = str(ZEPHYR_BASE)
    os.environ["ZEPHYR_TOOLCHAIN_VARIANT"] = "cross-compile"
    arm_toolchain_prefix = os.environ.get("ARM_TOOLCHAIN_PREFIX", "/usr/bin/arm-none-eabi-")
    os.environ["CROSS_COMPILE"] = arm_toolchain_prefix
    os.environ["TOOLCHAIN_HOME"] = os.environ.get("TOOLCHAIN_HOME", "/usr")
    os.environ["ZEPHYR_MODULES"] = zmod_str
    # 框架里 `${ZEPHYR_BASE}/modules` 下若曾存在 west 全量目录，则忽略，避免混入
    os.environ.pop("ZEPHYR_WEST_PROJECT", None)

    # 4) Zephyr CMake 配置 + 编译
    log(f"[zephyr] 配置并编译 {APP} -> {BUILD_DIR}")
    cmake_cmd = [
        "cmake", "-S", str(APP), "-B", str(BUILD_DIR), "-G", GEN,
        f"-DBOARD={ZEPHYR_BOARD}",
        f"-DZEPHYR_BASE={ZEPHYR_BASE}",
        f"-DZEPHYR_MODULES={zmod_str}",
        f"-DUSER_CACHE_DIR={USER_CACHE}",
        f"-DPYTHON_EXECUTABLE={VENV_PY}",
        "-DZEPHYR_TOOLCHAIN_VARIANT=cross-compile",
        f"-DCROSS_COMPILE={arm_toolchain_prefix}",
        f"-DTOOLCHAIN_HOME={os.environ['TOOLCHAIN_HOME']}",
    ]
    run(cmake_cmd)
    run(["cmake", "--build", str(BUILD_DIR)])

    log(f"[zephyr] 完成。产物：{BUILD_DIR}/zephyr/ 下的 zephyr.bin|.hex|.elf")
    log(f"[zephyr] 烧录建议使用 Zephyr 的 flash 目标：cmake --build {BUILD_DIR} --target flash")
    return 0


if __name__ == "__main__":
    sys.exit(main())