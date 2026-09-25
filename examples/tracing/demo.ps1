# Mounts this example, reads every output, rewrites input.txt to cause a
# rebuild, and saves what makebelieve recorded to trace.json beside this script.
#
#   .\demo.ps1 [-Makebelieve <path to makebelieve.exe>]
param(
  [string]$Makebelieve = "$PSScriptRoot\..\..\build\src\Debug\makebelieve.exe"
)
$ErrorActionPreference = 'Stop'

$exe = (Resolve-Path $Makebelieve).Path
$mount = Join-Path $env:TEMP 'makebelieve-tracing-demo'
$saved = Join-Path $PSScriptRoot 'trace.json'

# No redirection: a redirected Start-Process makes the daemon inherit handles
# it would pass on to every build command, which then never finish.
Start-Process -FilePath $exe -ArgumentList 'mount', $mount `
  -WorkingDirectory $PSScriptRoot -WindowStyle Hidden | Out-Null
try {
  $deadline = (Get-Date).AddSeconds(10)
  while (-not (Test-Path "$mount\tracing.json")) {
    if ((Get-Date) -gt $deadline) { throw "makebelieve did not mount $mount" }
    Start-Sleep -Milliseconds 100
  }

  Write-Host 'Reading every output (the first read of each builds it)...'
  foreach ($output in 'fast.txt', 'slow.txt', 'copied.txt', 'nested\direct.txt') {
    $content = (Get-Content -Raw "$mount\$output").Trim()
    Write-Host "  $output -> $content"
  }

  Write-Host 'Rewriting input.txt, which rebuilds the outputs that read it...'
  $bytes = [IO.File]::ReadAllBytes("$PSScriptRoot\input.txt")
  [IO.File]::WriteAllBytes("$PSScriptRoot\input.txt", $bytes)
  Start-Sleep -Seconds 1
  Get-Content -Raw "$mount\copied.txt" | Out-Null

  # Read before unmounting, as the trace only exists while mounted.
  $trace = [IO.File]::ReadAllText("$mount\tracing.json")
} finally {
  & $exe unmount $mount
}

[IO.File]::WriteAllText($saved, $trace)
$events = ConvertFrom-Json $trace
Write-Host ''
Write-Host "Saved $($events.Count) events to $saved"
Write-Host 'Open it at https://ui.perfetto.dev (Open trace file) or chrome://tracing.'
