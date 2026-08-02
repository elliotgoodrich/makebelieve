# ProjFS Experiment

This experiment sets up a ProjFS (Windows Projected File System)
virtualization instance with a single file `time.txt`, which contains a
greeting and the current time. It's the ProjFS counterpart to
[`../FUSE`](../FUSE) and is intended to behave identically:

  1. Test that we can generate the content of files on-the-fly.
  2. Test that we can trigger a Windows file-change notification
     (observable via `ReadDirectoryChangesW`, e.g. from Explorer or any
     watcher) at the same time the file's contents would change.
  3. Provide a playground for future experimentation.
