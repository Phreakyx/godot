---
type: Finding
title: A/A noise floors on the i9-14900K (MSVC, GCC, Clang)
description: The largest paired difference between two same-source builds for every suite workload, metric and build type, and for every micro case, on MSVC, GCC and Clang; frame floors 1-15% (cheap Linux frames up to 53% from one round), level 0.3-8%, gdbench cases median 2-4%, micro cases median 5-7%; perf counters next to the Linux medians; Linux peak memory of the small workloads still to measure.
tags: [benchmark, noise-floor, msvc, gcc, clang, linux, i9-14900K, phase-2]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T03:00:00Z }
sources:
  - id: floors
    resource: ../../devtools/bench/floors.json
    title: The floors as data, read by bench.py compare
  - id: protocol
    resource: ../tooling/benchmark-protocol.md
    title: Benchmark protocol
---

# What a floor is

The floor of a metric is the largest |paired difference| of n alternating
rounds between the bench control and its A/A twin, two builds of the same
source with their own objects (D-04, [protocol](/tooling/benchmark-protocol.md)).
`compare` reads them from `devtools/bench/floors.json` (5843 entries keyed
`workload|case|metric|build|compiler`: 1959 MSVC, 1942 each GCC and Clang) and refuses a stored floor whose
compiler, BIOS, microcode, OS build or n differ from the run
(`RE-MEASURE-AA`).

# Conditions

| Item | Value |
| --- | --- |
| Source | `dev` at `1fe5a7927c` plus plan 02-10's changes (the commit that adds this entry) |
| Compiler | MSVC toolset 14.51.36231, Visual Studio 18.11.0, `production=yes`, bench builds (`FORK_ALLOC_COUNTERS=1`) |
| CPU | Intel i9-14900K, bench CPUs 0xfffc (P-cores 1-7, both threads of each); the runner on the others |
| Firmware | BIOS 1.K0, microcode 0x12b |
| OS | Windows 11 IoT Enterprise LTSC 2024, build 26100.9711 |
| Power | plan `fe1217fb-3f66-46bf-adc6-e48c27825f80` (Bitsum Highest Performance), core parking off (all `CPMINCORES` 100), Game Mode on |
| Load at start | bench CPUs 4.6% busy (suite), 8.5% (micro); default `--max-load 10`; PC left idle |
| Clocks | suite 3880-4277 MHz, round 5 flagged (1 of 11, more than 5% off the median); micro 3495-3678 MHz, no measured round flagged |
| Workers | game runs 14, editor-mode runs (`editor_startup`) 32 (D-12) |
| Series | `aa --workloads all` (12.8 min planned) and `micro --aa`, 2026-11-21 |

Rounds 6-11 of the suite run only the n = 11 workloads, so their effective
clock (an average over all bench CPUs, idle ones included) is lower than
that of rounds 1-5, where gdbench, the level and editor startup run too.

# Floors per workload (MSVC)

