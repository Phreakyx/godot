---
type: Finding
title: Old benchmark drivers against the runner
description: On the same binaries, devtools/bench/bench.py reproduces the three old drivers - scene_bench.ps1 (instance and churn at 10k nodes within 1-3%, identical node counts and memory), core_templates_bench.py (same-code cases within 0-1% once the allocation counter is accounted for; the integer-map cases are different workloads in the new matrix) and profile.ps1 (script-mode source-directory shares within 0.7 points); the scene driver is deleted with this entry, the profile and container drivers in plan 02-13's commit.
tags: [benchmark, testing, tooling, phase-2]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: runner
    resource: ../../devtools/bench/bench.py
    title: The runner (aa, micro, profile)
  - id: scene
    resource: ../../devtools/bench/workloads/scene.gd
    title: Scene workload, a copy of the old scene_bench.gd with BENCH lines
  - id: micro
    resource: ../../devtools/bench/micro/containers_bench.cpp
    title: Container matrix, grown from devtools/test/core_templates_bench.cpp
  - id: old-micro
    resource: ../../devtools/test/core_templates_bench.py
    title: Old container driver (with core_templates_bench.cpp; deleted in the next commit)
  - id: old-profile
    resource: ../../devtools/profile/profile.ps1
    title: Old profile driver (with core_workload.gd; deleted in the next commit)
  - id: protocol
    resource: ../tooling/benchmark-protocol.md
    title: The benchmark protocol
---

# Why

Phase 2 decision D-02: the runner absorbs the old drivers, and each old
driver is deleted only after the runner has reproduced its numbers, so
nobody measures with two rule sets. This entry is that record.

# Conditions

2026-11-21, commit `a3d4b27aaf`, i9-14900K, Windows 11 26100, MSVC
14.51, power plan Bitsum Highest Performance. Every runner series ran at
the default bench-CPU load limit (`--max-load 10`; measured 3.3-6.8%).
Engine binary for both scene and profile drivers: the bench control editor
`bin/godot.windows.editor.x86_64.bench.exe` built for the allocation
counter (version string `5cc9b6586`, counter source committed as
`5e91ec2565`); the old drivers start its `.console.exe` wrapper, the
runner starts the real exe. n = 5 everywhere. None of these runs is a
floor.

# Scene: scene_bench.ps1

Old: `scene_bench.ps1 -Builds <bench console exe> -Scenarios
@('instance','churn') -Counts @(10000) -Rounds 5` (with `-File` the
list parameters arrive as one string; see
[the splat mistake](/mistakes/powershell-single-arg-splat.md)). New:
`bench.py aa --skip-build --workloads scene:instance:10000,scene:churn:10000
--builds editor --rounds 5` (control medians). `aa` keeps only its judged
metrics, so `op`, setup, static memory and nodes come from the runner's
own command lines (`scene_command`, pinned with `run_pinned`, 5 runs
after a cold one).

| Case | Metric | Old | Runner | Runner/old |
| --- | --- | --- | --- | --- |
| instance 10000 | frame_ms_p50 (aa) | 8.312 | 8.248 | 0.99 |
| instance 10000 | frame_ms_p50 (runner command) | 8.312 | 8.321 | 1.00 |
| instance 10000 | op_ms_p50 | 8.306 | 8.314 | 1.00 |
| instance 10000 | setup_ms | 0.95 | 0.95 | 1.00 |
| instance 10000 | static_mb | 60.0 | 60.0 | 1.00 |
| instance 10000 | nodes | 10002 | 10002 | 1.00 |
| churn 10000 | frame_ms_p50 (aa) | 0.256 | 0.258 | 1.01 |
| churn 10000 | frame_ms_p50 (runner command) | 0.256 | 0.262 | 1.02 |
| churn 10000 | op_ms_p50 | 0.255 | 0.260 | 1.02 |
| churn 10000 | setup_ms | 8.47 | 8.24 | 0.97 |
| churn 10000 | static_mb | 61.4 | 61.4 | 1.00 |
| churn 10000 | nodes | 10012 | 10012 | 1.00 |

All within 3%. The runner differs in pinning to P-cores 1-7, `--fixed-fps
60`, the real exe instead of the wrapper, a discarded cold round and the
allocation count bracket in `scene.gd`; none of these moves the numbers
beyond the round-to-round spread (A/A floor of this run: 2.35% instance,
2.29% churn). The runner adds peak working set (113.6 and 115.3 MB) and
allocation counts (4961487 and 96924, identical in every round).

