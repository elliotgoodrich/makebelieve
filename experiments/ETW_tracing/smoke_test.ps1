<#
.SYNOPSIS
  Black-box smoke test for the ETW tracing experiment, mirroring
  ../FUSE_tracing/smoke_test.sh: reads a handful of files (some more than
  once, some out of alphabetical order, one via ".." into the parent
  directory, one never at all), then diffs the command's entire output
  against an exact expectation.

  This asserts an *exact* match, same as FUSE_tracing's smoke test, even
  though ETW_tracing is documented as best-effort/lossy under load (see
  its README). For a handful of sequential opens in a short-lived scratch
  scenario like this one, real buffer loss is not expected - if this test
  ever does flake, the first thing to check is genuine NT Kernel Logger
  event loss (e.g. a heavily loaded CI runner), not a test bug.

.PARAMETER TracingExe
  Path to the etw_tracing_experiment binary.

.PARAMETER CatfilesScript
  Path to catfiles.ps1 (this experiment's own copy - see
  ../detours_tracing/CMakeLists.txt, which reuses this same script rather
  than duplicating it). Run via a real child powershell.exe process (the
  traced command), not dot-sourced - see catfiles.ps1's own header
  comment for why that's known to still be byte-exact.
#>
param(
  [Parameter(Mandatory = $true)][string]$TracingExe,
  [Parameter(Mandatory = $true)][string]$CatfilesScript
)

$ErrorActionPreference = 'Stop'

# Consuming NT Kernel Logger events always needs elevation, regardless of
# what the traced command itself needs - fail with a clear diagnostic
# rather than the far more confusing StartTraceW ERROR_ACCESS_DENIED the
# binary itself would otherwise report. Fails (not skips), matching
# ../FUSE_tracing/smoke_test.sh's policy for /dev/fuse being unavailable.
$isElevated = ([Security.Principal.WindowsPrincipal] `
  [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isElevated) {
  Write-Host "FAIL: this test must run elevated (Administrator) - " `
    "consuming NT Kernel Logger events requires it"
  exit 1
}

$parent = Join-Path ([System.IO.Path]::GetTempPath()) ("etw-tracing-" + [guid]::NewGuid())
$workDir = Join-Path $parent "work"
New-Item -ItemType Directory -Path $workDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $workDir "sub") | Out-Null

Set-Content -Path (Join-Path $workDir "top.txt") -Value "top-level content"
Set-Content -Path (Join-Path $workDir "sub\nested.txt") -Value "nested content"
Set-Content -Path (Join-Path $workDir "aaa.txt") -Value "aaa content"
Set-Content -Path (Join-Path $workDir "untouched.txt") -Value "untouched content"
Set-Content -Path (Join-Path $parent "outside.txt") -Value "outside content"

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

# Reads top.txt and sub\nested.txt twice each (dedup), aaa.txt last even
# though it must sort first (sort-order), and ..\outside.txt once (must
# not be tracked - it falls outside the current directory).
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

Cleanup
exit 0
