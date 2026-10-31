<#
.SYNOPSIS
  Safe interactive serial monitor for ATK WarShip V4 (STM32F103).
  Opens the port with DTR and RTS DE-ASSERTED so the board's on-board
  "one-key download" circuit (CH340 DTR#/RTS# -> Q2/Q3 -> BOOT0/RESET)
  cannot pull BOOT0 high and make the MCU boot into ROM on reset.

.DESCRIPTION
  Why: on this board BOOT0 and RESET are wired to the CH340 control lines
  through transistors. If a serial program asserts DTR/RTS, the next reset
  can land in the system-memory bootloader (ROM) instead of Flash
  (and some combinations hold the MCU in reset).
  Opening with DtrEnable=$false, RtsEnable=$false avoids that.

  Interactive input:
    By default the keyboard is forwarded to the board, so you can type msh
    commands directly in this window. Special keys are translated:
      Enter       -> CR (0x0D)
      Backspace   -> 0x08
      Tab         -> 0x09
      Esc         -> 0x1B
      Up/Down     -> ESC [ A / ESC [ B   (msh command history)
      Left/Right  -> ESC [ D / ESC [ C
    Use -NoInput for scripted runs (or when stdin is redirected).

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
  Charset of the text sent/received: utf8 (default) | gbk | ascii | latin1.
  NB: System.IO.Ports.SerialPort defaults to ASCII, which silently turns every
  non-ASCII byte into '?'. This firmware prints UTF-8, so the default is utf8.

.PARAMETER NoInput
  Do not read the keyboard; only print what the board sends. Automatically
  enabled when stdin is redirected.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1 -Port COM6 -Baud 115200
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1 -Bad -Seconds 8
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\serial-mon.ps1 -Port COM4 -Log uart.log
#>
param(
  [string]$Port = "",
  [int]$Baud = 115200,
  [switch]$Bad,
  [int]$Seconds = 0,
  [string]$Log = "",
  [string]$Charset = "utf8",
  [switch]$NoInput
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

# Keyboard forwarding needs a real console. Redirected stdin (script/test) => RX only.
$interactive = (-not $NoInput) -and (-not [Console]::IsInputRedirected)

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
Write-Host ("[mon] {0} @ {1}  DTR={2} RTS={3}  charset={4}  input={5}" -f `
            $Port, $Baud, $assert, $assert, $Charset, $(if ($interactive) { "on (Ctrl+C 退出)" } else { "off" })) -ForegroundColor Green
if ($Log) { Write-Host ("[mon] logging to {0}" -f $Log) -ForegroundColor Green }

$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { $null }
try {
  while ($true) {
    if ($deadline -and (Get-Date) -ge $deadline) { break }

    # ---- board -> PC ----
    try {
      $s = $sp.ReadExisting()
      if ($s) {
        [Console]::Out.Write($s); [Console]::Out.Flush()
        if ($Log) { [System.IO.File]::AppendAllText($Log, $s, $utf8NoBom) }
      }
    } catch { }

    # ---- PC -> board ----
    if ($interactive) {
      while ([Console]::KeyAvailable) {
        $k = [Console]::ReadKey($true)
        $bytes = $null

        switch ($k.Key) {
          'Enter'       { $bytes = [byte[]](13) }
          'Backspace'   { $bytes = [byte[]](8) }
          'Tab'         { $bytes = [byte[]](9) }
          'Escape'      { $bytes = [byte[]](27) }
          'UpArrow'     { $bytes = [byte[]](27, 91, 65) }
          'DownArrow'   { $bytes = [byte[]](27, 91, 66) }
          'RightArrow'  { $bytes = [byte[]](27, 91, 67) }
          'LeftArrow'   { $bytes = [byte[]](27, 91, 68) }
          default {
            if ($k.KeyChar -ne [char]0) {
              $bytes = $enc.GetBytes([string]$k.KeyChar)
            }
          }
        }

        if ($bytes -and ($bytes.Length -gt 0)) {
          try { $sp.Write($bytes, 0, $bytes.Length) } catch { }
        }
      }
    }

    Start-Sleep -Milliseconds 40
  }
} finally {
  try { $sp.Close() } catch { }
}
