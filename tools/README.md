# Tools

Analysis tools used for the reverse engineering and the diagnostics. They are
here to replay that work, not to run on every build.

| File | Role |
|---|---|
| `ghidra/Dump.java` | Ghidra script: function dump / decompilation from a command file |
| `crash_dump.py` | Windows minidump (MDMP) parser: modules, threads, stacks, exception, memory, FunctionTable |
| `match_clear.py` | carries the `ComponentRegistry::Clear` signature from 1.15.18 over to 1.15.23 and extracts the mask |
| `gen_grow.py` | same method for `RingBuffer::Grow` |

## Missing scripts

The names `find_catalogue.py`, `gen_sigs*.py`, `compare_builds.py`,
`read_const.py`, `match_funcs.py`, `match_helpers.py` and `binparse*.py` show up
in some work plans, but these files **do not exist**. The scripts actually
present and related to the jet are the ones listed above.

## Dependencies

The Python scripts only use the standard library (`struct`, `os`, `sys`) and
assume the paths of the reference executables:

```text
H:/Jeux/StormworksV1.15.23/Stormworks/stormworks64.exe
H:/Jeux/Stormworks/stormworks64.exe
```

Change these paths in the scripts if your install is somewhere else.