| Workload | Metric | editor | template_release | n |
| --- | --- | --- | --- | --- |
| `scene:instance:10000` | `frame_ms_p50` | 3.04% | 10.80% | 11 |
| `scene:instance:10000` | `peak_ws_mb` | 0.51% | 0.99% | 11 |
| `scene:churn:10000` | `frame_ms_p50` | 6.75% | 9.90% | 11 |
| `scene:churn:10000` | `peak_ws_mb` | 0.55% | 0.40% | 11 |
| `scene:reparent:10000` | `frame_ms_p50` | 3.13% | 8.93% | 11 |
| `scene:reparent:10000` | `peak_ws_mb` | 0.44% | 0.41% | 11 |
| `scene:script:10000` | `frame_ms_p50` | 1.16% | 1.79% | 11 |
| `scene:script:10000` | `peak_ws_mb` | 0.43% | 0.62% | 11 |
| `script:nodes` | `frame_ms_p50` | 5.38% | 5.12% | 11 |
| `script:nodes` | `peak_ws_mb` | 0.44% | 0.71% | 11 |
| `script:script` | `frame_ms_p50` | 1.04% | 6.94% | 11 |
| `script:script` | `peak_ws_mb` | 0.48% | 0.56% | 11 |
| `script:instance` | `frame_ms_p50` | 7.16% | 14.82% | 11 |
| `script:instance` | `peak_ws_mb` | 1.23% | 1.04% | 11 |
| `gdbench` | `peak_ws_mb` | 0.17% | 0.10% | 5 |
| `gdbench` cases (135) | `time_ms` | median 3.8%, p90 11.5%, max 71% | median 3.1%, p90 10.0%, max 33% | 5 |
| `editor_startup` | `wall_ms` | 6.41% | n/a (editor only) | 5 |
| `editor_startup` | `peak_ws_mb` | 1.06% | n/a | 5 |
| `level` | `main_ms_p50` | 5.60% | 0.30% | 5 |
| `level` | `render_cpu_ms_p50` | 7.99% | 1.86% | 5 |
| `level` | `instance_ms` | 5.14% | 2.08% | 5 |
| `level` | `load_ms` | 0.89% | 1.35% | 5 |
| `level` | `peak_ws_mb` | 0.32% | 0.28% | 5 |
| `micro_engine` | `peak_ws_mb` | 0.20% | 0.26% | 11 |
| `micro_engine` cases (298) | `ns_per_op` | median 7.4%, p90 25.6%, max 116% | median 6.7%, p90 24.3%, max 1238% | 11 |

Each per-case floor is in `floors.json`; the table summarizes them.

What sets the large ones:

- **Systematic A/A offset.** `script:script` on template_release reads the
  twin 4.3-6.9% slower in all 11 rounds: the two same-source builds differ
  in code layout, not in noise. Its 6.94% floor is that offset. On editor
  the same workload is 1.04%.
- **Two-sided spread.** `script:instance` template_release ranges from
  -14.8% to +13.0% over the rounds (editor -2.0% to +7.2%). In 02-04 (5
  rounds, loaded PC) the twin read +17.7% in every round; at the default
  load it is no longer one-sided, so it is noise, not layout.
- **Single rounds.** Most scene floors on template_release come from one
  or two rounds (`scene:instance` +10.8% in round 4, `scene:churn` +9.9%
  in round 8, with the others within 4%); the level's `main_ms_p50` on
  editor is the one clock-flagged round (+5.6%, the rest within 0.1%).
  The floor is the largest difference by definition (D-04), so one such
  round sets it.
- **gdbench cases.** Each case times one call per process. At the default
  load the median case floor is 3.1-3.8% (02-04 saw about 23% on a busy
  PC); 14-19 of 135 cases are above 10%, the widest
  `gdbench_dictionary.benchmark_iterate_string_100k` (71%, editor) and
  `string_manipulation.benchmark_find` (33%, template_release). The
  runner stays at one call per case; a candidate whose target is one of
  the wide cases needs several timed calls per case first.
- **micro_engine cases.** At n = 11 the median case floor is 6.7-7.4%
  (30% at n = 5 in 02-07 on a busy PC); about a third of the cases are
  above 10%, mostly small N (timed regions of 0.1-1 ms), where one slow
  process sets the floor. Micro cases never decide acceptance
  (Pitfall 2), so the timed regions stay as they are; a Variant-container
  candidate (kept on the table, [family shares](/project/family-shares.md))
  would lengthen them first.

# Floors per workload (GCC and Clang)

Measured in one CachyOS session on the same PC
([Linux builds](/project/linux-build.md)), `bench.py aa --workloads all
--counters` and `micro --aa` per compiler.

