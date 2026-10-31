param(
  [Parameter(Mandatory=$true)][string]$TextFile,
  [Parameter(Mandatory=$true)][string]$OutFile,
  [string]$Voice = "",
  [int]$Rate = 0
)

Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer

if ($Voice -ne "") {
  $s.SelectVoice($Voice)
} else {
  $zh = $s.GetInstalledVoices() |
        Where-Object { $_.VoiceInfo.Culture.Name -like 'zh*' } |
        Select-Object -First 1
  if ($zh) { $s.SelectVoice($zh.VoiceInfo.Name) }
}

$s.Rate = $Rate
$s.SetOutputToWaveFile($OutFile)
$text = Get-Content -Raw -Encoding UTF8 $TextFile
$s.Speak($text)
$s.Dispose()
Write-Output "OK"
