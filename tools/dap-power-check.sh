#!/usr/bin/env bash
# =============================================================================
# dap-power-check.sh - 检测 DAP 类型，判断能否软件禁止其给目标板供电
#
# 背景：CMSIS-DAP 协议规范本身没有"目标板电源开关"标准命令。能否软件关
#   供电取决于：(a) 固件是否为标准 DAPLink；(b) 硬件上 3V3 是否接在受固件
#   控制的 POWER_EN 开关管上。两者缺一就只能物理隔离。
# 用法：在插着 DAP 的宿主机终端执行：  bash tools/dap-power-check.sh
# =============================================================================
set -euo pipefail

echo "== 1) 检测 USB 中的调试器 (lsusb) =="
if command -v lsusb >/dev/null 2>&1; then
  lsusb | grep -iE 'cmsis|daplink|dap-link|dap|hover|keil|st-link|0d28' \
    || echo "（未匹配到 DAP，请确认已插好并重新插拔一次）"
else
  echo "（无 lsusb，跳过）"
fi

echo
echo "== 2) 是否已安装 pyocd =="
if command -v pyocd >/dev/null 2>&1; then
  pyocd --version
else
  echo "pyocd 未安装。仅当要尝试\"软件关供电\"时才需要（且只对 DAPLink 固件+带电源开关的 DAP 有效）："
  echo "    python3 -m venv ~/pyocd-venv"
  echo "    source ~/pyocd-venv/bin/activate"
  echo "    pip install pyocd"
fi

echo
echo "== 3) pyocd 探测（看固件类型 / 是否暴露 target power）=="
if command -v pyocd >/dev/null 2>&1; then
  pyocd list --verbose || echo "（pyocd 连接失败——可能是权限：见 tools/99-cmsis-dap.rules）"
fi

echo
echo "==== 判定与下一步 ===="
echo "【若】第 3 步显示固件为 DAPLink，且带 target power / target voltage："
echo "    可软件关供电（烧录结束时断开 DAPLink 目标电源）："
echo "      pyocd flash -t stm32f103rb --target-power disconnect build/stm32f103/rt-thread/stm32f103.bin"
echo "【否则】（大概率——非 DAPLink / 无电源项，硬件就没有 POWER_EN 开关）："
echo "    软件关不掉，只能物理隔离（Linux/Windows 通用，与系统无关）："
echo "      ① 退掉/绝缘排线里 3V3（及其它如 5V 供电线）那一根，套热缩管留空；"
echo "      ② 或在目标板 SWD 座把 3V3 与板载电源隔离，只用 SWDIO/SWCLK/GND/RST；"
echo "    禁掉 DAP 供电后，目标板必须用自己板载电源（如 USB CH340 那路 3.3V）。"