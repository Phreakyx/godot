---
type: Playbook
title: Benchmark protocol
description: How a core change is measured and judged - A/A floors first, three binaries, alternating reversed rounds, the accept, regression and skip rules, the machine checks and what every run records.
tags: [benchmark, testing, measuring, protocol, performance]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-20T00:00:00Z }
sources:
  - id: bench-py
    resource: ../../devtools/bench/bench.py
    title: The runner (statistics, verdicts, series, floors, metadata)
  - id: bench-windows
    resource: ../../devtools/bench/bench_windows.py
    title: Windows adapter (P-core mask, pinned launch, OS counters, machine checks, build)
  - id: bench-linux
    resource: ../../devtools/bench/bench_linux.py
    title: Linux adapter (P-cores by PMU, wait4, sysfs checks, scons builds, perf counters and profiles)
  - id: bench-readme
    resource: ../../devtools/bench/README.md
    title: How to run the runner
  - id: bench-workloads
    resource: ../../devtools/bench/workloads/
    title: Workload drivers (scene.gd, script.gd, gdbench.gd, gdbench_dictionary.gd, real_level.gd)
  - id: godot-benchmarks
    resource: https://github.com/godotengine/godot-benchmarks/tree/ff8e67af1eac6054f011ee7137163b64bd0bf7fb
    title: godot-benchmarks at the pinned commit (MIT; fetched, never vendored)
  - id: tps-demo
    resource: https://github.com/godotengine/tps-demo/tree/a82f15448e9b015440d3bbdf5e10801b260c4e9f
    title: Godot TPS demo at the pinned commit (MIT code, CC-BY 3.0 assets; fetched, never vendored)
  - id: containers-bench
    resource: ../../devtools/bench/micro/containers_bench.cpp
    title: The container micro-benchmark harness (bench.py micro)
  - id: micro-engine-cases
    resource: ../../tests/fork/test_fork_bench_variant_keys.cpp
    title: The in-engine StringName and Variant-key cases (workload micro_engine)
  - id: profile
    resource: ../../devtools/profile/README.md
    title: Sampling profiles (bench.py profile, sampler.exe, perf) and analyze.py families
  - id: unordered-dense
    resource: https://github.com/martinus/unordered_dense/tree/7c58a8b7a48e790fe72f6508eb3b46317cbba9d5
    title: ankerl::unordered_dense v5.3.3 (MIT; micro control, fetched, never vendored)
  - id: gtl
    resource: https://github.com/greg7mdp/gtl/tree/b9a2586edd9f052b6d071c751fe188b0e71bd2ee
    title: gtl v1.3.1 (Apache-2.0; micro control, fetched, never vendored)
  - id: emhash
    resource: https://github.com/ktprime/emhash/tree/e84fa51009e5040a82801290313b22976bfa56de
    title: emhash v1.3.0 (MIT; micro control, fetched, never vendored)
  - id: boost
    resource: https://github.com/boostorg/boost/releases/tag/boost-1.92.0
    title: Boost 1.92.0 b2-nodocs archive (BSL-1.0; micro control, fetched, never vendored)
---

# Ground rules

These rules are written before any floor or share is measured, and the
runner prints the accept and regression rules before its first round.

- **A/A first.** Before a comparison is judged, the noise floor of every
  workload is measured per compiler and per build type (editor,
  `template_release`) with the same round count n the comparison uses: the
  floor is the largest |paired difference| between two builds of the same
  source with their own objects (`bench.py aa`), and it grows with n.
- **Re-measure the floors** after a compiler, BIOS or microcode update, a
  major OS update (a new OS build number; the monthly update revision alone
  does not count) or any change to the machine setup. The runner stores the
  compiler version, BIOS, microcode and OS build with each floor and prints
  `RE-MEASURE-AA` instead of a verdict when one of them, or n, differs.
- **Three binaries** per comparison, each with its own objects:
  the previous build (the commit before the candidate's first commit, or
  `origin/dev` while the candidate is uncommitted, built in a git worktree),
  the candidate (the same source plus its `FORK_BACKEND_*` defines) and the
  same-source control. The verdict compares the candidate with the control
  only: the previous build shows how much code layout moved, and a layout
  shift can't pass as a gain (see
  [control build for layout effects](/successes/control-build-for-layout-effects.md)).
- **One discarded cold round** per series: every new exe runs once before
  the measured rounds (driver and file caches are per exe,
  [warm runs on a new exe](/mistakes/warm-runs-on-a-new-exe.md)).
- **Alternating rounds:** round 1 runs `[previous, candidate, control]`
  (A/A: `[control, aa]`), every even round runs the reversed order, so slow
  drift (heat, background work) hits every binary the same way
  ([separate runs had an order bias](/mistakes/separate-runs-order-bias.md)).
- **A failed sample is never a number.** A timeout, a script error, an
  `ERROR` line, a non-zero exit or a missing `BENCH` line marks the sample
  failed with its reason; the round is dropped from the paired differences
  on both sides, and fewer than 5 pairs is `INCOMPLETE`.

# Rules stated before measuring

Paired difference per round: `(candidate - control) / control`.

**Accept (target workload, D-05).** Both must hold against the control:
the median paired difference is faster than the workload's A/A floor, and
the candidate wins at least k of n rounds, k from the one-sided sign test
(chance alone passes below 5%). A tie counts as a loss.

| n | k | p |
| --- | --- | --- |
| 5 | 5 | 0.031 |
| 6 | 6 | 0.016 |
| 7 | 7 | 0.008 |
| 8 | 7 | 0.035 |
| 9 | 8 | 0.020 |
| 10 | 9 | 0.011 |
| 11 | 9 | 0.033 |
| 12 | 10 | 0.019 |
| 13 | 10 | 0.046 |

n = 10 is no better than n = 11 (both need 9 wins).

**Regression (every other workload, a target that is not faster, and peak
memory everywhere, D-06).** Time or OS peak memory regresses when the median paired difference is
slower or bigger than the floor and the candidate loses at least k of n
rounds. Allocation counts are deterministic once the A/A run shows two
same-source builds give identical counts: then **any allocation-count
increase is flagged and must be explained** in the `kb/` entry, or the
candidate is rejected. A workload whose A/A counts differ is reported as
not deterministic, with its spread. The counts come from the
[allocation counter](/project/allocation-counter.md) every `-Bench` build
carries (`allocs=`, `reallocs=` on the BENCH line); `aa` prints
`COUNTS ... identical` or `COUNT-NONDETERMINISTIC (min..max)`, `compare`
prints `ALLOC-INCREASE` or `NO-INCREASE`, and a binary without the counter
(a previous build from before it) reads `n/a`.