| Item | Value |
| --- | --- |
| Source | `7b81582fab` plus the session's local fixes (test and runner files only, committed afterwards) |
| Compilers | GCC 16.2.1 20260810 (full LTO), Clang 23.1.5 (ThinLTO, lld), held with `IgnorePkg`; bench builds (`FORK_ALLOC_COUNTERS=1`) |
| CPU | i9-14900K, bench CPUs 0xfffc (CPUs 2-15, P-cores 1-7, both threads); the runner on 0xffff0003 |
| Firmware | BIOS 1.K0, microcode 0x137 (loaded at boot; Windows runs 0x12b) |
| OS | CachyOS, kernel 7.3.5-2-cachyos |
| Power | governor `performance`, EPP `performance`, boost on (`no_turbo` 0), sched_ext disabled (`scx_loader` stopped for the series), `perf_event_paranoid` 2 |
| Load at start | bench CPUs 2.3% (GCC suite), 0.1% (Clang suite), 0.0-0.2% (micro) |
| Clocks | mean `scaling_cur_freq`: GCC suite 4950-5543 MHz, rounds 7 and 10 flagged; Clang suite 5245-5441 MHz, none flagged; micro 5475-5704 MHz, none flagged. A different measure from the Windows effective clock, which averages idle CPUs in, so the two are not compared |
| Workers | game runs 14, editor-mode runs 32, on Linux too (D-12, research A1) |
| Series | GCC suite 13.2 min planned, Clang 9.0 min; 2026-11-21 |

| Workload | Metric | GCC editor | GCC template_release | Clang editor | Clang template_release | n |
| --- | --- | --- | --- | --- | --- | --- |
| `scene:instance:10000` | `frame_ms_p50` | 9.04% | 1.33% | 7.74% | 8.31% | 11 |
| `scene:churn:10000` | `frame_ms_p50` | 1.32% | 21.55% | 8.55% | 53.45% | 11 |
| `scene:reparent:10000` | `frame_ms_p50` | 2.38% | 6.25% | 4.76% | 21.43% | 11 |
| `scene:script:10000` | `frame_ms_p50` | 3.44% | 3.44% | 10.06% | 8.16% | 11 |
| `script:nodes` | `frame_ms_p50` | 9.62% | 15.90% | 15.21% | 13.32% | 11 |
| `script:script` | `frame_ms_p50` | 5.61% | 3.00% | 3.28% | 2.20% | 11 |
| `script:instance` | `frame_ms_p50` | 9.02% | 9.10% | 9.99% | 7.22% | 11 |
| scene and script (7), `gdbench` | `peak_ws_mb` | not stored (below) | not stored | not stored | not stored | |
| `gdbench` cases (135) | `time_ms` | median 3.4%, p90 9.1%, max 53% | median 2.8%, p90 9.8%, max 24% | median 2.3%, p90 7.5%, max 45% | median 2.2%, p90 5.8%, max 38% | 5 |
| `editor_startup` | `wall_ms` | 0.16% | n/a | 7.42% | n/a | 5 |
| `editor_startup` | `peak_ws_mb` | 0.10% | n/a | 0.28% | n/a | 5 |
| `level` | `main_ms_p50` | 1.55% | 0.69% | 0.47% | 0.36% | 5 |
| `level` | `render_cpu_ms_p50` | 1.76% | 0.52% | 2.39% | 1.48% | 5 |
| `level` | `instance_ms` | 4.39% | 4.48% | 2.28% | 6.10% | 5 |
| `level` | `load_ms` | 2.12% | 1.78% | 2.71% | 1.36% | 5 |
| `level` | `peak_ws_mb` | 0.19% | 0.20% | 1.29% | 1.11% | 5 |
| `micro_engine` | `peak_ws_mb` | 0.21% | not stored | 0.31% | not stored | 11 |
| `micro_engine` cases (298) | `ns_per_op` | median 5.9%, p90 28.4%, max 315% | median 5.9%, p90 27.6%, max 83% | median 7.3%, p90 33.8%, max 345% | median 5.1%, p90 21.1%, max 251% | 11 |

What sets the large ones on Linux:

- **Single rounds on cheap frames.** Every Linux floor above 10% comes
  from one or two rounds, with the rest within a few percent:
  `scene:churn` template_release reads +21.6% (GCC) and +53.4% (Clang)
  in one round and within 6% in the other ten; Clang `scene:reparent`
  template_release -21.4% in one round; `script:nodes` 13-16% in one
  round on both compilers. `scene:churn` and `scene:reparent` frames
  take 0.12-0.15 ms and 0.03-0.04 ms, so their medians also move in
  steps of 0.7-3%. A candidate aimed at them needs a heavier variant of
  the workload, not more rounds.
