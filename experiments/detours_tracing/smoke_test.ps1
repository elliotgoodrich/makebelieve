<#
.SYNOPSIS
  Black-box smoke test for the Detours tracing experiment, mirroring
  ../FUSE_tracing/smoke_test.sh.

  Two phases:
    1. The same dedup/sort/parent-dir-exclusion scenario the other
       tracing experiments use, diffed against an exact expectation.
    2. A child-process phase specific to this tracer: runs catfiles as a
       grandchild (via cmd.exe /c), checking that detours_tracing_hook.dll's
       CreateProcess re-injection (see its own header comment) actually
       makes the grandchild's reads visible - a behavior that's free for a
       mount-based tracer (FUSE_tracing, or a ProjFS one) since anything
       touching the mount is seen regardless of which process does it, but
       something this process-hooking approach has to earn explicitly.

.PARAMETER TracingExe
  Path to the detours_tracing_experiment binary.

.PARAMETER CatfilesScript
  Path to catfiles.ps1 (../ETW_tracing's copy; see
  ../ETW_tracing/CMakeLists.txt for why it's shared rather than
  duplicated). Run via a real child powershell.exe process (the traced
  command, or - in phase 2 - the grandchild), not dot-sourced.
#>
param(
  [Parameter(Mandatory = $true)][string]$TracingExe,
  [Parameter(Mandatory = $true)][string]$CatfilesScript
)

$ErrorActionPreference = 'Stop'

$parent = Join-Path ([System.IO.Path]::GetTempPath()) ("detours-tracing-" + [guid]::NewGuid())
$workDir = Join-Path $parent "work"
New-Item -ItemType Directory -Path $workDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $workDir "sub") | Out-Null

Set-Content -Path (Join-Path $workDir "top.txt") -Value "top-level content"
Set-Content -Path (Join-Path $workDir "sub\nested.txt") -Value "nested content"
Set-Content -Path (Join-Path $workDir "aaa.txt") -Value "aaa content"
Set-Content -Path (Join-Path $workDir "untouched.txt") -Value "untouched content"
Set-Content -Path (Join-Path $parent "outside.txt") -Value "outside content"
Set-Content -Path (Join-Path $workDir "child.txt") -Value "child content"

function Cleanup {
  Remove-Item -Recurse -Force $parent -ErrorAction SilentlyContinue
}

function Fail([string]$Message, [string[]]$Actual) {
  Write-Host "FAIL: $Message"
  Write-Host "--- actual output ---"
  $Actual | ForEach-Object { Write-Host $_ }
  Cleanup
  exit 1
}

# --- Phase 1: dedup, sort order, parent-dir exclusion ---
Push-Location $workDir
try {
  $rawOutput = & $TracingExe powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File $CatfilesScript "top.txt" "sub\nested.txt" `
    "top.txt" "sub\nested.txt" "..\outside.txt" "aaa.txt" 2>&1
} finally {
  Pop-Location
}
$actual = @($rawOutput | ForEach-Object { $_.ToString() } | Where-Object { $_ -ne "" })

$expected = @(
  "top-level content",
  "nested content",
  "top-level content",
  "nested content",
  "outside content",
  "aaa content",
  "Files read from ${workDir}:",
  "  $workDir\aaa.txt",
  "  $workDir\sub\nested.txt",
  "  $workDir\top.txt"
)

if (($actual -join "`n") -ne ($expected -join "`n")) {
  Fail "report did not match exactly (expected $($expected.Count) lines, got $($actual.Count))" $actual
}
Write-Host "OK: report matches exactly" `
  "(dedup, parent-dir exclusion, and sort order all correct)"

# --- Phase 2: a grandchild process's reads are traced too ---
Push-Location $workDir
try {
  $childOutput = & $TracingExe cmd.exe /c powershell.exe -NoProfile `
    -ExecutionPolicy Bypass -File $CatfilesScript "child.txt" 2>&1
} finally {
  Pop-Location
}
$childActual = @($childOutput | ForEach-Object { $_.ToString() } | Where-Object { $_ -ne "" })

if (-not ($childActual -contains "  $workDir\child.txt")) {
  Fail "grandchild process's read of child.txt was not reported - CreateProcess re-injection may not be working" $childActual
}
Write-Host "OK: a file read by a grandchild process (via cmd.exe /c) is reported"

Cleanup
exit 0
