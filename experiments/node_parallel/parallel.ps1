# Mounts this experiment and times node loading ten slow outputs in each way,
# printing a markdown table. See README.md.
#
# Usage: parallel.ps1 [-Makebelieve path\to\makebelieve.exe]
param([string]$Makebelieve = "$PSScriptRoot\..\..\build\src\Debug\makebelieve.exe")
$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Makebelieve)) { throw "makebelieve not found at $Makebelieve" }
$work = Join-Path ([IO.Path]::GetTempPath()) "makebelieve-parallel-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$srcdir = "$work\src"; $mnt = "$work\mnt"
New-Item -ItemType Directory -Force $srcdir | Out-Null
Copy-Item "$PSScriptRoot\build.makebelieve", "$PSScriptRoot\module.mjs", "$PSScriptRoot\module.js" $srcdir

$daemon = Start-Process -FilePath $Makebelieve -ArgumentList 'mount', $mnt `
    -WorkingDirectory $srcdir -WindowStyle Hidden -PassThru
try {
  foreach ($i in 1..100) { if (Test-Path "$mnt\static0.mjs") { break }; Start-Sleep -Milliseconds 50 }
  if (-not (Test-Path "$mnt\static0.mjs")) { throw 'makebelieve did not mount' }
  '| How the 10 files are loaded | Time |'
  '| --- | --- |'
  foreach ($mode in 'static', 'dynamic', 'require', 'promises', 'callback', 'pool16', 'prefetch') {
    if ($mode -eq 'pool16') { $env:UV_THREADPOOL_SIZE = '16' }
    node "$PSScriptRoot\load.mjs" $mnt $mode
    Remove-Item Env:UV_THREADPOOL_SIZE -ErrorAction SilentlyContinue
  }
} finally {
  & $Makebelieve unmount $mnt | Out-Null
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