- **Clang editor startup** is bimodal: two of five rounds read -7.2% and
  -7.4%, the others within 0.1% (GCC: 0.16%).
- **No layout offset on `script:script` template_release.** The MSVC
  twin reads 4.3-6.9% slower in every round; on GCC and Clang all rounds
  but one lie within 1%, and the floors (3.00%, 2.20%) are that one
  round. The editor floors are 5.61% and 3.28%, the reverse of MSVC.
- **gdbench and micro_engine cases** look like MSVC's: median case
  floors 2-4% and 5-7%, the widest cases one call or one small-N process
  (`array.benchmark_packed_*` on GCC editor, `string_manipulation` split
  and rsplit on Clang, `*_sname_insert_0` and iterate at 16-256 in
  micro_engine).

**Peak memory of the small workloads is not measured yet.** In these
series every Linux workload whose peak stayed below about 604 MiB read
exactly the runner's own peak (603.39 MiB GCC, 604.39 MiB Clang, every
round, both builds): the runner had read the 569 MB level pack into
memory to hash it, and Linux starts a child's `ru_maxrss` at the
spawning process's peak (the old address space's high-water mark is
kept across `exec`; with `vfork` that space is the runner's). The 34
affected entries (scene and script workloads and gdbench on both builds,
micro_engine on template_release, per compiler) are left out of
`floors.json`, so `compare` reports `NO-FLOOR` for them, not a verdict.
The runner now hashes in chunks and refuses a child peak that is not
above its own (`bench_linux.child_peak_bytes`); the next CachyOS session
re-measures these floors (`aa --workloads all` per compiler). Editor
startup, the level and micro_engine on editor peaked above the runner,
so their floors are real. See
[the mistake](/mistakes/child-peak-rss-inherits-the-runners.md).

## Counters (perf stat, one pass per binary after the timed rounds)

Counted on the P-cores (`cpu_core/*/u`), never judged. The time is the
control's median (frame median for scene and script, wall time for
editor startup, main-thread frame median for the level).

