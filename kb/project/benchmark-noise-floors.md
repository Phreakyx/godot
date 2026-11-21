---
type: Finding
title: A/A noise floors on the i9-14900K (MSVC)
description: The largest paired difference between two same-source builds for every suite workload, metric and build type, and for every micro case, on MSVC; scene and script frame floors 1-15%, level 0.3-8%, gdbench cases median 3-4%, micro cases median 5-7%; GCC and Clang pending (02-12).
tags: [benchmark, noise-floor, msvc, i9-14900K, phase-2]
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
`compare` reads them from `devtools/bench/floors.json` (1959 entries keyed
`workload|case|metric|build|compiler`) and refuses a stored floor whose
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
| Linux (GCC, Clang) | all | pending (02-12) | pending (02-12) | |

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
  candidate (skipped for now, [family shares](/project/family-shares.md))
  would lengthen them first.

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
