<#
.SYNOPSIS
  Safe serial monitor for ATK WarShip V4 (STM32F103).
  Opens the port with DTR and RTS DE-ASSERTED so the board's on-board
  "one-key download" circuit (CH340 DTR#/RTS# -> Q2/Q3 -> BOOT0/RESET)
  cannot pull BOOT0 high and make the MCU boot into ROM on reset.

.DESCRIPTION
  Why: on this board BOOT0 and RESET are wired to the CH340 control lines
  through transistors. If a serial program asserts DTR/RTS, the next reset
  can land in the system-memory bootloader (ROM) instead of Flash
  (and some combinations hold the MCU in reset).
  Opening with DtrEnable=$false, RtsEnable=$false avoids that.

.PARAMETER Port
  COM port. If omitted, the first CH340 port is used, otherwise the first COM port.

.PARAMETER Baud
  Baud rate (default 115200).

.PARAMETER Bad
  Reproduce the "bad" state: assert DTR and RTS (board will go to ROM on reset).

.PARAMETER Seconds
  Exit after N seconds (0 = run until Ctrl+C). Useful for scripted tests.

.PARAMETER Log
  Also append everything received to this file (useful to capture a boot log).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1 -Port COM6 -Baud 115200
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1 -Bad -Seconds 8
#>
param(
  [string]$Port = "",
  [int]$Baud = 115200,
  [switch]$Bad,
  [int]$Seconds = 0,
  [string]$Log = ""
)

if (-not $Port) {
  $ch = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'CH340' -and $_.Name -match '(COM\d+)' } |
        Select-Object -First 1
  if ($ch) {
    if ($ch.Name -match '(COM\d+)') { $Port = $Matches[1] }
  }
  if (-not $Port) { $Port = [System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -First 1 }
}
if (-not $Port) { Write-Host "[mon] no COM port found"; exit 1 }

$assert = [bool]$Bad
$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName    = $Port
$sp.BaudRate    = $Baud
$sp.Parity      = [System.IO.Ports.Parity]::None
$sp.DataBits    = 8
$sp.StopBits    = [System.IO.Ports.StopBits]::One
$sp.ReadTimeout = 200
$sp.DtrEnable   = $assert     # keep $false for normal use
$sp.RtsEnable   = $assert     # keep $false for normal use
try {
  $sp.Open()
} catch {
  Write-Host ("[mon] cannot open {0}: {1}" -f $Port, $_.Exception.Message) -ForegroundColor Red
  Write-Host "[mon] hint: another program (MobaXterm / SSCOM / another monitor) may still hold the port; close it and retry." -ForegroundColor Yellow
  exit 2
}
Write-Host ("[mon] {0} @ {1}  DTR={2} RTS={3}" -f $Port, $Baud, $assert, $assert) -ForegroundColor Green
if ($Log) { Write-Host ("[mon] logging to {0}" -f $Log) -ForegroundColor Green }

$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { $null }
try {
  while ($true) {
    if ($deadline -and (Get-Date) -ge $deadline) { break }
    try {
      $s = $sp.ReadExisting()
      if ($s) {
        [Console]::Out.Write($s); [Console]::Out.Flush()
        if ($Log) { Add-Content -Path $Log -Value $s -NoNewline -ErrorAction SilentlyContinue }
      }
    } catch { }
    Start-Sleep -Milliseconds 40
  }
} finally {
  try { $sp.Close() } catch { }
}
