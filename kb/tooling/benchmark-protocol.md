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
  - id: bench-readme
    resource: ../../devtools/bench/README.md
    title: How to run the runner
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

**Regression (every other workload, and peak memory everywhere, D-06).**
Time or OS peak memory regresses when the median paired difference is
slower or bigger than the floor and the candidate loses at least k of n
rounds. Allocation counts are deterministic once the A/A run shows two
same-source builds give identical counts: then **any allocation-count
increase is flagged and must be explained** in the `kb/` entry, or the
candidate is rejected. A workload whose A/A counts differ is reported as
not deterministic, with its spread.

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
  candidate is accepted without the Linux numbers.

# Budget (D-03)

At most 30 minutes of measurement per compiler (MSVC; GCC and Clang each),
builds excluded, for editor and `template_release` and every workload. The
runner times the cold round and plans the rounds per workload: processes
under 3 s get n = 11 (k = 9), the others n = 5 (k = 5). It prints the plan
(workload, seconds per process, class, n, k, processes, minutes) and the
total before round 1, and refuses a plan above `--budget` (default 30).
`--rounds` sets n for every workload (5 or more). A/A runs have two
binaries, comparisons three.

# What every run records (BENCH-11)

Each series writes `devtools/out/bench/<series>/` (gitignored; a series
started in the same second as another gets a `-2` suffix):

- `raw.csv`: every sample in run order (cold round as round 0): round,
  position, binary, build type, workload, case, metric, value, failed flag
  and reason, CPU and wall seconds, `clock_mhz`, `clock_flag`.
- `summary.csv`: per workload, metric and build type the medians, the
  median paired differences, wins, losses, the floor and the verdict.
- `meta.json`: commit, dirty flag and the sha256 of `git diff HEAD`, the
  binaries in round-1 order with their commit, `build.ps1` and SCons
  arguments and cppdefines, MSVC toolset and Visual Studio versions, CPU
  name, BIOS, microcode revision, OS build, power plan GUID and parking
  values, Game Mode, bench-CPU load, affinity mask, worker counts, clocks
  per round, the round plan, the Python version and the runner arguments.
  A value the runner can't read is written `"unavailable"`, never left out
  or written as 0. Counter paths are stripped before writing (they carry
  the host name).

Floors live in `devtools/bench/floors.json` (`aa --write-floors`), one
entry per workload, case, metric, build type and compiler with its n,
date and machine state; the human-readable copy goes in `kb/` in the same
commit.

# Commands

```
python devtools/bench/bench.py check
python devtools/bench/bench.py aa --workloads scene:instance:10000,scene:churn:10000 --write-floors
python devtools/bench/bench.py compare --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev \
    --target scene:churn:10000 --workloads scene:churn:10000,scene:instance:10000
```

Options of `aa` and `compare`: `--workloads`, `--builds` (default
`editor,template_release`), `--rounds`, `--budget`, `--floors`,
`--max-load`, `--skip-build` (reuse the binaries), `--out`. `compare` adds
`--name` (the candidate's `build.ps1 -Variant`, binary `bench.<name>`),
`--defines`, `--prev` (required) and `--target` (the workloads judged by
the accept rule; the others get the regression rule). Later plans add
their workloads and options to [the runner README](../../devtools/bench/README.md).

# Disk

Each bench variant keeps about 3 GB of objects in `bin/obj`. A previous
build lives in `devtools/out/bench/wt/<sha7>/` with its own objects; after
deleting that folder, run `git worktree prune`.

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
- [A test scene that could not quit](/mistakes/test-scene-without-hard-quit.md),
  [single-flag benchmark runs measured the defaults](/mistakes/powershell-single-arg-splat.md)
- Successes: [control build for layout effects](/successes/control-build-for-layout-effects.md),
  [actual-container A/B without checkout switches](/successes/actual-container-ab-bench.md),
  [one editor copy per variant](/successes/editor-copy-per-variant.md)
