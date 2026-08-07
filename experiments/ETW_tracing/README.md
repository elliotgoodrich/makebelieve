# ETW Tracing Experiment

`etw_tracing_experiment <command> [args...]` runs `<command>` under an NT
Kernel Logger ETW trace, and prints every file under the current directory
that the command - or a descendant process it spawns - opened for reading.
It's the ETW counterpart to [`../FUSE_tracing`](../FUSE_tracing), and aims
for the same report, by a structurally different mechanism: FUSE_tracing
is a virtual filesystem that *is* the boundary it traces; this one is a
passive, system-wide observer with no boundary of its own, both re-created
by hand here (see below).

**Requires running elevated** (Administrator, or a member of the
"Performance Log Users" group) - consuming NT Kernel Logger events is
privileged regardless of what the traced command itself needs.

## How it works

1. Starts the NT Kernel Logger - Windows' single, system-wide kernel
   event-tracing session - with file-I/O and process-lifecycle events
   enabled.
2. Launches `<command>` suspended, registers its PID with the tracer, then
   resumes it - closing the race where an early file access could arrive
   before the tracer recognizes the PID that made it.
3. A background thread drains the trace's `FileIo_Create` events (logged
   at open time for every file *and directory* opened, system-wide, by
   every process) and `Process` start events, in real time.
4. Every `FileIo_Create` event is kept only if its process ID is in a
   traced-PID set - seeded with the launched command's PID and grown by
   the `Process` start events, so descendants are covered too - and its
   path falls under the current directory.
5. Once `<command>` exits, the trace is stopped, and the collected
   candidates are checked once more against the real filesystem (dropping
   anything that's now a directory or no longer exists) before the
   deduplicated, sorted report is printed.

## Why this one is structurally different

FUSE_tracing is a virtual filesystem: the mount *is* the boundary, so
"which process" and "which paths" are answered by construction - nothing
outside the mount, or outside whichever process has it open, is ever
seen. ETW has no such boundary; it observes the whole system. This tracer
reconstructs both boundaries by hand (process attribution via the
traced-PID set, path filtering via string comparison against the current
directory), which is where its two real weaknesses next to FUSE_tracing
come from:

- **Lossy.** ETW buffers are finite and real-time delivery can drop
  events under load - the NT Kernel Logger doesn't guarantee every access
  is seen the way a virtual filesystem's callback contract does. This
  tracer can under-report; it should never over-report, since every kept
  path both matched a traced PID and passed the current-directory check.
- **NT-native paths.** `FileIo_Create`'s `OpenPath` is reported in
  `\Device\HarddiskVolumeN\...` form, not `C:\...` - this tracer resolves
  it back via a `QueryDosDeviceW`-built prefix map. A path that fails to
  resolve (a UNC path, an unrecognized device) is silently dropped rather
  than reported wrong.

Both are consistent with treating this as a "trust the report, then
verify by re-`stat`ing before trusting a cached result" dependency
signal - see the project's own design discussion for why that's an
acceptable trade-off for ETW specifically, even though it wouldn't be for
a tracer that needs to synchronously gate opens (which none of these
three experiments do).
