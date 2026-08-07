<#
.SYNOPSIS
  Black-box smoke test for the ProjFS experiment, mirroring
  ../FUSE/smoke_test.sh:
    1. Nothing is notified while nobody reads the file.
    2. A read is followed by exactly one change notification.
    3. Reading again afterwards shows the content actually changed.
    4. That second read schedules exactly one more notification.
    5. It also checks that writes from anyone but the provider itself are
       rejected.

  filewatch's own stdout is redirected straight to a file via process
  redirection (not a job pipe) - piping a background job's output through
  Add-Content buffers inside the job's own pipeline and can sit unflushed
  for seconds, which is difficult for a test that polls for events arriving
  within a few hundred milliseconds of a read.

.PARAMETER ProjfsExe
  Path to the projfs_experiment binary.

.PARAMETER FilewatchExe
  Path to the filewatch binary.
#>
param(
  [Parameter(Mandatory = $true)][string]$ProjfsExe,
  [Parameter(Mandatory = $true)][string]$FilewatchExe
)

$ErrorActionPreference = 'Stop'

$mnt = Join-Path ([System.IO.Path]::GetTempPath()) ("projfs-mnt-" + [guid]::NewGuid())
$tmpBase = Join-Path ([System.IO.Path]::GetTempPath()) ("projfs-log-" + [guid]::NewGuid())
$readLog = "$tmpBase.reads.log"
$watchOut = "$tmpBase.watch.out"
$providerOut = "$tmpBase.provider.out"
$providerErr = "$tmpBase.provider.err"
New-Item -ItemType File -Path $readLog | Out-Null

$providerProcess = $null
$watchProcess = $null

function Cleanup {
  if ($watchProcess -and -not $watchProcess.HasExited) {
    Stop-Process -Id $watchProcess.Id -Force -ErrorAction SilentlyContinue
  }
  if ($providerProcess -and -not $providerProcess.HasExited) {
    # projfs_experiment normally stops on Ctrl+C from its own console; for
    # an automated run we just terminate it, and rely on ProjFS itself to
    # tear down the virtualization instance when the owning process's
    # handles close - there's no clean-unmount step to wait on the way
    # fusermount3 -u gives the FUSE test.
    Stop-Process -Id $providerProcess.Id -Force -ErrorAction SilentlyContinue
  }
  Remove-Item -Recurse -Force $mnt -ErrorAction SilentlyContinue
  Remove-Item -Force $readLog, $watchOut, $providerOut, $providerErr -ErrorAction SilentlyContinue
}

# Reports the failure (without throwing - Write-Error under
# $ErrorActionPreference = 'Stop' is a terminating statement, which would
# skip Cleanup and every line after it, leaking the provider process),
# dumps the logs for debugging, cleans up, and exits nonzero.
function Fail([string]$Message) {
  Write-Host "FAIL: $Message"
  Write-Host "--- reads ---"
  if (Test-Path $readLog) { Get-Content $readLog | Write-Host }
  Write-Host "--- filewatch output ---"
  if (Test-Path $watchOut) { Get-Content $watchOut | Write-Host }
  Write-Host "--- provider stderr ---"
  if (Test-Path $providerErr) { Get-Content $providerErr | Write-Host }
  Cleanup
  exit 1
}

# Records a "READ <time>" line into $readLog (for failure diagnostics) and
# returns the timestamp for the caller to compare across reads.
function Record-Read {
  $content = Get-Content -Raw (Join-Path $mnt "time.txt")
  if ($content -notmatch '\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}') {
    Fail "could not find a timestamp in time.txt's content: [$content]"
  }
  $t = $Matches[0]
  Add-Content -Path $readLog -Value "READ $t"
  return $t
}

function Event-Count {
  if (-not (Test-Path $watchOut)) { return 0 }
  return (Select-String -Path $watchOut -Pattern '^EVENT$' -ErrorAction SilentlyContinue |
          Measure-Object).Count
}