| Workload | Build | GCC time | GCC instructions | GCC IPC | GCC cache misses (rate) | Clang time | Clang instructions | Clang IPC | Clang cache misses (rate) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `scene:instance:10000` | editor | 4.34 ms | 12.69 G | 2.88 | 1.4 M (1.9%) | 4.33 ms | 13.31 G | 3.02 | 1.8 M (2.3%) |
| `scene:instance:10000` | template_release | 3.58 ms | 10.80 G | 2.86 | 1.2 M (1.7%) | 3.66 ms | 11.22 G | 3.02 | 1.2 M (1.8%) |
| `scene:churn:10000` | editor | 0.15 ms | 1.48 G | 2.44 | 1.3 M (18.2%) | 0.15 ms | 1.45 G | 2.43 | 1.6 M (21.3%) |
| `scene:churn:10000` | template_release | 0.12 ms | 1.25 G | 2.54 | 1.0 M (16.5%) | 0.12 ms | 1.21 G | 2.54 | 1.2 M (18.4%) |
| `scene:reparent:10000` | editor | 0.04 ms | 1.22 G | 2.44 | 1.3 M (22.9%) | 0.04 ms | 1.18 G | 2.43 | 1.5 M (24.9%) |
| `scene:reparent:10000` | template_release | 0.03 ms | 1.05 G | 2.56 | 1.0 M (21.1%) | 0.03 ms | 1.01 G | 2.54 | 1.1 M (22.6%) |
| `scene:script:10000` | editor | 0.73 ms | 4.18 G | 3.49 | 1.5 M (3.5%) | 0.73 ms | 4.47 G | 3.77 | 1.7 M (4.0%) |
| `scene:script:10000` | template_release | 0.59 ms | 3.59 G | 3.70 | 1.1 M (2.9%) | 0.62 ms | 3.73 G | 3.80 | 1.2 M (3.4%) |
| `script:nodes` | editor | 1.18 ms | 4.21 G | 2.64 | 10.6 M (10.2%) | 1.22 ms | 4.39 G | 2.71 | 10.6 M (10.2%) |
| `script:nodes` | template_release | 1.03 ms | 3.59 G | 2.58 | 8.8 M (8.8%) | 1.04 ms | 3.67 G | 2.66 | 8.4 M (8.7%) |
| `script:script` | editor | 1.71 ms | 5.68 G | 2.78 | 1.3 M (4.6%) | 1.71 ms | 5.68 G | 2.81 | 1.6 M (5.3%) |
| `script:script` | template_release | 1.39 ms | 4.99 G | 2.99 | 1.0 M (4.1%) | 1.34 ms | 4.85 G | 3.02 | 1.1 M (4.7%) |
| `script:instance` | editor | 2.85 ms | 7.39 G | 2.33 | 1.3 M (0.9%) | 2.91 ms | 7.61 G | 2.41 | 1.5 M (1.1%) |
| `script:instance` | template_release | 2.50 ms | 6.62 G | 2.42 | 1.0 M (0.8%) | 2.52 ms | 6.79 G | 2.48 | 1.1 M (0.9%) |
| `gdbench` | editor | per case | 111.54 G | 2.97 | 84.6 M (37.4%) | per case | 110.76 G | 3.00 | 83.9 M (37.5%) |
| `gdbench` | template_release | per case | 100.09 G | 3.15 | 73.7 M (35.9%) | per case | 97.06 G | 3.16 | 73.7 M (36.2%) |
| `editor_startup` | editor | 1188.5 ms | 11.70 G | 1.74 | 27.4 M (42.7%) | 1187.4 ms | 12.13 G | 1.80 | 27.2 M (44.4%) |
| `level` | editor | 1.95 ms | 99.64 G | 1.94 | 200.2 M (18.5%) | 1.48 ms | 56.09 G | 1.73 | 115.6 M (15.7%) |
| `level` | template_release | 1.75 ms | 102.06 G | 1.92 | 206.3 M (17.8%) | 1.40 ms | 51.31 G | 1.66 | 99.6 M (14.4%) |
| `micro_engine` | editor | per case | 24.84 G | 1.03 | 196.2 M (36.1%) | per case | 25.52 G | 1.05 | 194.4 M (35.7%) |
| `micro_engine` | template_release | per case | 23.74 G | 1.03 | 183.8 M (34.6%) | per case | 24.48 G | 1.06 | 181.2 M (34.9%) |

The A/A twin's instruction count is within 0.25% of the control's on
every workload but the level (+5.8 to +14.7% on GCC, -0.3 to +5.4% on
Clang): the level runs for a fixed time, so the pass counts whatever
frames and loading fit, not a fixed amount of work. So a counted
instruction difference between candidate and control is evidence on the
scene, script, gdbench and micro_engine workloads, not on the level. The
level's counts also differ about 2x between the compilers for a run of
the same length; why was not looked into.

## A/A allocation counts on Linux

Identical on both builds for every count of every workload, as on MSVC,
except the level's `load_allocs` (within 30 of 1.07-1.11M) and
`fly_allocs` (within 54 of 0.68-0.70M), COUNT-NONDETERMINISTIC on both
compilers; `fly_reallocs` is identical.

# Micro tier floors (standalone harness, n = 11)

| Family | Cases | Median floor | Largest case floor |
| --- | --- | --- | --- |
| `hashmap` | 138 | 5.3% | 46.9% (`hashmap_ptr_miss_16`) |
| `ahashmap` | 138 | 6.3% | 118.0% (`ahashmap_ptr_iterate_16`) |
| `hashset` | 138 | 6.6% | 107.5% (`hashset_ptr_churn_1`) |
| `rbmap` | 132 | 5.9% | 71.4% (`rbmap_str_miss_0`) |
| `rbset` | 132 | 4.8% | 31.0% (`rbset_str_miss_16`) |
| `list` | 27 | 3.7% | 29.4% (`list_i64_iterate_32768`) |
| `vector` | 40 | 3.9% | 139.7% (`vector_i64_iterate_0`) |
| `localvector` | 40 | 5.2% | 88.8% (`localvector_str_iterate_16`) |
| `string` | 2 | 5.0% | 5.4% (`string_copy_detach_16`) |
| `hash` | 6 | 4.9% | 8.3% (`hash_bytes_4096`) |
| third-party controls | 256 | 4.6-6.1% per library | 78.6% (`ctl_gtl_flat_i64_iterate_256`) |

