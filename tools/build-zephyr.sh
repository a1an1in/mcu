#!/usr/bin/env bash
# build-zephyr.sh - 在容器/宿主机内构建 Zephyr 固件（--rtos zephyr 专用）
#
# 精简模式：NOT `west update`（那会全量拉取 Zephyr manifest 里 80+ 个与 STM32
# 无关的厂商模块）。这里只按需 clone 真正需要的模块（默认 hal_stm32 + cmsis），
# 版本 revision 从 rtos/zephyr/west.yml 动态解析以保证对齐，再用 Zephyr 的
# ZEPHYR_MODULES 机制挂载进构建。
#
# 说明：Zephyr 是构建系统宿主，不走工程顶层 CMakeLists.txt。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ZEPHYR_BASE="${ZEPHYR_BASE:-$ROOT/rtos/zephyr}"
ZEPHYR_BOARD="${ZEPHYR_BOARD:-stm32f103_mini}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/stm32f103/zephyr}"
# Zephyr 用户缓存（工具链能力探测 ToolchainCapabilityDatabase 等）重定向到**当次构建目录**，
# 避免落到 Zephyr 兜底目录 rtos/zephyr/.cache（那会污染 Zephyr 源码树）；
# 与 rt-thread 固件（build/<board>/rt-thread/）互不干扰：zephyr 全在 build/<board>/zephyr/ 内。
USER_CACHE="${BUILD_DIR}/.cache"
GEN="${GEN:-Ninja}"
APP="$ROOT/applications/zephyr"   # Zephyr 应用工程（置于仓库 applications/ 下，不落入 rtos/ 源码树）
VENV="${BUILD_DIR}/.venv-zephyr"   # Python 工具链随构建目录收纳（集中非源码产物）；删 build 后需联网重建
# 只存放"按需拉取"的模块（原 west 全量目录已废弃）；放**构建目录**内，彻底脱离 rtos/ 源码树
MODDIR="${BUILD_DIR}/external"

# 需要哪些第三方模块（<name> <相对 MODDIR 的子目录> <git 仓库>）
# stm32 必需：hal_stm32；cmsis 提供 ARM CMSIS 头（stm32 常用依赖）
ZMODULES=(
  "hal_stm32|hal/stm32|https://github.com/zephyrproject-rtos/hal_stm32.git"
  # cmsis 需用 ARM CMSIS-6 源（Zephyr 4.4 的 SCB_Type 用 SHPR，仅 CMSIS-6 提供；
  # zephyrproject-rtos/cmsis 仍是 5.x，不匹配）。该仓库自带 zephyr/module.yml:
  # name=cmsis_6, cmake-ext → 触发 Zephyr modules/cmsis_6 glue 加载 Cortex-M 头。
  # cmsis_6：来源是 ARM-software/CMSIS_6 仓（Zephyr 4.4 的 SCB_Type 用 SHPR，仅 CMSIS-6 提供；
  #   west.yml 的 `cmsis` 是 5.x 的 zephyrproject-rtos/cmsis，不匹配）。arm 仓对应 west 项目名是
  #   `cmsis_6`（repo-path=CMSIS_6），故第四段 west_name=cmsis_6，让 module_rev 从正确项目取 pinned rev。
  "cmsis|hal/cmsis|https://github.com/ARM-software/CMSIS_6.git|cmsis_6"
)

echo "[zephyr] ZEPHYR_BASE=$ZEPHYR_BASE  BOARD=$ZEPHYR_BOARD"

# 1) Python 工具链（Zephyr 构建脚本需要）。幂等：venv 缺失则创建，
#    关键模块缺失则按 Zephyr 官方 requirements-base.txt 补装（含 jsonschema/
#    pykwalify/pyelftools/pyyaml 等，不依赖是否新建过 venv）。
PY_BIN="$VENV/bin/python"
if [ ! -x "$PY_BIN" ]; then
  echo "[zephyr] 创建 python venv ..."
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
fi

NEEDED='jsonschema pykwalify elftools yaml packaging anytree intelhex'
MISSING=""
for m in $NEEDED; do "$PY_BIN" -c "import $m" 2>/dev/null || MISSING="$MISSING $m"; done
if [ -n "$MISSING" ]; then
  echo "[zephyr] 补装 Zephyr 构建 python 依赖（缺失：$MISSING）..."
  REQ_BASE="$ZEPHYR_BASE/scripts/requirements-base.txt"
  if [ -f "$REQ_BASE" ]; then
    "$VENV/bin/pip" install --quiet -r "$REQ_BASE"
  else
    "$VENV/bin/pip" install --quiet $NEEDED
  fi
fi

# 从 rtos/zephyr/west.yml 读取模块 pinned revision（保证与当前 Zephyr 对齐）
module_rev() {
  "$PY_BIN" -c 'import sys,yaml; d=yaml.safe_load(open(sys.argv[1])); mod=sys.argv[2]
for p in d["manifest"]["projects"]:
    if p.get("name")==mod: print(p["revision"]); break' "$ZEPHYR_BASE/west.yml" "$1"
}

