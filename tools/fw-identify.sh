#!/usr/bin/env bash
# tools/fw-identify.sh - 判断当前 Flash 烧的是 zephyr 还是 rt-thread（在线调试）
#
# 只读、不擦写、不运行固件，所以不受这颗 DAP 复位进 ROM / 调试暂停影响
# （见 docs/hello-world-issue-and-fix.md P5/P6），也不需要看串口启动横幅。
# 两种判定证据（都来自系统固件自身携带的内容）：
#   1. 向量表（快，秒出）：Flash 0x08000000 的初始 SP + 0x08000004 的 Reset 向量；
#   2. 品牌/版本字符串（更“系统化”的证据，可强制 --strings）：
#        zephyr    : "*** Booting Zephyr OS build ... ***"（约位于 0x08003977）
#        rt-thread : "2006 - 2024 Copyright by RT-Thread team"（约位于 0x08007771）
#
# 用法：
#   ./tools/fw-identify.sh [--flasher dap|stlink] [--strings]
#     --strings   强制用 "dump Flash + 搜特征串" 判定（不检查向量表）
#     默认：先向量表判定；失败或匹配不到时自动回退到搜特征串。
#
# 前提：宿主机有 openocd，且调试器当前能稳定 halt（DAP 不稳时可多试几次）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

flasher="dap"
want_strings=0
while [ $# -gt 0 ]; do
  case "$1" in
    --flasher) flasher="${2:?需要参数}"; shift 2 ;;
    --flasher=*) flasher="${1#*=}" ;;
    --strings) want_strings=1 ;;
    -h|--help) awk 'NR>=2{ if(/^#/){sub(/^#[ ]*/,"");print} else if(/^set -uo/){exit} }' "$0" ; exit 0 ;;
    *) echo "未知选项: $1（--flasher dap|stlink [--strings]）" >&2; exit 1 ;;
  esac
done

case "$flasher" in
  dap|cmsis-dap) cfg="$ROOT/tools/openocd-stm32f1-dap.cfg" ;;
  stlink|st-link) cfg="$ROOT/tools/openocd-stm32f1.cfg" ;;
  *) echo "[fw-identify] 未知 flasher: $flasher（支持 dap / stlink）" >&2; exit 1 ;;
esac

read_word() {  # $1 = 地址（如 0x08000000）; 输出 hex 值字符串
  openocd -f "$cfg" \
      -c 'gdb_port 0' \
      -c 'adapter speed 1000' \
      -c 'init' \
      -c "mdw $1 1" \
      -c 'exit' 2>&1 | grep -E "^$1:" | awk '{print $2}'
# 兜底判定：dump Flash 前段，本地搜系统自带的品牌/版本字符串。
dump_and_probe() {  # $1 = 输出文件
  local dump="$1"
  echo "[fw-identify] 搜系统特征字符串（dump Flash 前 256 KiB）..."
  if ! openocd -f "$cfg" \
        -c 'gdb_port 0' \
        -c 'adapter speed 400' \
        -c 'init' \
        -c "flash read_bank 0 \"$dump\" 0 0x40000" \
        -c 'exit' 2>/dev/null; then
    echo "[fw-identify] dump Flash 失败（调试器未连接或未能 halt）。" >&2
    return 1
  fi
  if strings -n 5 "$dump" | grep -qiE 'Booting Zephyr|Zephyr OS build'; then
    echo "[fw-identify] 判定：zephyr —— Flash 含 \"Booting Zephyr OS build ...\""
  elif strings -n 5 "$dump" | grep -qiE 'Copyright by RT-Thread|RT-Thread [0-9]+'; then
    echo "[fw-identify] 判定：rt-thread —— Flash 含 \"Copyright by RT-Thread team\""
  else
    echo "[fw-identify] 未在 Flash 前 256 KiB 找到可识别特征串（可能是其他/旧固件）。"
  fi
}

dump="$(mktemp)"
trap 'rm -f "$dump"' EXIT

# 强制走字符串法（看“系统内容”级别的证据）
if [ "$want_strings" = "1" ]; then
  dump_and_probe "$dump"
  exit 0
fi

echo "[fw-identify] 读 Flash 向量表（flasher=$flasher）..."
sp="$(read_word 0x08000000 || true)"
reset="$(read_word 0x08000004 || true)"

if [ -n "$sp" ] && [ -n "$reset" ]; then
  echo "[fw-identify] SP      (0x08000000) = $sp"
  echo "[fw-identify] Reset   (0x08000004) = $reset"
fi

case "$sp:$reset" in
  0x20001060:0x080008e5)
    echo "[fw-identify] 判定：zephyr —— 与 zephyr.bin 向量表一致" ;;
  0x20010000:0x0800122d)
    echo "[fw-identify] 判定：rt-thread —— 与 rt-thread.bin 向量表一致" ;;
  *)
    # 向量表读不出或匹配不到 → 回退到系统字符串证据
    echo "[fw-identify] 向量表未匹配到已知固件（读不出或为其他/旧固件），改搜系统特征串..."
    dump_and_probe "$dump"
    ;;
esac
}