All 1049 cases: median 5.4%. Godot rows p90 21.2%, controls p90 12.8%.

GCC 16.2.1 and Clang 23.1.5 (`g++` and `clang++ -O2`, the same flags on
every side):

| Family | Cases | GCC median | GCC largest | Clang median | Clang largest |
| --- | --- | --- | --- | --- | --- |
| `hashmap` | 138 | 6.4% | 216.6% (`hashmap_str_iterate_32768`) | 6.9% | 247.3% (`hashmap_str_iterate_32768`) |
| `ahashmap` | 138 | 6.1% | 160.6% (`ahashmap_str_iterate_1`) | 6.3% | 81.7% (`ahashmap_i64_erase_1`) |
| `hashset` | 138 | 7.0% | 94.5% (`hashset_i64_insert_16`) | 6.2% | 94.7% (`hashset_str_erase_1`) |
| `rbmap` | 132 | 4.1% | 46.2% (`rbmap_ptr_erase_0`) | 3.9% | 80.0% (`rbmap_ptr_iterate_1`) |
| `rbset` | 132 | 4.5% | 58.1% (`rbset_i64_miss_1`) | 3.6% | 78.4% (`rbset_i64_hit_1`) |
| `list` | 27 | 3.8% | 246.4% (`list_i64_iterate_32768`) | 2.9% | 242.6% (`list_i64_iterate_32768`) |
| `vector` | 40 | 5.4% | 219.6% (`vector_str_copy_0`) | 6.0% | 69.1% (`vector_str_iterate_16`) |
| `localvector` | 40 | 7.3% | 69.4% (`localvector_str_iterate_1048576`) | 5.3% | 28.3% (`localvector_i64_push_4096`) |
| `string` | 2 | 3.1% | 3.6% | 3.5% | 4.6% |
| `hash` | 6 | 2.8% | 9.4% (`hash_bytes_65536`) | 0.8% | 2.4% (`hash_bytes_32`) |
| third-party controls | 256 | 6.7% | 57.3% (`ctl_unordered_dense_str_insert_1048576`) | 6.5% | 65.2% (`ctl_boost_flat_str_insert_1048576`) |

All cases: median 5.8% (GCC) and 5.7% (Clang); Godot rows p90 25.7% and
21.7%, controls p90 14.5% and 29.7%. 34 (GCC) and 39 (Clang) cases are
above 50%, the same kinds on both compilers: `iterate` at 4096 and
32768, which is bimodal between processes, and `erase` and `copy` at
small sizes.

**Layout sentinel at the default load.** A same-source compare
(`micro --prev HEAD`, the candidate the same source in its own build
directory) against these floors read 9 of 1049 cases as REGRESSION
(`list_i64_copy` at 16, 256 and 4096, `rbmap`/`rbset` `churn`, two
`ahashmap_i64` cases) and the sentinel fired with 2 of 256 control rows
moved. The regressions cluster by case at several sizes, which points at
layout between separately built binaries rather than random noise. So one
moved control row is not yet evidence of a layout run: until the false
alarm rate is measured over more same-source runs, read a sentinel with a
few moved rows as "same-source level" and look at which rows moved.

# Allocation counts in the A/A run

| Workload | Counts | A/A |
| --- | --- | --- |
| `scene:*` (4), `script:*` (3) | `allocs`, `reallocs` | identical on both builds |
| `gdbench` | `allocs`, `reallocs` per case (270) | identical on both builds |
| `micro_engine` | `allocs` per case (298) | identical on both builds |
| `level` | `load_allocs`, `fly_allocs` | COUNT-NONDETERMINISTIC: load within 30 of 1.09-1.12M, fly within 8 of 0.69M |
| `level` | `fly_reallocs` | identical |
| `editor_startup` | none | |

Counts that vary between same-source runs are reported, never judged
(D-06).
