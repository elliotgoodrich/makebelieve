# Tool compatibility

Checks whether common tools read generated outputs correctly.

`build.makebelieve` declares 48 outputs, `00.txt` to `47.txt`, each of which
waits a second and then copies `input.txt`. The scripts mount it, give every
tool invocation an output of its own, and run the invocation on `input.txt`
and on its output before the output has been built.

| Result | Meaning |
| --- | --- |
| Yes | Both runs matched: the tool waited for the build. |
| No | They did not. |
| Not installed | The tool was not found. |

## Running

Build `makebelieve` first, then:

```sh
# Linux, from a native filesystem (FUSE will not mount over /mnt/c in WSL)
experiments/tool_compat/compat.sh [path/to/makebelieve]
```

```powershell
# Windows
experiments\tool_compat\compat.ps1 [-Makebelieve path\to\makebelieve.exe] [-Only regex] [-Detail]
```

Each prints a markdown table. On Windows, `-Only` runs just the tools whose
name matches, and `-Detail` shows what each run produced for rows that are
"No". A run takes about 30 seconds on Linux and a minute on Windows, mostly
the one-second builds.

To add a tool, add a line to the `tests` list in the script and, if there are
more tools than outputs, more outputs to `build.makebelieve`.