# Polls Event-Count in 0.1s steps, returning as soon as it reaches $Target,
# rather than always waiting the full $TimeoutSeconds ceiling - the update a
# read schedules lands at the next whole-second boundary, so it's typically
# well under 1s away, not a fixed worst case. $TimeoutSeconds is only a
# safety ceiling for a slow/loaded machine, not the expected duration.
function Wait-ForCount([int]$Target, [double]$TimeoutSeconds) {
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $count = 0
  while ((Get-Date) -lt $deadline) {
    $count = Event-Count
    if ($count -ge $Target) { break }
    Start-Sleep -Milliseconds 100
  }
  return $count
}

# Re-reads time.txt until its content differs from $Previous, returning the
# new content's timestamp (or the last one seen if the timeout elapses first,
# so the caller can still assert and fail loudly). Polls in 50ms steps,
# recording every read into $readLog for failure diagnostics.
#
# Why phase 3 needs to poll here at all, when the FUSE sibling
# (../FUSE/smoke_test.sh) asserts on a single re-read: the two providers
# make "the content changed" observable in fundamentally different ways.
# FUSE opens time.txt with direct_io (see fuse.m.cpp's FUSE::open), so the
# kernel re-runs the read callback on *every* read, regenerating the content
# from the live wall clock; once the phase-2 notification has been seen the
# clock has already crossed the next-second boundary, so the very next read
# is synchronously guaranteed a new timestamp. ProjFS has no direct_io
# equivalent: its content is a pure function of the placeholder's stored
# ContentID, and fire_update()'s PrjUpdateFileIfNeeded (see projfs.m.cpp) is
# a *metadata-only* push. That push bumps LastWriteTime - which is what fires
# the notification phase 2 waited for - but ProjFS re-virtualizes the file's
# data a hair *after* that metadata bump becomes observable to a watcher. So
# a single read taken the instant the notification is seen can still be
# served the previous second's already-hydrated content, even though the
# update itself always advances the ContentID to a strictly later second
# (fire_update stamps >= t1_second + 1, so any *fresh* re-hydration is
# guaranteed to differ - the same-second read only ever comes from that brief
# stale-cache window, never from the update landing in the same second).
#
# Polling until the content actually reflects the update - rather than
# sleeping a fixed amount and hoping - keeps the assertion honest: a provider
# that genuinely never changes the content still fails, and fails within
# $TimeoutSeconds rather than hanging. It also preserves phase 4's
# "exactly one more notification" invariant: only the read that actually
# re-hydrates re-arms an update (debounced in the provider via
# g_update_scheduled), and it does so exactly once; the stale reads ahead of
# it are served from cache without re-hydrating, and the loop stops the
# moment the first fresh read lands, well before that re-armed update fires.
function Wait-ForChangedContent([string]$Previous, [double]$TimeoutSeconds) {
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $t = Record-Read
  while ($t -eq $Previous -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 50
    $t = Record-Read
  }
  return $t
}

Add-Type -Name Kernel -Namespace ProjfsSmokeTest -MemberDefinition @"
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool AttachConsole(uint dwProcessId);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetConsoleCtrlHandler(IntPtr HandlerRoutine, bool Add);
"@

# Sends a real CTRL_C_EVENT to $Process, the same signal a user pressing
# Ctrl+C in its console would send - Cleanup's Stop-Process (TerminateProcess)
# skips the provider's whole shutdown path (PrjStopVirtualizing and removing
# the virtualization root), so this is the only way to actually exercise
# that path from an automated test. Returns $true if the signal was sent.
function Send-CtrlC([System.Diagnostics.Process]$Process) {
  [ProjfsSmokeTest.Kernel]::FreeConsole() | Out-Null
  if (-not [ProjfsSmokeTest.Kernel]::AttachConsole($Process.Id)) {
    return $false
  }
  # Ignore Ctrl+C in this script's own (now shared) console so it doesn't
  # get killed along with the provider.
  [ProjfsSmokeTest.Kernel]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
  [ProjfsSmokeTest.Kernel]::GenerateConsoleCtrlEvent(0, 0) | Out-Null  # CTRL_C_EVENT
  [ProjfsSmokeTest.Kernel]::FreeConsole() | Out-Null
  return $true
}

