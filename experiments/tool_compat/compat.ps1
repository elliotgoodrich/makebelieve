# Mounts this experiment, runs each tool below against its own unbuilt
# output, and prints a markdown table of the results. See README.md.
#
# Usage: compat.ps1 [-Makebelieve path\to\makebelieve.exe] [-Only regex] [-Detail]
#   -Only    runs just the tools whose name matches the regex
#   -Detail  also shows what each run produced, for rows that are "No"
param(
  [string]$Makebelieve = "$PSScriptRoot\..\..\build\src\Debug\makebelieve.exe",
  [string]$Only = '',
  [switch]$Detail
)
$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Makebelieve)) { throw "makebelieve not found at $Makebelieve" }
$work = Join-Path ([IO.Path]::GetTempPath()) "makebelieve-compat-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$srcdir = "$work\src"; $destdir = "$work\dest"; $mnt = "$work\mnt"
New-Item -ItemType Directory -Force $srcdir, $destdir | Out-Null
Copy-Item "$PSScriptRoot\build.makebelieve", "$PSScriptRoot\input.txt" $srcdir
$src = "$srcdir\input.txt"
$gitusr = if (Get-Command git -ErrorAction SilentlyContinue) {
  Join-Path (Split-Path (Split-Path (Get-Command git).Source)) 'usr\bin'
}
$chrome = "$env:ProgramFiles\Google\Chrome\Application\chrome.exe"
$port = 8765

function Fresh { $d = "$destdir\$([guid]::NewGuid().ToString('N').Substring(0, 8))"; New-Item -ItemType Directory $d | Out-Null; $d }
function Slash([string]$p) { $p -replace '\\', '/' }
function Leaf([string]$p) { Split-Path -Leaf $p }
# `name` (note) as markdown, for a tool spelled "name (note)".
function Name([string]$tool) { if ($tool -match '^(\S+) \((.+)\)$') { "``$($Matches[1])`` ($($Matches[2]))" } else { "``$tool``" } }

# The text Chrome shows for a URL, from its rendered DOM (a text file renders
# as a <pre>).
function ChromeText([string]$url) {
  $out = "$work\chrome-$([guid]::NewGuid().ToString('N')).html"
  Start-Process -FilePath $chrome -Wait -NoNewWindow -RedirectStandardOutput $out `
      -RedirectStandardError "$out.log" -ArgumentList @(
        '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
        "--user-data-dir=`"$work\chrome-profile`"", '--dump-dom', $url)
  $dom = Get-Content -Raw $out
  if ($dom -match '(?s)<pre[^>]*>(.*)</pre>') { $Matches[1] } else { $dom }
}

