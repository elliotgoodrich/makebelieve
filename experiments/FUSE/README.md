# FUSE Experiment

This experiment sets up a FUSE filesystem with a single file `time.txt`,
which contains a greeting and the current time. It's purpose is to:

  1. Test that we can generate the content of files on-the-fly,
  2. Test that we can trigger inotify by a dummy write by ourselves at the same
     time that the file contents would change (there is no way to know
     about watched files in your FUSE filesystem
     [link](https://github.com/libfuse/libfuse/wiki/Fsnotify-and-FUSE)),
  3. Provide a playground for future experimentation.
