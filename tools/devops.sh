#!/usr/bin/env bash
# devops.sh - 统一的开发/运维入口（构建 / 烧录 / 测试）
#
# 构建与测试一律在**同一个 Docker 镜像**内完成，宿主机无需安装任何工具链。
# 产物按目标板区分：默认输出到 build/<BOARD>/，多板互不干扰。
#
# 用法:
#   ./tools/devops.sh build [选项]        Docker 交叉编译固件（唯一构建方式）
#       --board <板名>        目标板（默认 stm32f103，可简写 f103）
#       --rtos <内核>         RTOS 后端：rt-thread/zephyr/freertos（默认 rt-thread）
#       --gpio-backend <名>   GPIO 实现后端（默认 stm32f1）
#       --host|--test|-t      额外编译 host 单测程序（宿主 gcc；仅编译，不运行）
#   ./tools/devops.sh test                只**运行**已编译好的 host 单测（编译请先 build --host）
#   ./tools/devops.sh flash               ST-Link 烧录（宿主机需 openocd）
#   ./tools/devops.sh help                显示本帮助
#
# 各值也可用同名环境变量作为默认值（选项优先）：BOARD / RTOS / GPIO_BACKEND。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BOARD="${BOARD:-stm32f103}"                    # 目标板，默认 stm32f103（可简写 f103）
RTOS="${RTOS:-rt-thread}"
GEN="${GEN:-Ninja}"
TOOLCHAIN="${TOOLCHAIN:-$ROOT/cmake/toolchain-arm-none-eabi.cmake}"
GPIO_BACKEND="${GPIO_BACKEND:-stm32f1}"
IMG="${IMG:-stm32f103-toolchain:latest}"
DOCKERFILE="${DOCKERFILE:-$ROOT/docker/Dockerfile}"

# BUILD_DIR/TEST_BUILD 默认派生自 BOARD；若显式给过环境变量则不再随 --board 移动。
BUILD_DIR="${BUILD_DIR:-}"
if [ -n "$BUILD_DIR" ]; then
  BUILD_DIR_SET=1
else
  BUILD_DIR="$ROOT/build/$BOARD"
  BUILD_DIR_SET=0
fi

usage() {
  cat <<'USAGE'
用法:
  ./tools/devops.sh build [选项]         Docker 交叉编译固件（唯一构建方式）
      选项:
        --board <板名>        目标板（默认 stm32f103，可简写 f103）
        --rtos <内核>         RTOS 后端：rt-thread / zephyr / freertos（默认 rt-thread）
        --gpio-backend <名>   GPIO 实现后端（默认 stm32f1）
        --host|--test|-t      额外编译 host 单测程序（宿主 gcc；仅编译，不运行）
  ./tools/devops.sh test                只**运行**已编译好的 host 单测（编译请先 build --host）
  ./tools/devops.sh flash               ST-Link 烧录（宿主机需 openocd）
  ./tools/devops.sh help                显示本帮助

示例:
  ./tools/devops.sh build                          # 默认板 stm32f103 + rt-thread，只编译固件
  ./tools/devops.sh build --host                   # 编译固件 + 编译 host 单测程序（不运行）
  ./tools/devops.sh test                           # 只运行已编译的 host 单测
USAGE
  exit "${1:-0}"
}

# f103 简写归一化到 stm32f103
normalize_board() {
  case "$1" in
    f103|stm32f103) echo "stm32f103" ;;
    *) echo "$1" ;;
  esac
}

# 构建/测试共用的镜像准备（幂等：每次构建，未变则走层缓存）
ensure_image() {
  echo "[devops] 准备镜像 $IMG（首次较慢，之后命中缓存）"
  docker build -t "$IMG" -f "$DOCKERFILE" "$(dirname "$DOCKERFILE")"
}

# 按新的 BOARD 重算派生目录（仅当未显式设置 BUILD_DIR 时）
apply_board() {
  BOARD="$(normalize_board "$1")"
  if [ "$BUILD_DIR_SET" != 1 ]; then
    BUILD_DIR="$ROOT/build/$BOARD"
  fi
}

