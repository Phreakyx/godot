# CPU sampling profiles and family shares

Kernel tracing (`xperf`, `wpr`) needs an elevated shell. `sampler.exe`
needs none: it opens a process of the same user, suspends each thread that
used CPU since the last tick, walks its stack, resumes it, and at the end
resolves the addresses with the PDB, inlined frames included. The innermost
inline frame is what matters for header-only code such as `HashMap`, which
is mostly inlined into its callers.

## Windows: `bench.py profile`

The runner profiles any of its workloads on the existing bench control
build (`devtools\build\build.ps1 -Bench`, plus `-Tests` for `micro_engine`;
bench builds have `debug_symbols=yes`, so the PDB is next to the exe).
`profile` never builds. It checks the machine like a series, builds the
sampler on first use (`build_sampler.ps1`, into `devtools/out/profile/`),
starts the real exe (not the `.console.exe` wrapper) pinned to P-cores 1-7,
attaches the sampler to that PID, ends the exe if it still runs (its own
child only), and runs the analysis:

```powershell
python devtools\bench\bench.py profile --workload scene:instance:10000 --build template_release
python devtools\bench\bench.py profile --workload script:script --seconds 15 --warmup 3
python devtools\bench\bench.py profile --workload editor_startup
```

Options: `--workload`, `--build editor|template_release` (default
`editor`), `--seconds` (15), `--warmup` (3), `--interval-us` (1000),
`--max-load`, `--out` (default `devtools/out/bench/profile/<stamp>-profile/`).
Frame workloads (`scene`, `script`, `level`) run 1,000,000 frames and are
sampled after `--warmup` for `--seconds`; `editor_startup` (`--headless
--editor --quit-after 3000` on a copy of the test project), `gdbench` and
`micro_engine` are sampled from launch until they exit. Each profile
writes `<workload>.<build>.tsv` (`:` and `/` become `_`), `.log` (the
engine's output), `.summary.txt` (`analyze.py summary`), `.families.json`
(`analyze.py families --json`), and an entry in the directory's
`meta.json` (exe, command, sampler counts, machine state).

## Reading a profile

```powershell
python devtools\profile\analyze.py <profile.tsv> summary
python devtools\profile\analyze.py <profile.tsv> callers "^(HashMap|HashSet|AHashMap)::"
python devtools\profile\analyze.py <profile.tsv> callers "AcquireSRW|ReleaseSRW|mtx_do_lock" --skip core/os
python devtools\profile\analyze.py <profile.tsv> families --json shares.json
python devtools\profile\test_analyze.py
```

`families` attributes each busy sample by its innermost frame to the
first family that matches (`FAMILIES` in `analyze.py`) and prints each
family's share of the busiest thread and of all busy samples:

| Family | Innermost frame |
| --- | --- |
| bench counter (excluded) | `core/os/fork_alloc_counters*`: bench builds only, no engine family |
| formatting | `String::sprintf`, `String::num*`, `vformat`, grisu2 (by name, wherever they live) |
| allocation | `core/os/memory.cpp`; CRT, libc and ntdll heap functions (`malloc`, `free`, `realloc`, `RtlAllocateHeap`, ...) |
| hash tables | `core/templates/` `hash_map.h`, `hash_set.h`, `a_hash_map.h`, `oa_hash_map.h`, `hashfuncs.*` |
| Variant containers | `core/variant/dictionary.cpp`, `array.cpp`, `core/string/string_name.*`, `Variant::hash` |
| trees/lists | `core/templates/rb_map.h`, `rb_set.h`, `list.h` |
| CowData | `core/string/ustring.*`, `core/templates/cowdata.h`, `vector.h` |
| other | everything else (refcounts, locks, scene code, GDScript, ...) |

A `HashMap` lookup inside `Dictionary` counts as hash tables: the innermost
frame decides. Waits are idle and in no family: the ntdll and win32u wait,
sleep and message functions, and on Linux futex, `epoll_wait`, `poll`,
`nanosleep`, `clock_nanosleep` and condition waits in libc, libpthread and
the kernel.

## Linux: perf

`perf record` samples the engine process (attach with `-p <pid>` or run
the workload under it), then `perf_to_tsv.py` writes the same TSV format,
so `analyze.py` computes the shares with the same rules:

```bash
perf record -F 997 -g -p <pid> -- sleep 15        # or: perf record -F 997 -g -- <engine command>
perf script -F tid,comm,ip,sym,dso,srcline --inline --full-source-path | python devtools/profile/perf_to_tsv.py > x.tsv
python devtools/profile/analyze.py x.tsv families
```

`ip` is needed for perf to print symbols and the call graph,
`--full-source-path` for the repository paths the families match. The
converter is checked on a fixture of the documented format; real CachyOS
output is checked when the Linux profiles are taken.

## Notes

`profile.ps1` (the old driver, which found the engine process under the
console wrapper) is replaced by `bench.py profile` and goes away with the
other old drivers. `core_workload.gd` (a `SceneTree` script, vsync off,
time-based) has three modes: `nodes`, `script` and `instance`; the runner's
`script:<mode>` workload is its frame-based successor.

Limits: about 0.5-1 kHz per thread; stack walks slow the sampling when a
process has many threads (the editor: about 100 Hz). Waits are recognized by
the system function's name (`analyze.py`), not by the scheduler. Editor
binaries have `DEBUG_ENABLED` allocation accounting that release templates
do not. Findings: [kb/project/core-cpu-profiles.md](../../kb/project/core-cpu-profiles.md).
