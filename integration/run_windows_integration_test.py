"""Exercise real ProjFS reads of a configured multi-byte placeholder.

The shared integration test in run_integration_test.cmake covers the default
one-byte placeholder and change notifications. This one covers the placeholder
feature: a placeholder larger than a byte must be readable in full, must hold
its configured bytes until the build lands, and must not itself trigger a build
for an output nobody read.
"""

from pathlib import Path
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time


executable = Path(sys.argv[1]).resolve()
workspace = Path(sys.argv[2]).resolve()
workspace.mkdir(parents=True, exist_ok=True)
root = Path(tempfile.mkdtemp(prefix="projfs-integration-", dir=workspace)).resolve()
assert root.parent == workspace
source = root / "source"
output = root / "output"
source.mkdir()
initial = b"first build: " + b"x" * 4096
placeholder = b"<!doctype html><html><body>Building</body></html>"
(source / "loading.html").write_bytes(placeholder)
(source / "input.txt").write_bytes(initial)
(source / "build.makebelieve").write_text(
    "[*.html]\n    %placeholder = loading.html\n"
    "@/page.html <- cmake -E sleep 0.5 && cmake -E copy input.txt %out\n"
    "@/unread.html <- cmake -E copy input.txt %out\n"
)
process = None


def wait_for(check):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError("MakeBelieve exited: " + process.stderr.read().decode())
        if check():
            return
        time.sleep(0.1)
    raise AssertionError("Timed out waiting for the projected output")


def read_page():
    # A separate reader makes a stuck kernel read fail within a bounded time.
    # Before the callback fix, this hangs or repeatedly returns a single byte.
    return subprocess.check_output(
        [sys.executable, "-c",
         "import pathlib,sys; sys.stdout.buffer.write(pathlib.Path(sys.argv[1]).read_bytes())",
         str(output / "page.html")],
        timeout=5,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )


def remove_readonly(function, path, error):
    # Positional, so this works as both rmtree's onexc= and its onerror=.
    os.chmod(path, stat.S_IWRITE)
    function(path)


def stop_daemon():
    """Unmount cleanly: a hard kill orphans the ProjFS placeholders, which then
    resist the cleanup below and the next run's."""
    if process.poll() is None:
        subprocess.run([str(executable), "unmount", str(output)],
                       timeout=30, capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    process.stderr.close()


try:
    process = subprocess.Popen(
        [str(executable), "mount", str(output)], cwd=source,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    wait_for(lambda: (output / "page.html").exists())
    assert (output / "page.html").stat().st_size == len(placeholder)
    assert read_page() == placeholder
    wait_for(lambda: read_page() == initial)
    assert (output / "unread.html").stat().st_size == len(placeholder)
    changed = b"second build: " + b"y" * 8192
    (source / "input.txt").write_bytes(changed)
    wait_for(lambda: read_page() == changed)
    assert (output / "unread.html").stat().st_size == len(placeholder)
    print("PASS: HTML placeholder is readable, full output arrives, source edit rebuilds, unread output stays lazy")
finally:
    if process is not None:
        stop_daemon()
    # ProjFS outputs are read-only. This is exclusively the test-owned tree.
    assert root.parent == workspace
    # rmtree's onerror= is deprecated from 3.12, and onexc= exists only there.
    if sys.version_info >= (3, 12):
        shutil.rmtree(root, onexc=remove_readonly)
    else:
        shutil.rmtree(root, onerror=remove_readonly)