Not carried over: the old `-Visual` switch (MeshInstance3D with a
window). `scene.gd` still accepts `--visual` when run by hand; windowed
frames are renderer-bound for scene changes anyway
([why](/mistakes/renderer-bound-frame-for-scene-changes.md)).

# Micro: core_templates_bench.py

Old: `core_templates_bench.py --samples 5` (HEAD against the clean
working tree, the same code; the candidate column is used). New:
`bench.py micro --aa --samples 5` (control medians). The old 24 cases ran
at N = 32768 with dense `int` keys 0..N-1 in shuffled order (miss keys
`key + N`, churn moves to fresh ranges `key + pass * N`), 4096 String keys
of 16 or 128 characters, and Vector of `int`.

| Old case | New case | Old ns/op | New ns/op | New/old | Old live B | New live B |
|---|---|---|---|---|---|---|
| `hashmap_insert` | `hashmap_i64_insert_32768` | 70.03 | 55.24 | 0.79 | 1376316 | 1638460 |
| `hashmap_hit` | `hashmap_i64_hit_32768` | 10.97 | 14.41 | 1.31 | 1376316 | 1638460 |
| `hashmap_miss` | `hashmap_i64_miss_32768` | 11.34 | 15.56 | 1.37 | 1376316 | 1638460 |
| `hashmap_churn` | `hashmap_i64_churn_32768` | 36.22 | 53.11 | 1.47 | 1376316 | 1638460 |
| `ahashmap_insert` | `ahashmap_i64_insert_32768` | 32.17 | 25.80 | 0.80 | 917504 | 1310720 |
| `ahashmap_hit` | `ahashmap_i64_hit_32768` | 5.39 | 9.51 | 1.77 | 917504 | 1310720 |
| `ahashmap_miss` | `ahashmap_i64_miss_32768` | 9.23 | 12.39 | 1.34 | 917504 | 1310720 |
| `ahashmap_churn` | `ahashmap_i64_churn_32768` | 15.87 | 20.87 | 1.32 | 917504 | 1310720 |
| `hashset_insert` | `hashset_i64_insert_32768` | 34.16 | 29.27 | 0.86 | 786512 | 983144 |
| `hashset_hit` | `hashset_i64_hit_32768` | 10.76 | 14.28 | 1.33 | 786512 | 983144 |
| `hashset_churn` | `hashset_i64_churn_32768` | 27.30 | 35.75 | 1.31 | 786512 | 983144 |
| `vector_append` | `vector_i64_push_32768` | 3.11 | 3.54 | 1.14 | 163880 | 327736 |
| `vector_copy_detach_4096` | `vector_i64_copy_4096` | 183.50 per copy | 0.19 per element (778 per copy) | 4.24 per copy | 32816 | 43176 |
| `vector_sort_unvalidated_4096` | (none) | 94642.19 | - | - | 16408 | - |
| `string_copy_detach_16` | `string_copy_detach_16` | 42.08 | 45.48 | 1.08 | 184 | 184 |
| `ahashmap_string_hit_16` | `ahashmap_str_hit_4096` | 16.21 | 18.73 | 1.16 | 163840 | 163840 |
| `string_copy_detach_128` | `string_copy_detach_128` | 46.31 | 49.73 | 1.07 | 1080 | 1080 |
| `ahashmap_string_hit_128` | (none) | 58.94 | - | - | 163840 | - |
| `hash_bytes_8` | `hash_bytes_8` | 2.29 | 2.29 | 1.00 | 0 | 0 |
| `hash_bytes_32` | `hash_bytes_32` | 2.29 | 2.29 | 1.00 | 0 | 0 |
| `hash_bytes_128` | `hash_bytes_128` | 4.25 | 4.26 | 1.00 | 0 | 0 |
| `hash_bytes_256` | `hash_bytes_256` | 23.99 | 24.11 | 1.00 | 0 | 0 |
| `hash_bytes_4096` | `hash_bytes_4096` | 111.33 | 111.45 | 1.00 | 0 | 0 |
| `hash_bytes_65536` | `hash_bytes_65536` | 1609.86 | 1608.08 | 1.00 | 0 | 0 |

Two checks separate the runner from the harness:

- **Pinning has no effect.** The old harness binary, run alternately
  unpinned and pinned to the runner's mask (7 samples each after a warm-up
  pair): pinned/unpinned 0.98-1.03 on 23 cases, 0.93 on `vector_append`.
