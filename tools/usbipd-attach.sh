#!/usr/bin/env bash
# =============================================================================
# usbipd-attach.sh —— 把板子的 USB 设备(DAP / CH340)从 Windows 通过 usbipd
# 自动 attach 进 WSL，供 openocd 烧录 / 串口监听使用。
#
# 用法：  ./tools/usbipd-attach.sh [--only dap|ch340]
#
# 依赖：
#   - 本脚本在 WSL 内运行；通过完整路径调用 Windows PowerShell 里的 usbipd。
#   - usbipd-win 已安装且板子设备已 bind --auto（共享）。若设备未 bind，
#     需在【管理员】PowerShell 里先执行： usbipd bind --auto -b <BUSID>
#
# 说明：
#   - DAP  (04d8:00df, ATK-FS-HID-CMSIS-DAP / USB Serial Device) —— 烧录用
#   - CH340(1a86:7523) —— 串口(默认)。

set -u

PS="$(
  [ -x /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe ] \
    && echo /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
    || command -v powershell.exe
)"
[ -x "$PS" ] || { echo "[usbipd-attach] 找不到 powershell.exe（需在 WSL 内运行）" >&2; exit 2; }

ONLY="all"
if [ "${1:-}" = "--only" ]; then
    ONLY="${2:-all}"
fi
PS1="/mnt/c/Windows/Temp/usbipd_attach_${$}.ps1"

# 生成挂临时 PowerShell 脚本，避免多层转义（ps1 固定 UTF-8 BOM + 全 ASCII，规避 GBK 乱码）
printf '\xef\xbb\xbf' > "$PS1"
cat >> "$PS1" <<'EOF'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" ' + ($args -join ' ')
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList $argList | Out-Null
    exit 0
}

param([string]$Only = "all")
$vids = @('04d8:00df','1a86:7523')
if ($Only -eq 'dap')   { $vids = @('04d8:00df') }
if ($Only -eq 'ch340') { $vids = @('1a86:7523') }

$l = usbipd list
Write-Output '=== usbipd list ==='
$l

foreach ($m in $vids) {
  $ln = $null
  for ($k = 0; $k -lt $l.Count; $k++) {
    if ($l[$k] -match '^Connected:') {
      for ($j = $k+1; $j -lt $l.Count; $j++) {
        if ($l[$j] -match '^\s*$') { break }
        if ($l[$j] -match 'Persisted:') { break }
        if ($l[$j] -Like "*$m*") { $ln = $l[$j]; break }
      }
      break
    }
  }
  if ($ln) {
    $bid = (($ln -split '\s{2,}')[0].Trim())
    Write-Output ("attach -> busid " + $bid + " (" + $m + ")")
    usbipd attach --wsl -b $bid
  } else {
    Write-Output ("NOT-FOUND/CONNECTED (" + $m + ") -> run bind --auto -b <BUSID> in admin PS, then retry")
  }
}
EOF

echo "== 用 usbipd 自动 attach 到 WSL（--only=$ONLY）=="
timeout 120 "$PS" -NoProfile -ExecutionPolicy Bypass -File "C:\\Windows\\Temp\\usbipd_attach_${$}.ps1" "$ONLY" 2>&1
rm -f "$PS1"

echo "== 等待 WSL 侧枚举 =="
sleep 4
lsusb 2>/dev/null | grep -iE '04d8|1a86|cmsis|ch34' || echo "(WSL 暂未枚举到目标设备)"
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "(暂无 /dev/ttyACM* / ttyUSB*)"