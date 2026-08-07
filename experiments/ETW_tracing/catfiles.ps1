<#
.SYNOPSIS
  Prints each named file's raw content to stdout, in argument order - a
  PowerShell stand-in for POSIX `cat`, replacing the previous catfiles.exe.
  Exists purely so smoke_test.ps1 (here and in ../detours_tracing) can
  drive its tracing experiment with a real child process that opens files
  for reading, the same way FUSE_tracing/smoke_test.sh drives it with
  `cat`.

  Get-Content -Raw was verified (not assumed) to be exactly byte-for-byte
  faithful for this purpose: piped through Write-Host in a real child
  powershell.exe process - the same path this script itself uses - its
  output matched a raw concatenation of the source files exactly,
  including line endings, with no re-encoding drift and no extra bytes.
  cmd.exe's `type` was also verified byte-perfect on stdout, but was
  rejected anyway because it writes each filename to stderr when given
  more than one file, which would land in the smoke tests' merged
  stdout+stderr capture and break their exact-diff assertions.

.PARAMETER Path
  One or more files to print, in order. Positional - not a named
  parameter - so the traced command line reads like `cat file1 file2`.
#>
param(
  [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
  [string[]]$Path
)

$ok = $true
foreach ($p in $Path) {
  try {
    $content = Get-Content -LiteralPath $p -Raw -ErrorAction Stop
    if ($null -ne $content) {
      Write-Host -NoNewline $content
    }
  } catch {
    [Console]::Error.WriteLine("catfiles.ps1: ${p}: $($_.Exception.Message)")
    $ok = $false
  }
}
if ($ok) { exit 0 } else { exit 1 }
