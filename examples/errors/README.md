# Errors example

A [`build.makebelieve`](build.makebelieve) whose builds fail on purpose, one
way each, with an `errors` rule to say why. Reading an output whose build
failed only gives an I/O error, so `errors.txt` is where to look - and since it
is just a file in the mount, anything that can read a file can check it,
without the terminal running the mount.

| Output | Fails because |
| --- | --- |
| `good.txt` | it doesn't: it builds |
| `compiled.txt` | [`compile.cmake`](compile.cmake), standing in for a compiler, rejects line 2 of [`source.txt`](source.txt) and exits 1 |
| `missing-tool.txt` | its command is not installed |
| `forgot-out.txt` | its `run` command succeeds but never writes `%out` |
| `missing-source.txt` | it copies a file that does not exist |
| `downstream.txt` | it reads `compiled.txt`, which fails |

Every command uses `cmake`, so it works under either shell with `cmake` on
`PATH`.

Mount it with `--build=all`, so every output builds straight away rather than
when first read, and read the report:

```sh
$ cd examples/errors
$ makebelieve mount --build=all ../../../errors-out
```

From another terminal, once the builds have run:

```sh
$ cat ../../../errors-out/mount/errors.txt
@/compiled.txt failed (exit 1): capture cmake -DINPUT=source.txt -P compile.cmake
    CMake Error at compile.cmake:9 (message):
      source.txt:2: expected a number, found 'two'
@/downstream.txt failed because @/compiled.txt failed
@/forgot-out.txt failed (command did not write %out): run cmake -E echo "built it, honest"
    built it, honest
    command exited successfully but wrote nothing to %out
@/missing-source.txt failed (could not read the file to copy): copy does-not-exist.txt
    cannot copy [does-not-exist.txt]: The system cannot find the file specified.
@/missing-tool.txt failed (exit 1): capture makebelieve-example-no-such-tool --version
    'makebelieve-example-no-such-tool' is not recognized as an internal or external command,
    operable program or batch file.
```

That is from Windows; on Linux the shell's and the system's own messages
differ - `not found`, `No such file or directory` - and the missing tool
exits 127. Each entry names the output, why its build failed and its command
as the manifest spells it, then indents whatever the command wrote. An output
that failed only because one it read did gets a single line instead.
`good.txt` is not there, as it built.

The terminal running the mount shows the same, each build's under a line
naming its output.

Fixing a failure clears it once the output builds again. A failed build is
retried when a file it read changes, so change `two` to `2` in `source.txt`
and, with `--build=all`, `compiled.txt` and then `downstream.txt` rebuild at
once and leave the report - nothing has to open them. Without `--build=all`,
that happens for outputs that have been opened. Once every failure is fixed
`errors.txt` is empty, which is the check that everything built.

A failure whose fix is outside this folder - installing the missing tool,
say - is not seen, so is only retried when its output is next opened.

Stop with Ctrl+C, or `makebelieve unmount ../../../errors-out`.