# 按需拉取单个模块（已存在则跳过）
ensure_module() { # $1=spec "name|dir|url"
  local spec="$1"
  local name="${spec%%|*}"
  local rest="${spec#*|}"
  local dir="${rest%%|*}"
  local rest2="${rest#*|}"
  local url="${rest2%%|*}"
  local west_name
  # 仅当存在第四段（含 '|'）时取 west 项目名；否则用 name（hal_stm32 等默认同名）。
  # 注意：若用 ${rest2#*|} 且 rest2 无 '|'，会返回整段 url（非空），导致误当作项目名。
  if [[ "$rest2" == *"|"* ]]; then
    west_name="${rest2#*|}"
  else
    west_name="$name"
  fi
  local target="$MODDIR/$dir" rev
  # 完整性校验：需 .git、有效 HEAD、且工作树存在（排除"仅剩 .git 的半成品"）。
  # 例如上次 fetch/checkout 被中断只留下空 .git 时，若仅凭 `-d .git` 判已存在，
  # 会以"空模块"混入构建，导致设备树等依赖模块内容的文件缺失，如 st/f1/*tx-pinctrl.dtsi。
  if [ -d "$target/.git" ] \
     && git -C "$target" rev-parse -q --verify HEAD >/dev/null 2>&1 \
     && [ -n "$(find "$target" -mindepth 1 -maxdepth 1 ! -name '.git' 2>/dev/null | head -1)" ]; then
    echo "[zephyr] 模块已存在：$name ($target)"
  else
    rm -rf "$target"   # 清掉半成品/残留 .git，整体重建
    mkdir -p "$(dirname "$target")"
    rev="$(module_rev "$west_name")"
    echo "[zephyr] 按需拉取模块 $name@${rev:0:12} -> $target"
    git init -q "$target"
    git -C "$target" remote add origin "$url"
    git -C "$target" fetch -q --depth 1 origin "$rev"
    git -C "$target" checkout -q FETCH_HEAD
  fi
}

# 2) 组装 ZEPHYR_MODULES 列表。用 `;` 分隔（CMake 原生列表分隔符）；
#    `:` 会被当作普通字符串，导致 zephyr_module.py 把 "dir1:dir2" 当单个路径。
#    只取 dir 段（不能带 url）。
ZMOD_LIST=""
for spec in "${ZMODULES[@]}"; do
  ensure_module "$spec"
  _rest="${spec#*|}"        # 去掉 "name|" → "dir|url"
  _dir="${_rest%%|*}"       # 截断 "|url" → "dir"
  ZMOD_LIST="${ZMOD_LIST:+$ZMOD_LIST;}$MODDIR/$_dir"
done
export ZMOD_LIST
echo "[zephyr] ZEPHYR_MODULES=${ZMOD_LIST//;/:}"

# 让 Zephyr/CMake 使用我们的 venv python（已装 yaml/jsonschema/elftools 等），
# 而非容器系统 python3（缺这些依赖）
export PATH="$VENV/bin:$PATH"

# 3) cross-compile 工具链变体：Zephyr 4.4 已移除 generic，改为 cross-compile。
#    generic.cmake 用 find_program(... NO_DEFAULT_PATH)：name 含 '/' 时按文件路径
#    直接命中，因此 CROSS_COMPILE 须给**绝对路径前缀**（镜像里工具链在 /usr/bin）。
export ZEPHYR_BASE
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
ARM_TOOLCHAIN_PREFIX="${ARM_TOOLCHAIN_PREFIX:-/usr/bin/arm-none-eabi-}"
export CROSS_COMPILE="$ARM_TOOLCHAIN_PREFIX"
export TOOLCHAIN_HOME="${TOOLCHAIN_HOME:-/usr}"
export ZEPHYR_MODULES="$ZMOD_LIST"
# 框架里 `${ZEPHYR_BASE}/modules` 下若曾存在 west 全量目录，则忽略，避免混入
unset ZEPHYR_WEST_PROJECT 2>/dev/null || true

# 4) Zephyr CMake 配置 + 编译
echo "[zephyr] 配置并编译 $APP -> $BUILD_DIR"
cmake -S "$APP" -B "$BUILD_DIR" -G "$GEN" \
      -DBOARD="$ZEPHYR_BOARD" \
      -DZEPHYR_BASE="$ZEPHYR_BASE" \
      -DZEPHYR_MODULES="$ZMOD_LIST" \
      -DUSER_CACHE_DIR="$USER_CACHE" \
      -DPYTHON_EXECUTABLE="$VENV/bin/python" \
      -DZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
      -DCROSS_COMPILE="$ARM_TOOLCHAIN_PREFIX" \
      -DTOOLCHAIN_HOME="${TOOLCHAIN_HOME:-/usr}"
cmake --build "$BUILD_DIR"

echo "[zephyr] 完成。产物：$BUILD_DIR/zephyr/ 下的 zephyr.bin|.hex|.elf"
echo "[zephyr] 烧录建议使用 Zephyr 的 flash 目标：cmake --build $BUILD_DIR --target flash"