- **The allocation counter explains the same-code gap.** The new harness
  is built with `FORK_ALLOC_COUNTERS=1` (allocation counts per case); the
  old one has no counter. Built once with the counter off (counter reads
  stubbed) and alternated with the old harness, pinned:
  `string_copy_detach_16` 41.78 against 42.14 ns, `string_copy_detach_128`
  45.97 against 46.49 ns, `hash_bytes_256` 24.20 against 24.03 ns. The
  counter costs about 3.5 ns per allocation, which is what a
  copy-and-detach adds.

So every case whose code is the same in both harnesses reproduces within
1%. The rest are different workloads under the old names, chosen by the
matrix in 02-06:

- **Integer maps and sets** (0.79-1.77): the matrix uses scrambled 64-bit
  keys (`key_value()`) instead of dense 32-bit keys, so elements are 8
  bytes larger (live bytes +262144 for HashMap, +393216 for AHashMap at
  32768 entries) and lookups scatter over a larger table; inserts build
  `TARGET / N` = 4 tables per sample instead of 8, in index order instead of
  shuffled; churn swaps between the hit and miss key sets instead of
  moving to fresh ranges. Insert also pays the counter. These rows are
  not comparable to the old numbers; old results stay with the old names
  in [the template benchmarks](/project/core-template-benchmarks.md).
- **Vector**: `i64` elements (twice the bytes), the counter on every
  growth, and copy counted per element over 32 copies instead of per copy
  over 1024.
- **String-key hit**: the matrix's 16-digit decimal keys differ in all
  positions; the old keys were 8 varying characters padded with `x`.
- **Dropped**: `vector_sort_unvalidated_4096` and `ahashmap_string_hit_128`
  (no 128-character key set in the matrix). Re-add them as matrix cells if
  a candidate touches sorting or long String keys.

Counter-on numbers are what the runner judges: control and candidate both
carry it, so it cancels in a comparison.

# Profile: profile.ps1

Old: `profile.ps1 -Label repro09 -Editor <bench console exe> -GodotArgs
@('--headless', '--path', 'misc/scene_bench_project', '--script',
'devtools/profile/core_workload.gd', '--', '--mode=script',
'--seconds=20')` (6 s warm-up, 15 s at 2 ms, the wrapper's child found by
parent PID). New: `bench.py profile --workload script:script --build
editor` (pinned real exe, 3 s warm-up, 15 s at 1 ms). Main thread, by
source directory (`analyze.py summary`):

| Directory | Old | Runner | Difference (points) |
| --- | --- | --- | --- |
| dll:ntdll | 20.4% | 20.1% | -0.3 |
| core/variant | 20.2% | 19.9% | -0.3 |
| modules/gdscript | 16.5% | 16.3% | -0.2 |
| core/templates | 15.5% | 16.2% | +0.7 |
| crt | 11.9% | 11.7% | -0.2 |
| core/os | 4.6% | 5.3% | +0.7 |
| core/string | 4.8% | 4.6% | -0.2 |
| core/object | 2.8% | 3.2% | +0.4 |
| core/math | 1.9% | 1.8% | -0.1 |
| templates + string + math | 22.2% | 22.5% | +0.3 |
| top function `SafeNumeric::decrement` | 5.61% | 5.71% | +0.1 |

Busy samples: 4454 old, 9382 runner (half the interval). The order of the
directories, the top types (SafeNumeric, String, CowData, HashMap) and the
top functions are the same; every share is within 0.7 points, the
spread of two processes sampled at different intervals (the binary, and
so the allocation counter in `core/os`, is the same in both).
`core_workload.gd` and `workloads/script.gd` run the same script mode.

# Deletion

- This commit deletes the scene driver (`devtools/test/scene_bench.ps1`,
  `devtools/test/scene_bench.gd`) and repoints every live reference to
  `bench.py` and `devtools/bench/workloads/scene.gd`. Old numbers in
  [the scene runtime benchmarks](../../docs/benchmarks/scene-runtime.md)
  and the update log keep their old names.
- Plan 02-13's commit deletes the profile driver
  (`profile.ps1`, `core_workload.gd`) and the container driver
  (`core_templates_bench.py`, `core_templates_bench.cpp` and the scripts
  that import from it) against the tables above, and adds its line here.
- Done: the commit `chore(bench): replace the profile and container
  drivers with devtools/bench` deleted `profile.ps1`, `core_workload.gd`,
  `core_templates_bench.py` and `core_templates_bench.cpp`; the three
  scripts that imported from the container driver (`math_simd_bench.py`,
  `unique_array_bench.py`, `local_vector_copy_candidate.py`) stay and now
  take `ensure_idle` from `devtools/bench/bench_windows.py`.
