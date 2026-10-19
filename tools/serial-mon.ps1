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
  The log file is always written as UTF-8 (no BOM).

.PARAMETER Charset
  Charset of the text sent by the MCU: utf8 (default) | gbk | ascii | latin1.
  NB: System.IO.Ports.SerialPort defaults to ASCII, which silently turns every
  non-ASCII byte into '?'. This firmware prints UTF-8, so the default is utf8.

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
  [string]$Log = "",
  [string]$Charset = "utf8"
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

# --- resolve charset: SerialPort.Encoding defaults to ASCII, which mangles UTF-8 ---
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
switch ($Charset.ToLower()) {
  'utf8'   { $enc = $utf8NoBom }
  'gbk'    { $enc = [System.Text.Encoding]::GetEncoding(936) }
  'ascii'  { $enc = [System.Text.Encoding]::ASCII }
  'latin1' { $enc = [System.Text.Encoding]::GetEncoding(28591) }
  default  { Write-Host ("[mon] unknown charset '{0}', falling back to utf8" -f $Charset) -ForegroundColor Yellow; $enc = $utf8NoBom }
}

$assert = [bool]$Bad
$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName    = $Port
$sp.BaudRate    = $Baud
$sp.Parity      = [System.IO.Ports.Parity]::None
$sp.DataBits    = 8
$sp.StopBits    = [System.IO.Ports.StopBits]::One
$sp.ReadTimeout = 200
$sp.Encoding    = $enc        # !!! must not stay at the ASCII default
$sp.DtrEnable   = $assert     # keep $false for normal use
$sp.RtsEnable   = $assert     # keep $false for normal use
try {
  $sp.Open()
} catch {
  Write-Host ("[mon] cannot open {0}: {1}" -f $Port, $_.Exception.Message) -ForegroundColor Red
  Write-Host "[mon] hint: another program (MobaXterm / SSCOM / another monitor) may still hold the port; close it and retry." -ForegroundColor Yellow
  exit 2
}
Write-Host ("[mon] {0} @ {1}  DTR={2} RTS={3}  charset={4}" -f $Port, $Baud, $assert, $assert, $Charset) -ForegroundColor Green
if ($Log) { Write-Host ("[mon] logging to {0}" -f $Log) -ForegroundColor Green }

$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { $null }
try {
  while ($true) {
    if ($deadline -and (Get-Date) -ge $deadline) { break }
    try {
      $s = $sp.ReadExisting()
      if ($s) {
        [Console]::Out.Write($s); [Console]::Out.Flush()
        if ($Log) { [System.IO.File]::AppendAllText($Log, $s, $utf8NoBom) }
      }
    } catch { }
    Start-Sleep -Milliseconds 40
  }
} finally {
  try { $sp.Close() } catch { }
}