# ---- 编译固件（Docker 交叉编译，arm-none-eabi-gcc）----
do_build() {
  ensure_image
  mkdir -p "$BUILD_DIR"
  echo "[devops] 目标板 BOARD=$BOARD"
  echo "[devops] 容器内编译固件：RTOS=$RTOS GPIO_BACKEND=$GPIO_BACKEND"
  echo "[devops] 挂载 $ROOT -> $ROOT ，产物目录 $BUILD_DIR"
  docker run --rm \
      -u "$(id -u):$(id -g)" \
      -e BOARD="$BOARD" \
      -e RTOS="$RTOS" \
      -e GPIO_BACKEND="$GPIO_BACKEND" \
      -e BUILD_DIR="$BUILD_DIR" \
      -e GEN="$GEN" \
      -e TOOLCHAIN="$TOOLCHAIN" \
      -e APPLY_PATCHES="${APPLY_PATCHES:-1}" \
      -v "$ROOT:$ROOT" \
      -w "$ROOT" \
      "$IMG" bash -lc '
        set -e
        tools/apply-patches.sh
        cmake -S . -B "$BUILD_DIR" -G "$GEN" \
              -DBOARD="$BOARD" \
              -DRTOS="$RTOS" \
              -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
              -DGPIO_BACKEND="$GPIO_BACKEND"
        cmake --build "$BUILD_DIR"
      '
  echo "[devops] 完成（$BOARD）。产物:"
  echo "        ELF : $BUILD_DIR/stm32f103（主 ELF，供 GDB）"
  echo "        BIN : $BUILD_DIR/stm32f103.bin"
  echo "        HEX : $BUILD_DIR/stm32f103.hex"
}

# ---- 编译 host 单测程序（宿主 gcc；仅编译，不运行）----
do_build_test() {
  ensure_image
  local tbuild="${TEST_BUILD:-$ROOT/build/$BOARD/tests-gpio}"
  echo "[devops] 编译 host 单测程序（宿主 gcc，仅编译，不运行）：$tbuild"
  docker run --rm \
      -u "$(id -u):$(id -g)" \
      -e TEST_BUILD="$tbuild" \
      -e GEN="$GEN" \
      -v "$ROOT:$ROOT" \
      -w "$ROOT" \
      "$IMG" bash -lc '
        set -e
        cmake -S tests/gpio -B "$TEST_BUILD" -G "$GEN"
        cmake --build "$TEST_BUILD"
      '
  echo "[devops] host 单测程序已就绪，运行请用 ./tools/devops.sh test"
}

# ---- 运行 host 单测（只运行 build --host 已编译好的程序，不重新编译）----
do_test() {
  local tbuild="${TEST_BUILD:-$ROOT/build/$BOARD/tests-gpio}"
  if [ ! -f "$tbuild/test_rtthread_gpio" ]; then
    echo "[devops] 尚未编译 host 单测程序，请先运行：./tools/devops.sh build --host（或 --test）。" >&2
    exit 1
  fi
  ensure_image
  echo "[devops] 运行 host 单测（目标板 $BOARD，目录 $tbuild）"
  docker run --rm \
      -u "$(id -u):$(id -g)" \
      -e TEST_BUILD="$tbuild" \
      -v "$ROOT:$ROOT" \
      -w "$ROOT" \
      "$IMG" bash -lc '
        set -e
        ctest --test-dir "$TEST_BUILD" --output-on-failure
      '
  echo "[devops] 容器内测试完成（$BOARD）"
}

# ---- build 子命令：选项解析 + 编译（可 --host/--test 顺带编译 host 单测程序）----
build() {
  local host=0
  local b="$BOARD" r="$RTOS" g="$GPIO_BACKEND"
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --host|--test|-t) host=1 ;;
      --board)        b="${2:?--board 需要参数}"; shift ;;
      --board=*)      b="${1#*=}" ;;
      --rtos)         r="${2:?--rtos 需要参数}"; shift ;;
      --rtos=*)       r="${1#*=}" ;;
      --gpio-backend) g="${2:?--gpio-backend 需要参数}"; shift ;;
      --gpio-backend=*) g="${1#*=}" ;;
      --help|-h)      usage 0 ;;
      *) echo "[devops] build 未知选项: $1（可用 --board/--rtos/--gpio-backend/--host(--test)）" >&2; usage 1 ;;
    esac
    shift
  done

  apply_board "$b"
  RTOS="$r"
  GPIO_BACKEND="$g"

  do_build
  if [ "$host" = 1 ]; then
    do_build_test
  else
    echo "[devops] build 未带 --host，未编译 host 单测程序；运行单测请先 ./tools/devops.sh build --host，再 ./tools/devops.sh test"
  fi
}

# ---- 烧录（宿主机 openocd）----
flash() {
  local bin="${BIN:-$BUILD_DIR/stm32f103.bin}"
  if [ ! -f "$bin" ]; then
    echo "[devops] 未找到 $bin，请先运行 ./tools/devops.sh build 生成镜像。" >&2
    exit 1
  fi
  local config="${CONFIG:-$ROOT/tools/openocd-stm32f1.cfg}"
  echo "[devops] 烧录 $bin ..."
  openocd -f "$config" -c "program $bin 0x08000000 verify reset exit"
  echo "[devops] 烧录完成。"
}

# ---- 入口 ----
cmd="${1:-help}"; shift || true
case "$cmd" in
  build)        build "$@" ;;
  test|tests)   do_test ;;
  flash)        flash ;;
  help|-h|--help) usage 0 ;;
  *) echo "[devops] 未知命令: $cmd（可用 build|test|flash|help）" >&2; usage 1 ;;
esac