**Skip (D-07).** A family's max gain is `share x (1 - 1/speedup)`, with the
measured share and the best plausible speedup of its internals. The family
is skipped only when the max gain is strictly below the target workload's
floor; "rejected, within noise" after the work is a valid outcome. The
largest share across editor and `template_release` and across MSVC, GCC and
Clang decides build or skip; acceptance must still hold on every one of
them.

**Micro-benchmarks never decide acceptance alone.** Their third-party
control rows are compiled identically into every side, so a control row
that moves shows the run measured code layout, not the change.

The verdict tokens: `ACCEPT`, `REJECT`, `WITHIN-NOISE`, `REGRESSION`,
`INCOMPLETE`, `NO-FLOOR` (no A/A floor stored), `RE-MEASURE-AA` (the floor
is from another machine state or n).

# Machine

- Intel i9-14900K (8 P-cores with HT, 16 E-cores, 32 logical CPUs),
  Windows 11 IoT Enterprise LTSC 2024, CachyOS on the same PC.
- **Pinning (D-11, D-12):** every measured process runs on P-cores 1-7
  with both HT threads (14 logical CPUs, mask `0xfffc` here), found at run
  time by efficiency class, never by hard-coded CPU numbers. P-core 0 and
  the E-cores are left to the OS, and the runner moves itself and its
  helpers (typeperf, PowerShell, git) there, so they never sit on a
  measured CPU. The child's mask is read back and checked.
- **Worker threads:** the bench project caps Godot's pool at 14
  (`[threading] worker_pool/max_threads`); the runner runs each game-mode
  binary once with `--verbose` in the cold round and refuses unless it
  reports `WorkerThreadPool: 14 threads`. The **editor worker exception:**
  editor-mode runs always start 32 workers (`init(-1, 0.75)` ignores the
  project and the affinity); the count is recorded with each run, the same
  for A and B, and revisited only if the editor-startup floor comes out
  large.
- **Checks that refuse (D-13).** System settings are read, never changed;
  a refusal prints the reason or the fix and leaves the PC as it was:
  - Power plan "Bitsum Highest Performance", matched by GUID
    `fe1217fb-3f66-46bf-adc6-e48c27825f80`, with core parking off
    (`CPMINCORES` and `CPMINCORES1` at 100 on AC and DC). The fix:

    ```
    powercfg /setacvalueindex fe1217fb-3f66-46bf-adc6-e48c27825f80 SUB_PROCESSOR CPMINCORES 100
    powercfg /setdcvalueindex fe1217fb-3f66-46bf-adc6-e48c27825f80 SUB_PROCESSOR CPMINCORES 100
    powercfg /setacvalueindex fe1217fb-3f66-46bf-adc6-e48c27825f80 SUB_PROCESSOR CPMINCORES1 100
    powercfg /setdcvalueindex fe1217fb-3f66-46bf-adc6-e48c27825f80 SUB_PROCESSOR CPMINCORES1 100
    powercfg /setactive fe1217fb-3f66-46bf-adc6-e48c27825f80
    ```
  - No compiler or SCons running (checked again before every round).
  - The bench CPUs less than 10% busy (mean `% Processor Utility` over
    3 s); above it the runner names the load and asks to close what runs
    there. `--max-load` raises the limit for smoke runs only; floors and
    shares are measured at the default.
  - One runner at a time (an OS file lock on `devtools/out/bench/.lock`,
    released when the process ends, so a crash leaves no stale lock).
  - Game Mode is read and recorded, not judged.
- **Boost on, no BIOS changes (D-14).** Each round records the effective
  clock of the bench CPUs (`% Processor Performance` x `Processor
  Frequency`, sampled once a second by `typeperf`, so a round shorter than
  a second gets the sample that ends after it); rounds more than 5% off the
  series median are flagged in `raw.csv`. The package temperature can't be read on Windows without a
  kernel driver and is recorded as unavailable.
- **Windows first, then one Linux session (D-15):** the Windows/MSVC
  floors and shares come first; one CachyOS session then measures the GCC
  and Clang floors and shares (compilers held with `IgnorePkg`). No
  candidate is accepted without the Linux numbers. The Linux checks and
  their fixes are in "Linux" below.

# Workloads

Every workload runs a fixed number of frames or calls, never a time, so
operation and allocation counts are the same for fast and slow builds.
Peak memory (`peak_ws_mb`) is the OS peak working set of the process.
Cheap workloads (a process under 3 s) get n = 11; costly ones n = 5.

| Workload id | What it runs | Judged | Counts | Class | Builds |
| --- | --- | --- | --- | --- | --- |
| `scene:<scenario>:<count>` | `devtools/bench/workloads/scene.gd`: node scenarios (instancing, churn, transforms), 120 frames after 30 warm-up | `frame_ms_p50`, `peak_ws_mb` | `allocs`, `reallocs` | by the cold round | both |
| `script:<nodes\|script\|instance>` | `devtools/bench/workloads/script.gd`: the three modes of the old profile workload script (10,000 nodes with 200 freed and re-added per frame and a group call; 2,000 agents with Dictionary stats, `%` formatting and String joins; a 2,000-node scene instanced and freed per frame), 120 frames after 30 warm-up | `frame_ms_p50`, `peak_ws_mb` | `allocs`, `reallocs` | by the cold round | both |
| `gdbench` | `devtools/bench/workloads/gdbench.gd`: a godot-benchmarks subset and the fork's Dictionary benchmark, one process, every case once to warm up and once timed | `time_ms` per case, `peak_ws_mb` per process | `allocs`, `reallocs` per case | costly | both |
| `editor_startup` | headless `--editor --quit` on a copy of `misc/large_world_test_project`, with `--benchmark-file` | `wall_ms` (process creation to exit), `peak_ws_mb` | none | costly | editor only |
| `level` | `devtools/bench/workloads/real_level.gd` on the Godot TPS demo `.pck`: load, instancing, 32 extra robots, 600 frames of a camera path after 120 warm-up, windowed on Vulkan | `load_ms`, `instance_ms`, `main_ms_p50`, `render_cpu_ms_p50`, `peak_ws_mb` | `load_allocs`, `fly_allocs`, `fly_reallocs` | costly | both |