# Not pre-created: projfs_experiment now creates (and fails if it can't
# create) its own virtualization root, matching how it must always be
# freshly created. $mnt's GUID-based name already guarantees it doesn't
# exist yet.

$providerProcess = Start-Process -FilePath $ProjfsExe -ArgumentList $mnt `
  -RedirectStandardOutput $providerOut -RedirectStandardError $providerErr `
  -WindowStyle Hidden -PassThru

# Wait for the provider to report it's up.
$ready = $false
$deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $deadline) {
  if ($providerProcess.HasExited) { break }
  if ((Test-Path $providerOut) -and (Get-Content $providerOut -Raw) -match 'running at') {
    $ready = $true
    break
  }
  Start-Sleep -Milliseconds 100
}
if (-not $ready) {
  $errText = if (Test-Path $providerErr) { Get-Content $providerErr -Raw } else { "" }
  Fail "projfs_experiment did not start (ProjFS unavailable on this machine?): $errText"
}

# One continuous watch spanning the whole scenario below, rather than
# re-arming per phase - a fresh watch started right before a read could
# race the update it's trying to observe.
$watchProcess = Start-Process -FilePath $FilewatchExe -ArgumentList @((Join-Path $mnt "time.txt"), "10") `
  -RedirectStandardOutput $watchOut -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 500  # give the watch time to arm

# --- 1. nobody has read the file yet: no notification should arrive ---
Start-Sleep -Seconds 1.5
$count = Event-Count
if ($count -ne 0) {
  Fail "got $count notification(s) with nobody reading (expected 0)"
}
Write-Host "OK: no notification while idle"

# --- 2. a read is followed by exactly one notification ---
$t1 = Record-Read
$count = Wait-ForCount 1 3
if ($count -ne 1) {
  Fail "expected exactly 1 notification after a read, got $count"
}
Write-Host "OK: exactly one notification after a read"

# --- 3. reading again shows the content actually changed ---
# Poll rather than a single read: ProjFS makes the new content observable a
# hair after the notification, so the first post-notification read can still
# be served the previous second's cached content (see Wait-ForChangedContent
# for the full FUSE-vs-ProjFS rationale). The bounded timeout means a
# provider that never changes the content still fails, and fails fast.
$t2 = Wait-ForChangedContent $t1 3
if ($t1 -eq $t2) {
  Fail "content did not change after the notification (still '$t1')"
}
Write-Host "OK: content changed ('$t1' -> '$t2')"

# --- 4. that second read schedules exactly one more notification ---
$count = Wait-ForCount 2 3
if ($count -ne 2) {
  Fail "expected exactly 2 total notifications after the second read, got $count"
}
# A short settle window: catches a runaway heartbeat immediately firing a
# third event, without paying a full extra second on every run just to
# rule that out.
Start-Sleep -Milliseconds 500
$count = Event-Count
if ($count -ne 2) {
  Fail "expected exactly 2 total notifications after the second read, got $count"
}
Write-Host "OK: exactly one more notification after the second read"

# --- 5. writes from anyone else are rejected ---
$writeSucceeded = $true
try {
  [System.IO.File]::WriteAllText((Join-Path $mnt "time.txt"), "x")
} catch {
  $writeSucceeded = $false
}
if ($writeSucceeded) {
  Fail "external write to time.txt unexpectedly succeeded"
}
Write-Host "OK: external write rejected"

# --- 6. a clean shutdown removes the virtualization root directory itself ---
if (-not (Send-CtrlC $providerProcess)) {
  Fail "could not attach to the provider's console to send Ctrl+C"
}
if (-not $providerProcess.WaitForExit(5000)) {
  Fail "provider did not exit within 5s of Ctrl+C"
}
if (Test-Path $mnt) {
  Fail "virtualization root still exists after a clean shutdown"
}
Write-Host "OK: virtualization root removed after a clean shutdown"

Cleanup
exit 0