# Tool, flags tested, the program it needs (if any), and a script block taking
# the file under test that returns output to compare with the source's.
function T($tool, $flags, $needs, $run) { [pscustomobject]@{ Tool = $tool; Flags = $flags; Needs = $needs; Run = $run } }
$tests = @(
  T 'cat' 'none' "$gitusr\cat.exe" { param($p) & "$gitusr\cat.exe" (Slash $p) | Out-String }
  T 'certutil' '`-hashfile FILE MD5`' 'certutil' { param($p) (certutil -hashfile $p MD5)[1] }
  T 'certutil' '`-hashfile FILE SHA256`' 'certutil' { param($p) (certutil -hashfile $p SHA256)[1] }
  T 'chrome' '`file://` URL' $chrome { param($p) ChromeText "file:///$(Slash $p)" }
  T 'chrome' '`http://` URL served by `python -m http.server`' $chrome {
    param($p) if ($p -eq $src) { ChromeText "file:///$(Slash $p)" } else { ChromeText "http://127.0.0.1:$port/$(Leaf $p)" } }
  T 'cmake' '`-E cat`' 'cmake' { param($p) cmake -E cat $p | Out-String }
  T 'cmake' '`-E copy`' 'cmake' { param($p) $d = Fresh; cmake -E copy $p "$d\f"; [IO.File]::ReadAllText("$d\f") }
  T 'cmake' '`-E sha256sum`' 'cmake' { param($p) (cmake -E sha256sum $p).Split(' ')[0] }
  T 'cmake' '`-E compare_files`' 'cmake' { param($p) cmake -E compare_files $src $p; "exit $LASTEXITCODE" }
  T 'cmp' 'none' "$gitusr\cmp.exe" { param($p) & "$gitusr\cmp.exe" (Slash $src) (Slash $p) | Out-Null; "exit $LASTEXITCODE" }
  T 'comp' '`/M`' 'comp' { param($p) $o = cmd /c "comp /M `"$src`" `"$p`" 2>&1" | Out-String; $o -match 'Files compare OK' }
  T 'copy (cmd)' '`/y`' $null { param($p) $d = Fresh; cmd /c "copy /y `"$p`" `"$d\f`"" | Out-Null; [IO.File]::ReadAllText("$d\f") }
  T 'Copy-Item' 'none' $null { param($p) $d = Fresh; Copy-Item $p "$d\f"; [IO.File]::ReadAllText("$d\f") }
  T 'curl' '`file://` URL' 'curl.exe' { param($p) curl.exe -s "file:///$(Slash $p)" | Out-String }
  T 'diff' 'none' "$gitusr\diff.exe" { param($p) & "$gitusr\diff.exe" (Slash $src) (Slash $p) | Out-Null; "exit $LASTEXITCODE" }
  T 'diff' '`-q`' "$gitusr\diff.exe" { param($p) & "$gitusr\diff.exe" -q (Slash $src) (Slash $p) | Out-Null; "exit $LASTEXITCODE" }
  T 'fc' 'none' 'fc.exe' { param($p) fc.exe $src $p 2>&1 | Out-Null; "exit $LASTEXITCODE" }
  T 'fc' '`/b`' 'fc.exe' { param($p) fc.exe /b $src $p 2>&1 | Out-Null; "exit $LASTEXITCODE" }
  T 'find (cmd)' '`/c`' $null { param($p) (cmd /c "find /c `"fox`" `"$p`"" | Out-String).Split(':')[-1].Trim() }
  T 'findstr' '`/c:`' 'findstr' { param($p) findstr /c:"line 050" $p | Out-String }
  T 'Get-Content' 'none' $null { param($p) Get-Content $p | Out-String }
  T 'Get-Content' '`-Raw`' $null { param($p) Get-Content -Raw $p }
  T 'Get-FileHash' '`-Algorithm SHA256`' $null { param($p) (Get-FileHash $p -Algorithm SHA256).Hash }
  T 'Get-Item' '`.Length`' $null { param($p) (Get-Item $p).Length }
  T 'git' '`hash-object --no-filters`' 'git' { param($p) git hash-object --no-filters $p }
  T 'md5sum' 'none' "$gitusr\md5sum.exe" { param($p) (& "$gitusr\md5sum.exe" (Slash $p)).Split(' ')[0] }
  T 'more (cmd)' 'file on standard input' $null { param($p) cmd /c "more < `"$p`"" | Out-String }
  T 'node' '`fs.readFileSync`' 'node' { param($p) node -e "console.log(require('fs').readFileSync(process.argv[1]).subarray(-40).toString())" $p }
  T 'node' '`fs.createReadStream`' 'node' { param($p) node -e "let n=0; require('fs').createReadStream(process.argv[1]).on('data',c=>n+=c.length).on('end',()=>console.log(n))" $p }
  T 'python' '`open().read()`' 'python' { param($p) python -c "import sys; print(open(sys.argv[1], 'rb').read()[-40:])" $p }
  T 'python' '`mmap` (read-only)' 'python' { param($p) python -c "import mmap, sys; f = open(sys.argv[1], 'rb'); print(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)[-40:])" $p }
  T 'robocopy' '`/R:0 /W:0`' 'robocopy' { param($p) $d = Fresh; robocopy (Split-Path $p) $d (Leaf $p) /R:0 /W:0 | Out-Null; if (Test-Path "$d\$(Leaf $p)") { [IO.File]::ReadAllText("$d\$(Leaf $p)") } }
  T 'robocopy' '`/R:1 /W:1`' 'robocopy' { param($p) $d = Fresh; robocopy (Split-Path $p) $d (Leaf $p) /R:1 /W:1 | Out-Null; if (Test-Path "$d\$(Leaf $p)") { [IO.File]::ReadAllText("$d\$(Leaf $p)") } }
  T 'Select-String' '`-Pattern`' $null { param($p) (Select-String -Path $p -Pattern 'fox' | Measure-Object).Count }
  T 'System.IO.File (.NET)' '`ReadAllBytes`' $null { param($p) [IO.File]::ReadAllBytes($p).Length }
  T 'System.IO.File (.NET)' '`Copy`' $null { param($p) $d = Fresh; [IO.File]::Copy($p, "$d\f"); [IO.File]::ReadAllText("$d\f") }
  T 'System.IO.File (.NET)' '`OpenRead().Length`' $null { param($p) $s = [IO.File]::OpenRead($p); try { $s.Length } finally { $s.Dispose() } }
  T 'tail' '`-c 100`' "$gitusr\tail.exe" { param($p) & "$gitusr\tail.exe" -c 100 (Slash $p) | Out-String }
  T 'tar' '`-cf`, then `-xf`' 'tar.exe' { param($p) $d = Fresh; tar -cf "$d\a.tar" -C (Split-Path $p) (Leaf $p) 2>&1 | Out-Null; tar -xf "$d\a.tar" -C $d 2>&1 | Out-Null; if (Test-Path "$d\$(Leaf $p)") { [IO.File]::ReadAllText("$d\$(Leaf $p)") } }
  T 'type (cmd)' 'none' $null { param($p) cmd /c "type `"$p`"" | Out-String }
  T 'wc' '`-c -l`' "$gitusr\wc.exe" { param($p) (& "$gitusr\wc.exe" -c -l (Slash $p)) -replace '\s+\S+$', '' }
  T 'xcopy' '`/y /q`' 'xcopy' { param($p) $d = Fresh; xcopy /y /q $p "$d\" | Out-Null; [IO.File]::ReadAllText("$d\$(Leaf $p)") }
)

$daemon = Start-Process -FilePath $Makebelieve -ArgumentList 'mount', $mnt `
    -WorkingDirectory $srcdir -WindowStyle Hidden -PassThru
$server = $null
try {
  foreach ($i in 1..100) { if (Test-Path "$mnt\00.txt") { break }; Start-Sleep -Milliseconds 50 }
  if (-not (Test-Path "$mnt\00.txt")) { throw 'makebelieve did not mount' }
  if (Get-Command python -ErrorAction SilentlyContinue) {
    $server = Start-Process -FilePath python -PassThru -WindowStyle Hidden `
        -ArgumentList '-m', 'http.server', $port, '--bind', '127.0.0.1', '--directory', $mnt
    Start-Sleep -Seconds 2
  }

  '| Tool | Flags tested | Compatible |'
  '| --- | --- | --- |'
  $i = 0
  foreach ($t in $tests) {
    if ($Only -and $t.Tool -notmatch $Only) { continue }
    if ($t.Needs -and -not (Get-Command $t.Needs -ErrorAction SilentlyContinue)) {
      "| $(Name $t.Tool) | $($t.Flags) | Not installed |"
      continue
    }
    $output = '{0}\{1:D2}.txt' -f $mnt, $i
    $i++
    $expected = (& $t.Run $src 2>&1 | Out-String).Trim()
    $actual = (& $t.Run $output 2>&1 | Out-String).Trim()
    $result = if ($actual -ceq $expected) { 'Yes' } else { 'No' }
    "| $(Name $t.Tool) | $($t.Flags) | $result |"
    if ($Detail -and $result -ne 'Yes') {
      foreach ($run in @(@('expected', $expected), @('actual', $actual))) {
        $text = $run[1] -replace "`r?`n", '|'
        "    $($run[0]) ($($run[1].Length) chars): $($text.Substring(0, [math]::Min(100, $text.Length)))"
      }
    }
  }
} finally {
  if ($server) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
  & $Makebelieve unmount $mnt | Out-Null
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