- **Scene project.** `scene`, `script` and `gdbench` run headless in
  `misc/scene_bench_project` with `--fixed-fps 60` (14 workers).
- **godot-benchmarks subset.** `bench.py fetch` (and every `gdbench`
  series) fetches [godotengine/godot-benchmarks](https://github.com/godotengine/godot-benchmarks)
  at commit `ff8e67af1e` (`ff8e67af1eac6054f011ee7137163b64bd0bf7fb`,
  MIT) into `devtools/out/bench/src/godot-benchmarks/ff8e67a/` and refuses
  to use it unless `git rev-parse HEAD` is that commit. It is never
  vendored: each series copies `benchmarks/gdscript/string_format.gd`,
  `string_manipulation.gd`, `array.gd`, `alloc.gd` and
  `benchmarks/core/string_name.gd`, `signal.gd` into
  `devtools/out/bench/gdbench/ff8e67a/`, changing only `extends Benchmark`
  to `extends RefCounted` and, in the first four, the `ITERATIONS` constant
  (to 100,000, 100,000, 200,000 and 25,000; upstream's values made one
  process take 55 s). `meta.json` lists each file with its changed-line
  count (1 or 2) and both values; the String cases keep upstream's text.
  The driver seeds the global RNG before every call (`alloc.gd` uses
  `randi()`).
- **Dictionary benchmark.** `devtools/bench/workloads/gdbench_dictionary.gd`
  is the fork's own (upstream has none at the pin): insert, lookup hit,
  lookup miss, erase (of a copy), iterate and duplicate with String,
  StringName, int and Vector2 keys at 1,000 and 100,000 entries, 100,000
  entry operations per case, fixed keys.
- **Cases.** Files run in file name order and methods in name order; each
  method is its own item named `gdbench/<file stem>.<method>`, so methods
  with the same name in two files stay apart (`FLOOR gdbench/string_format.benchmark_x time_ms editor: ...`).
  A file with no `benchmark_` methods, or that does not load, prints
  `BENCH_FAIL` and fails the whole sample; a script error fails it too. A
  case the cold round printed and a later sample lacks is failed ("case
  missing"), never left out; a cold round with no cases stops the series.
- **Editor startup.** Each series mirrors the tracked files of
  `misc/large_world_test_project` into
  `devtools/out/bench/<series>/editor_project/<binary>/` (deleted and copied
  again), so the tracked project is never opened by the editor. The cold
  round is the import of each copy by its own binary. `meta.json` records
  `editor_workers` (32, the D-12 exception). `main_start_ms`
  (`[Startup] Main::Start`) and `first_scan_ms` (`[Editor] First Scan`)
  from the JSON go to `raw.csv` and are printed as `INFO` medians, never
  judged. On `template_release` the floor and verdict lines read
  `n/a (editor only)`.
- **Real level (D-08 to D-10).** The Godot TPS demo
  ([godotengine/tps-demo](https://github.com/godotengine/tps-demo), MIT
  code, CC-BY 3.0 assets) at commit `a82f15448e`
  (`a82f15448e9b015440d3bbdf5e10801b260c4e9f`): real game code (robot AI,
  AnimationTree, physics, particles) in a large lit level. `bench.py fetch`
  (and every `level` series) fetches it into
  `devtools/out/bench/src/tps-demo/a82f154/` and refuses it unless HEAD is
  the pin; nothing of it is vendored. Once per series the control editor
  bench build (`godot.windows.editor.x86_64.bench.exe`) runs
  `--headless --path <copy> --import`, writes a minimal `export_presets.cfg`
  (Windows Desktop, all resources; the demo ships none and ignores the
  file) into the copy and runs `--export-pack bench <series>/tps-demo.pck`;
  no export templates are needed for a pack. Every binary of the series, on
  editor and `template_release`, runs that one `.pck` with `--main-pack`.
  `meta.json` `level` records its SHA-256, its size and the number of
  files the editor changed in the copy (`git status --porcelain`).
  `override.cfg` next to the `.pck` pins what the project leaves open
  without editing it: 14 workers, vsync off, `max_fps` 0, windowed
  1920x1080, no HDR output, Vulkan, and `GodotPhysics3D` (the project names
  "Bullet", a Godot 3 engine). The command adds `--fixed-fps 60
  --rendering-driver vulkan --resolution 1920x1080 --audio-driver Dummy`.
- **What the level driver pins.** Every option of the demo's `Settings`
  autoload is set to the demo's defaults before the level loads (a
  `user://settings.ini` from playing the demo is ignored), then windowed,
  no vsync, no fps cap, VoxelGI at low quality. After `level.gd` and the
  forklifts call `randomize()` in `_ready`, the driver seeds the RNG, puts
  the player on spawn point 1 and shows forklift model 1. The player gets
  no input (its `InputSynchronizer` stops; the mouse is not captured) and
  the window takes no input events. 32 extra robots (`--robots`, in
  `meta.json`) are spawned through the level's own `spawn_robot()` on a
  2 m ring around its four robot spawn points (36 robots in all). A
  `Camera3D` of the driver flies one closed loop through the four robot
  spawn points and the first player spawn point, 2 m up, over the warm-up
  and measured frames; its position is a function of the frame index only.
- **What the level measures.** `load_ms` is `load()` of
  `res://level/level.tscn`; `instance_ms` is `instantiate()`, `add_child()`
  (the level's `_ready`: GI setup, its four robots, the player) and the
  extra robots; `load_allocs` counts the allocations of both. Over the 600
  measured frames: `main_ms` is the physics step plus the process step, each
  from the SceneTree's own callback (which runs first) to a marker node with
  the highest priority (which runs last); `render_cpu_ms` is
  `viewport_get_measured_render_time_cpu()` of the root viewport plus
  `get_frame_setup_time_cpu()`; `fly_allocs` and `fly_reallocs` count the
  measured frames. Judged: `load_ms`, `instance_ms`, `main_ms_p50`,
  `render_cpu_ms_p50` and `peak_ws_mb`, and the counts by the count rule.
  `first_frame_ms`, the p95 values, `gpu_ms_p50` (viewport GPU time) and
  `wall_ms` are `INFO` only: GPU time and the wall frame time follow the
  renderer, so they never judge a core change
  ([renderer-bound frames](/mistakes/renderer-bound-frame-for-scene-changes.md)).
  The level runs windowed, so close GPU-heavy programs before measuring.
- **Level counts are not deterministic.** The first two A/A runs
  (2026-11-21) gave `load_allocs` within 31 of 1.1 million and
  `fly_allocs` within 1,300 of 0.7 million (0.2%) across twins and rounds
  (resource loading and rendering use worker threads, and the robots fire
  at the idle player); only `fly_reallocs` was identical. Its COUNTS lines
  read `COUNT-NONDETERMINISTIC` with the spread, so the level's counts are
  reported, never judged by "any increase" (Pitfall 4 of the research).
- **Exit.** The driver frees the level after the BENCH line and keeps the
  tree running 500 ms before it quits, so the audio thread has removed the
  music's playback ([quit right after freeing audio](/mistakes/quit-right-after-freeing-audio.md)).
- **Order.** `summary.csv`, `counts.csv` and the printed summary go by
  workload, then case, then build type, then metric.

# Micro tier: container micro-benchmarks (BENCH-04)

`bench.py micro` builds `devtools/bench/micro/containers_bench.cpp` once
per side with MSVC and runs every case in one process. The harness uses
Godot's real container headers, `memory.cpp`, `fork_alloc_counters.cpp`,
`hashfuncs.cpp` and four String methods copied verbatim from
`ustring.cpp` (`operator==`, `hash()`, both `operator<`); only
diagnostics are stubbed, and any diagnostic fails the run.

- **Sides.** `--aa`: the control (working tree) and a second control build
  in its own directory. `--prev <ref>`: previous (the container headers,
  `hashfuncs.cpp` and `ustring.cpp` of `<ref>` in an include directory
  before the tree), candidate (working tree plus `--defines`, as `/D`) and
  control (working tree). All sides use the same flags: `/O2 /MT /Gy /Gw
  /EHsc /std:c++20 /Zc:__cplusplus /GR- /DDEBUG_ENABLED /DTOOLS_ENABLED
  /DWINDOWS_ENABLED /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
  /DFORK_ALLOC_COUNTERS=1`, linked with `/OPT:REF /OPT:ICF`. `/EHsc` is
  there because Boost needs exceptions; every side gets it. `meta.json`
  records the flags, defines, the `cl` version and the sha256 of every
  source per side; the tree sides are hashed again before the first sample
  and the series stops if a file changed after its build.
- **Rounds.** One warm-up pair (round 0, discarded), then `--samples`
  rounds (default 11, at least 5), odd rounds in side order, even rounds
  reversed, each process pinned to the bench CPUs. Every case is an item
  `micro/<case>` with the metric `ns_per_op` and the build type
  `standalone`: `aa` prints `FLOOR micro/<case> ns_per_op standalone` and
  `--write-floors` stores it like any floor; `--prev` prints `VERDICT`
  lines (`--target` names the cases judged by the accept rule, the others
  get the regression rule).
- **Parity.** Every case prints `ns_per_op`, `operations`, `live_bytes`
  (Godot bytes held by one container of the case at its end, read
  untimed), `allocs` (Godot allocations in the timed region) and an
  order-independent checksum. The series stops when a side's processes
  differ in operations, checksum, live bytes or allocations
  (non-repeatable), or when a side's operations or checksum differ from
  the first side's (`hash_bytes_*` checksums excepted: a candidate may
  change the byte hash). In `aa` live bytes and allocations must match
  across sides too; in a comparison they are a result: `counts.csv` holds
  both per case and `COUNTS` lines name every case where the candidate
  differs from the control (`INCREASE` or `NO-INCREASE`).
- **Matrix.** Cases are `<container>_<key>_<op>_<N>`. HashMap, AHashMap,
  HashSet, RBMap and RBSet with keys `i64` (64-bit integers from an odd
  multiplier, inserted in index order, queried in a shuffled order), `ptr`
  (addresses into one arena, shuffled) and `str` (16 ASCII decimal digits,
  compared with String `==`; non-ASCII keys are not part of the suite);
  ops `insert`, `hit`, `miss`, `erase`, `churn` (erase one, insert one),
  `iterate`, `copy`. List (`i64`): `push`, `iterate`, `copy`, `erase` (by
  element). Vector and LocalVector of `i64` and `str`: `push`, `iterate`,
  `copy` (Vector copies are detached with `ptrw()`). N = 0, 1, 16, 256,
  4096, 32768, 1048576. Plus `string_copy_detach_16`/`_128` and
  `hash_bytes_8` to `_65536` from the old harness. StringName and Variant
  keys need the engine: see the next section (`micro_engine`).
- **Work per case.** About 2^17 element operations: lookups, iterations
  and erases repeat over small containers; at large N, lookups and erases
  touch 2^17 keys of the container (in query order) and churn swaps 2^15
  keys out and back. N = 0 runs 2^17 lookups, iterations, copies, erase
  calls or constructions on the empty container (operations above 0,
  live bytes 0). One process takes about 12 s with the controls.
- **Left out** (to keep one process under 15 s, or not applicable):
  `copy` at N = 1048576 for every container; every RBMap and RBSet `str`
  case at N = 1048576; `hit` and `churn` at N = 0 (no key to find or
  erase).
- **Third-party controls.** `ctl_<lib>_<key>_<op>_<N>` rows (keys `i64`
  and `str`, ops `insert`, `hit`, `miss`, `iterate`, N = 256 to 1048576)
  for ankerl::unordered_dense::map (`unordered_dense`, v5.3.3,
  `7c58a8b7a4`, MIT), boost::unordered_flat_map and unordered_node_map
  (`boost_flat`, `boost_node`; Boost 1.92.0 `b2-nodocs` release archive,
  sha256 `e2a814b3a1...`, equal to GitHub's asset digest, BSL-1.0),
  gtl::flat_hash_map and gtl::btree_map (`gtl_flat`, `gtl_btree`; v1.3.1,
  `b9a2586edd`, Apache-2.0), emhash8::HashMap (`emhash8`; v1.3.0, commit
  `e84fa51009`, MIT; the tag object is `2f6301b972`), `std::map` and
  `std::unordered_map` (`std_map`, `std_umap`). `str` keys are Godot
  Strings with `String::hash()` (each library mixes it) and `==` or `<`.
  They are fetched into `devtools/out/bench/src/` (git pins checked with
  `rev-parse`, the archive by size and sha256 on every use, extracted with
  tarfile's `data` filter), never vendored or tracked. A micro series
  fetches them itself and defines `BENCH_CONTROLS`, the same on every
  side. Controls use the CRT allocator, so their `live_bytes` and
  `allocs` read 0.
- **Layout sentinel.** The controls are the same code on every side, so a
  control row that moves measured code layout (Pitfall 2 of the research).
  A comparison prints `LAYOUT-SENTINEL: <k> of <m> control rows moved`:
  the control rows whose candidate-vs-control median difference is beyond
  their floor. With k above 0 the Godot rows of that run are not evidence.
  The first same-source comparison (a no-op define, n = 5 floors from a
  5-round A/A run, bench CPUs 15-43% busy) printed 5 of 256: at that load
  and n the sentinel also fires on noise, so 02-10 re-checks its rate at
  the default load with n = 11 floors. The side binaries are never
  byte-identical (MSVC writes the output path and a timestamp), so a moved
  control can't be told from a layout change by hashing the exes.
- **Ratio to the fastest control.** For each Godot case with control rows
  of the same key, op and N, the summary prints `RATIO` (and
  `summary.csv` the columns `fastest_control`, `ratio_to_fastest_control`)
  from the control binary's medians: RBMap and RBSet against `gtl_btree`
  and `std_map`, the hash containers against the hash controls (sets
  against maps). 02-10 uses it as the "best plausible speedup" of D-07.
- **Never decides alone.** A micro row never accepts or rejects a
  candidate; acceptance comes from the engine workloads. Micro rows feed
  the max-gain estimate and the layout sentinel.
- **One runner at a time.** `micro` takes the same lock as `aa` and
  `compare`; `summary.csv` is written last, so an interrupted series has
  none and is never read as a result.
- It replaced the old container driver (deleted in 02-13; the comparison
  is in [the migration record](/project/benchmark-driver-migration.md)).

# Micro tier in the engine: StringName and Variant keys (`micro_engine`)

The standalone harness can't link StringName, Variant or Dictionary (they
need the engine), so those cases are doctest cases in
`tests/fork/test_fork_bench_variant_keys.cpp`, run by `aa` and `compare`
as the workload `micro_engine`.

- **Skipped everywhere else.** Each of the 6 cases is `TEST_CASE("[Bench]
  <name>" * doctest::skip())`, and no name contains "Fork": `--test` and
  the `*Fork*` contract runs count them as skipped and run none of them
  ([unit-test baseline](/project/unit-test-baseline.md)).
- **Builds.** The bench tests builds: `build.ps1 -Tests -Bench`
  (`extra_suffix=bench.tests`, `FORK_ALLOC_COUNTERS=1`), `-Variant aa`
  (`bench.aa.tests`) for `aa`, `-Variant <name>` for the candidate of
  `compare`; `bench.py` builds them unless `--skip-build`. A series that
  mixes `micro_engine` with game workloads builds and runs both kinds.
- **Run.** One process per sample runs every case: the real exe with
  `--test --test-case=*Bench* --no-skip`, pinned like every workload, one
  discarded cold round, then n = 11 rounds (fixed, whatever the cold round
  takes; `--rounds` overrides), alternating order. No worker-count check
  (no game runs). One process takes about 5.7 s, of which about 2.3 s are
  timed; the rest builds keys and untimed containers. One key set serves
  every StringName-keyed container (`sname_keys`: HashMap, AHashMap,
  Dictionary) and one name set the three StringName operations
  (`sname_ops`): interning 2M new names takes about 2 s, and a test case
  per container did it three times per process (11.1 s; 02-10).
- **Items.** Each case prints `BENCH: workload=micro_engine case=<name>
  ns_per_op=... operations=... allocs=... checksum=...` and is the item
  `micro_engine/<name>`: `ns_per_op` is judged (`FLOOR` in `aa`,
  `VERDICT` in `compare`), `allocs` (Godot allocations in the timed
  region) goes through the count rule (`COUNTS`), and the process item
  `micro_engine` has `peak_ws_mb`. Operations and checksum must repeat in
  every process of the series, every binary and round (`engine_parity`);
  otherwise the series stops. A failed `CHECK` fails the process (exit
  code), and with it every case of that sample.
- **Matrix.** Case names follow the standalone harness,
  `<container>_<key>_<op>_<N>`: `hashmap_sname`, `ahashmap_sname`
  (`HashMap`/`AHashMap<StringName, int>`), `hashmap_variant`
  (`HashMap<Variant, Variant>` with Vector2 keys), `dict_str`,
  `dict_sname`, `dict_i64`, `dict_variant` (Dictionary with String,
  StringName, int and Vector2 keys); ops `insert` (`set()` for
  Dictionary), `hit` (`getptr()`), `miss` (`has()`), `erase`, `iterate`,
  `copy` (`duplicate()` for Dictionary, whose copy only shares); N = 0, 1,
  16, 256, 4096, 32768, 1048576, the same work per case as the standalone
  harness (about 2^17 element operations). Left out as there: `copy` at
  N = 1048576 and `hit` at N = 0. Plus `sname_from_string_<N>`,
  `sname_from_cstr_<N>` (a StringName made from a String or a C string
  of one of N existing names, destruction included) and
  `sname_compare_<N>` (two names in query order), N = 1 to 1048576:
  280 + 18 = 298 cases.
- **Release coverage.** `tests=yes` builds for `template_release` too
  (probe of 02-07, `build.ps1 -Tests -Bench -Target template_release`,
  4.1 min, `bin/godot.windows.template_release.x86_64.bench.tests.exe`): it
  runs the 10 cases (10 passed, 0 failed; 1554 cases in that binary, the
  editor-only tests are not built in it), so `micro_engine` runs on both
  build types and `aa --builds template_release` prints a `FLOOR` per case
  (299 lines in the probe series). Each new tests variant takes about 3 GB
  of objects and a 0.7 GB pdb; see [the disk-full mistake](/mistakes/bench-variants-filled-the-disk.md).
- **First series** (smoke, i9-14900K, MSVC, n = 5, not floors): editor at
  the default `--max-load` (bench CPUs 7.9% busy), template_release with
  `--max-load 50` (10.9%). Every process ran 298 cases, identical
  operations and checksums and identical allocation counts in every
  process (298 `COUNTS ... identical`). Control medians on the editor
  build at N = 4096: `hit` 5.6 ns AHashMap StringName, 10.0 ns HashMap
  StringName, 18-19 ns Dictionary StringName and int, 21 ns
  `HashMap<Variant, Variant>` and 23 ns Dictionary with Vector2 keys,
  34 ns Dictionary String; StringName from an existing String 0.42 us
  (0.77 us at 1M names, one global table), comparison 1.1 ns.
  Floors (largest of 5 paired differences): median 30% on both build
  types. The process medians of all cases agree within 1%, but about 10%
  of the cases in each process read more than 1.3x their median, spread
  over all processes and sizes; one such case in one round sets the
  floor. 02-10 measures the floors at n = 11 and decides whether the
  cases need longer timed regions (now about 2^17 operations, 0.1 to
  14 ms). **Outcome (02-10, PC idle):** median case floor 6.7-7.4%, p90
  24-26%; the timed regions stay as they are, since micro cases never
  decide acceptance; a Variant-container candidate would lengthen them
  first ([floors](/project/benchmark-noise-floors.md)).

# Shares (BENCH-07, BENCH-10)

A family's share is measured from a sampling profile of each target
workload, per build type (editor and `template_release`) and per
compiler: `bench.py profile`, with `sampler.exe` on Windows and `perf
record` plus `perf_to_tsv.py` on Linux ([profiling](../../devtools/profile/README.md),
"Linux" below).
Both feed `analyze.py families`, one rule set for every OS:

- each busy sample (waits excluded) goes to one family by its innermost
  frame, inline frames included, first match of `FAMILIES` wins:
  formatting by function name, then allocation, hash tables, Variant
  containers, trees/lists, CowData; anything else is `other`;
- the share is computed twice, of the **busiest thread** and of all busy
  samples, and the larger of the two (`share_pct` in the JSON) is the
  family's share for that workload and build;
- the bench counter (`core/os/fork_alloc_counters`, in every `-Bench`
  build and in no shipped one) is its own excluded row, never part of a
  family; its share is printed so a profile where it is large is seen.

The max gain of a family is then `share x (1 - 1/speedup)` (Skip rule
above, D-07), with the largest share across build types and compilers.
A profile taken with `--max-load` above the default is not a recorded
share. Every workload is profiled with its measured command (02-10:
`editor_startup` is `--quit`, startup only; a first profile with
`--quit-after 3000` measured mostly the frame delay after startup). The
MSVC shares and decisions: [family shares](/project/family-shares.md).

# Budget (D-03)

At most 30 minutes of measurement per compiler (MSVC; GCC and Clang each),
builds excluded, for editor and `template_release` and every workload. The
runner times the cold round and plans the rounds per workload: processes
under 3 s get n = 11 (k = 9), the others n = 5 (k = 5). It prints the plan
(workload, seconds per process, class, n, k, processes, minutes) and the
total before round 1, and refuses a plan above `--budget` (default 30).
`--rounds` sets n for every workload (5 or more). A/A runs have two
binaries, comparisons three.

**The suite.** `--workloads all` is `SUITE` in `bench.py`:
`scene:instance:10000`, `scene:churn:10000`, `scene:reparent:10000`,
`scene:script:10000`, `script:nodes`, `script:script`, `script:instance`,
`gdbench`, `editor_startup`, `level`, `micro_engine`; the standalone
container harness is the micro tier (`micro --aa`, `micro --prev`).
Planned full compare on MSVC (i9-14900K, three binaries, editor and
`template_release`, default n), from the cold rounds of `suite-10a`
(02-10, n = 5, 912 `FLOOR` lines, no failed sample):

| Item | s/process (cold) | n | Minutes |
| --- | --- | --- | --- |
| 7 scene and script workloads | 0.15 to 1.52, 4.29 together | 11 | 4.7 |
| `gdbench` | 9.5 | 5 | 4.8 |
| `editor_startup` (editor only) | 2.2 | 5 | 0.6 |
| `level` | 7.7 | 5 | 3.9 |
| `micro_engine` | 5.8 | 11 | 6.4 |
| micro tier (one build, warm-up pair included) | 11.3 | 11 | 6.8 |
| **Total** (unrounded rows) | | | **27.0** |

The warm processes are shorter (25.2 min). Before 02-10, `micro_engine`
took 11.2 s per process (12.3 min, 33.0 min in total), almost all of it
interning the same 2M StringNames in three test cases; sharing one key
set brought it to 5.7 s with every case kept, so nothing else was cut
(gdbench `ITERATIONS`, level frames and the 1M-cell cases stay).

# What every run records (BENCH-11)

Each series writes `devtools/out/bench/<series>/` (gitignored; a series
started in the same second as another gets a `-2` suffix):

- `raw.csv`: every sample in run order (cold round as round 0): round,
  position, binary, build type, workload, case, metric, value, failed flag
  and reason, CPU and wall seconds, `clock_mhz`, `clock_flag`.
- `summary.csv`: per workload, metric and build type the medians, the
  median paired differences, wins, losses, the floor and the verdict.
- `counts.csv`: per workload, count and build type the range of each
  binary's counts and the count verdict.
- `meta.json`: commit, dirty flag and the sha256 of `git diff HEAD`, the
  binaries in round-1 order with their commit, `build.ps1` and SCons
  arguments and cppdefines, MSVC toolset and Visual Studio versions, CPU
  name, BIOS, microcode revision, OS build, power plan GUID and parking
  values, Game Mode, bench-CPU load, affinity mask, worker counts, clocks
  per round, the round plan, the Python version and the runner arguments.
  A value the runner can't read is written `"unavailable"`, never left out
  or written as 0. Counter paths are stripped before writing (they carry
  the host name).

On Linux `meta.json` has `compiler_version` (the first line of `gcc
--version` or `clang --version`) and `distribution` instead of the MSVC
and Visual Studio versions, `os_build` is the kernel release (a floor
stays valid across kernel patch releases, not across 6.17 to 6.18),
`build_command` replaces the `build.ps1` arguments, the D-13 values
(`governor`, `epp`, `no_turbo`, `sched_ext`, `sched_ext_ops`,
`compiler_hold`, `perf_event_paranoid`) replace the power plan, and
`temperature_c` holds the mean package temperature per round.

Floors live in `devtools/bench/floors.json` (`aa --write-floors`), one
entry per workload, case, metric, build type and compiler with its n,
date and machine state; the human-readable copy goes in `kb/` in the same
commit.

# Linux (CachyOS, D-15)

CachyOS runs on the same PC (dual-boot), with the repository cloned on
the Linux file system. `devtools/bench/bench_linux.py` gives `bench.py`
the functions of the Windows adapter, so the statistics, verdicts,
floors and outputs are the same code on both. Run it with `python3`;
it needs nothing beyond the standard library and `perf`
(`sudo pacman -S --needed perf`).

- **Compiler (`--compiler gcc|clang`, default `gcc`).** It picks the
  build and is part of every floor key, so GCC, Clang and MSVC each get
  their own A/A floors and shares. The three binaries differ by more than
  the compiler (Pitfall 8): production builds use **full LTO with GCC,
  ThinLTO with Clang and no LTO with MSVC** (`platform/linuxbsd/detect.py`,
  `platform/windows/detect.py`). Numbers are compared within one
  compiler, never across. Builds are the setup script's `scons` call
  with the bench options (`debug_symbols=yes disable_path_overrides=no
  extra_suffix=bench[.<variant>][.tests] cppdefines="FORK_ALLOC_COUNTERS=1
  ..."`; Clang adds `use_llvm=yes linker=lld`, tests builds `tests=yes
  object_prefix=t_`). Clang binaries put `.llvm` before the suffix:
  `bin/godot.linuxbsd.editor.x86_64.llvm.bench.aa`. The A/A floors alone
  need eight bench builds (editor and `template_release`, control and
  twin, two compilers) plus the tests builds for `micro_engine`; the
  GCC full-LTO links are long.
- **Pinning (D-12).** The bench CPUs are the `cpu_core` PMU's CPUs
  (`/sys/bus/event_source/devices/cpu_core/cpus`) grouped into cores by
  `topology/thread_siblings_list`, minus the core that holds the lowest
  CPU; never CPU numbers from Windows. The runner moves itself to the
  other CPUs (`os.sched_setaffinity`) and reads each child's set back.
- **Memory and CPU time (BENCH-05).** The runner reaps each child with
  `os.wait4`: peak memory is `ru_maxrss` (the peak resident set, KiB),
  CPU time `ru_utime + ru_stime`. Peak RSS and the Windows peak working
  set are each compared within their OS only. Linux starts a child's
  `ru_maxrss` at the runner's own peak (kept across `vfork` and `exec`),
  so the runner must stay small: it hashes big files in chunks, and a
  child peak that is not above the runner's refuses the run
  (`child_peak_bytes`;
  [the mistake](/mistakes/child-peak-rss-inherits-the-runners.md)).
- **Checks that refuse (D-13, D-17).** Read only; each refusal prints
  the command that fixes it, for the user to run (the runner never runs
  sudo or writes a system file). A file that does not exist here is
  recorded as unavailable, not refused.

  | Check | Wants | Fix printed |
  | --- | --- | --- |
  | `cpufreq/scaling_governor` of the bench CPUs | `performance` | `sudo cpupower -c <cpus> frequency-set -g performance` |
  | `cpufreq/energy_performance_preference` | `performance` | `echo performance \| sudo tee` the bench CPUs' files (the performance governor sets it) |
  | `intel_pstate/no_turbo` (boost, D-14) | `0` | `echo 0 \| sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo` |
  | `/sys/kernel/sched_ext/state` (Pitfall 9; `root/ops` recorded) | `disabled` | stop the scx scheduler for the series (`sudo systemctl stop scx_loader.service`, or `scx.service`) |
  | `IgnorePkg` in `/etc/pacman.conf` | `gcc gcc-libs clang llvm llvm-libs lld` held | add `IgnorePkg = gcc gcc-libs clang llvm llvm-libs lld` under `[options]` |
  | `/proc/sys/kernel/perf_event_paranoid` (with `--counters`, `profile`, `check`) | 2 or less | `sudo sysctl kernel.perf_event_paranoid=2` |

  Plus, as on Windows: no `cc1plus`, `cc1`, `lto1`, `collect2`, `ld`,
  `lld`, `clang` or SCons process (`/proc/*/comm`; never killed), the
  bench CPUs under 10% busy (`/proc/stat` over 3 s) and one runner
  (`fcntl.flock` on `devtools/out/bench/.lock`).
- **Clock and temperature (D-14).** A thread on the home CPUs samples
  `scaling_cur_freq` of the bench CPUs every 0.25 s; the round's mean is
  its clock (flagged as on Windows), and the hwmon `coretemp`
  "Package id 0" temperature is averaged per round.
- **Counters (BENCH-08).** `aa` and `compare` take `--counters`: after
  the timed rounds, one extra process per binary, build type and
  workload runs under `perf stat -x, -o <file> -e
  cpu_core/instructions/u,cpu_core/cycles/u,cpu_core/cache-misses/u,cpu_core/cache-references/u
  -- <command>` (the P-core PMU only, Pitfall 10). `perf` as the parent
  of a timed run would own its `wait4` numbers, so counter passes are
  never timed or memory samples. The runner prints instructions, cycles,
  IPC, cache misses and the miss rate after the medians and writes the
  `instructions`, `cycles`, `cache_misses` and `cache_references`
  columns of `summary.csv` as `label=value` per binary (`n/a`: perf
  printed `<not counted>` or `<not supported>`). Counts are reported,
  never judged, and their run-to-run spread has no floor (one pass).
- **Profiles (BENCH-07).** `bench.py profile` runs `perf record -e
  cpu_core/cycles/u -F 997 -g --user-callchains -o <stem>.perf.data`
  (`--user-callchains`: at `perf_event_paranoid` 2 a call graph that may
  include kernel frames is refused) with the workload pinned by
  `taskset` (perf itself stays on the home CPUs); frame workloads start
  recording after `--warmup` (`-D`) and end after `--seconds` (SIGINT
  to perf, which ends its workload). Then `perf script -F
  tid,comm,ip,sym,dso,srcline --inline --full-source-path` goes through
  `devtools/profile/perf_to_tsv.py` into the same TSV, and `analyze.py
  families` applies the Windows rules. On Clang binaries `perf script`
  gets `--addr2line llvm-addr2line`: binutils `addr2line` restarts on
  timeouts there (6-7 min per profile) and adds no source line, and
  perf 7.3 skips the external tool with it (the same TSV in 8 s).
  Two Linux-only facts the analysis handles: perf names glibc's
  internal allocator frames `[unknown]` with a `malloc/malloc.c` line,
  so allocation also takes that file, and GCC and Clang name translation
  units repository-relative (`core/os/memory.cpp`).
- **Micro tier.** `micro` compiles the harness with `g++` or `clang++`,
  the same flags on every side and for both compilers (`-O2
  -std=gnu++20 -fno-rtti -DDEBUG_ENABLED -DTOOLS_ENABLED
  -DLINUXBSD_ENABLED -DUNIX_ENABLED -DFORK_ALLOC_COUNTERS=1`, the include
  paths of the Windows build with `platform/linuxbsd`).
- **Level.** Each `level` series exports its `.pck` with the Linux
  control editor; a Windows `.pck` is never copied over. `override.cfg`
  sets `rendering_device/driver.linuxbsd="vulkan"`.
- **What to copy back.** The GCC and Clang entries of
  `devtools/bench/floors.json` (merged, committed with their `kb/` notes),
  and from each series directory `summary.csv`, `counts.csv`,
  `meta.json`, `raw.csv` and `counters/`, and from each profile the
  `.families.json`, `.summary.txt` and `.tsv` (the TSV lets the shares
  be made again when an attribution rule changes, as in 02-12), for
  [the floors](/project/benchmark-noise-floors.md) and
  [the shares](/project/family-shares.md). Binaries, `.perf.data` and
  `.pck` files stay on Linux. Tracked notes use repository-relative paths
  only (the output keeps absolute paths in `runner_args`).
- **Session practice (02-12).** Builds and series run on a Linux
  partition, never an image file on NTFS
  ([mistake](/mistakes/ext4-image-on-ntfs-build-disk.md)); a compiler's
  bench objects take about 40 GB (GCC: 39 GB in `bin/obj`). Hand loops
  over option lists run under bash, not zsh
  ([mistake](/mistakes/zsh-unquoted-args-not-split.md)); a watch on a
  series matches the real process or its output file
  ([mistake](/mistakes/pgrep-f-matches-its-own-shell.md)). Afterwards,
  `sudo systemctl start scx_loader.service` brings the scheduler back
  (the governor resets at reboot); the `IgnorePkg` hold stays only
  while another series is planned.

# Commands

```
python devtools/bench/bench.py check
python devtools/bench/bench.py fetch
python devtools/bench/bench.py aa --workloads scene:instance:10000,scene:churn:10000 --write-floors
python devtools/bench/bench.py aa --workloads script:script,gdbench,editor_startup --rounds 5
python devtools/bench/bench.py compare --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev \
    --target scene:churn:10000 --workloads scene:churn:10000,scene:instance:10000
python devtools/bench/bench.py micro --aa --write-floors
python devtools/bench/bench.py micro --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev
python devtools/bench/bench.py aa --workloads micro_engine --write-floors
python devtools/bench/bench.py profile --workload scene:instance:10000 --build template_release
python3 devtools/bench/bench.py aa --workloads all --compiler clang --write-floors --counters   # Linux
```

Every command but `fetch` takes `--compiler` (Windows `msvc`; Linux
`gcc` or `clang`). `micro` takes `--aa` or `--prev`, and `--name`, `--defines`, `--target`
(`micro/<case>` items), `--samples`, `--floors`, `--write-floors`,
`--max-load`, `--out`; it always builds its sides (seconds each).

`profile` takes `--workload`, `--build`, `--seconds`, `--warmup`,
`--interval-us`, `--max-load` and `--out`; it never builds and writes
`devtools/out/bench/profile/<series>/`.

Options of `aa` and `compare`: `--workloads`, `--builds` (default
`editor,template_release`), `--rounds`, `--budget`, `--floors`,
`--max-load`, `--skip-build` (reuse the binaries), `--out`, and on Linux
`--counters`. `compare` adds
`--name` (the candidate's `build.ps1 -Variant`, binary `bench.<name>`),
`--defines`, `--prev` (required) and `--target` (the workloads, or single
cases such as `micro/<case>` or `gdbench/<case>`, judged by the accept
rule; the others get the regression rule). Later plans add
their workloads and options to [the runner README](../../devtools/bench/README.md).

# Disk

Each bench variant keeps about 3 GB of objects in `bin/obj`. A previous
build lives in `devtools/out/bench/wt/<sha7>/` with its own objects; after
deleting that folder, run `git worktree prune`. A `level` series writes a
570 MB `.pck` into its series directory, and the fetched TPS demo with its
import cache takes about 1.5 GB in `devtools/out/bench/src/tps-demo/`.
The micro controls take about 0.5 GB in `devtools/out/bench/src/` (Boost:
the 73 MB archive and its 443 MB, 49,000-file tree); a micro series keeps
its side builds in `<series>/build/`.

`check` prints `disk_free_gb` (free space on the repository's disk), and
a series that builds refuses below 5 GB (`MIN_BUILD_FREE_GB`): a full disk
fails the link with LNK1106 and cuts the series' own logs short
([the disk-full mistake](/mistakes/bench-variants-filled-the-disk.md)). A
tests variant (`micro_engine`) adds about 3 GB of objects (2.1 GB for
template_release) and a 0.5-0.65 GB pdb; the objects of a linked variant
can be deleted when it won't be rebuilt soon.

# Lessons this protocol applies

- [Metrics that measured noise](/mistakes/metric-measured-noise.md),
  [credited a fix on 3 runs of a noisy test](/mistakes/three-runs-of-a-noisy-test.md),
  [conclusions from unconverged runs](/mistakes/conclusions-from-unconverged-runs.md)
- [Separate runs had an order bias](/mistakes/separate-runs-order-bias.md),
  [warm runs on a new exe](/mistakes/warm-runs-on-a-new-exe.md)
- [Judged a scene-side change on renderer-bound frame time](/mistakes/renderer-bound-frame-for-scene-changes.md)
- [Copy benchmark retained-byte omission](/mistakes/copy-benchmark-retained-bytes.md),
  [benchmark snapshot dependencies](/mistakes/benchmark-snapshot-dependencies.md),
  [a benchmark ran with the feature it measures inactive](/mistakes/bench-without-the-feature-active.md)
- [A relative benchmark-file path went to the project folder](/mistakes/relative-path-after-project-chdir.md),
  [a level driver quit right after freeing the level's audio](/mistakes/quit-right-after-freeing-audio.md)
- [Ran a GI test with auto exposure and real-time timing](/mistakes/uncontrolled-test-conditions.md)
  (the level driver pins every setting the demo leaves to the player)
- [A test scene that could not quit](/mistakes/test-scene-without-hard-quit.md),
  [single-flag benchmark runs measured the defaults](/mistakes/powershell-single-arg-splat.md)
- Successes: [control build for layout effects](/successes/control-build-for-layout-effects.md),
  [actual-container A/B without checkout switches](/successes/actual-container-ab-bench.md),
  [one editor copy per variant](/successes/editor-copy-per-variant.md)
