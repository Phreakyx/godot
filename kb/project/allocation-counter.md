---
type: Finding
title: Allocation counts in release builds
description: An opt-in counter in core/os/memory.cpp, compiled into every benchmark build and no shipped build, gives editor and template_release real allocation and reallocation counts through the custom monitors fork/alloc_count and fork/realloc_count; on an A/A run the counts of scene instancing and churn were identical across both binaries and all rounds, on editor and template_release.
tags: [core, memory, benchmark, testing, phase-2]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: counter
    resource: ../../core/os/fork_alloc_counters.cpp
    title: Per-thread counter slots (header core/os/fork_alloc_counters.h)
  - id: memory
    resource: ../../core/os/memory.cpp
    title: The three counting calls
  - id: monitors
    resource: ../../main/performance.cpp
    title: The two custom monitors
  - id: workload
    resource: ../../devtools/bench/workloads/scene.gd
    title: The bracket around the measured frames
  - id: runner
    resource: ../../devtools/bench/bench.py
    title: COUNTS lines and the count rule
---

# Why

The regression rule (D-06) flags any increase in allocation counts, and
`template_release` is judged next to the editor. `Memory::get_mem_usage()`
only counts with `DEBUG_ENABLED`, so it reads 0 in `template_release`, and
it counts bytes, not calls. Release builds needed a counter of their own,
without changing that accounting or any public API.

# What is counted, and where

* `FORK_ALLOC_COUNTERS` (`core/os/fork_alloc_counters.h`) defaults to 0
  under `#ifndef`, with a `static_assert` that it is 0 or 1. With 0 the
  hunks in `memory.cpp` and `performance.cpp` compile to nothing: shipped
  builds are unchanged.
* `devtools/build/build.ps1 -Bench` always passes
  `cppdefines=FORK_ALLOC_COUNTERS=1` (before any `-Defines`), so control,
  candidate, previous and A/A twin carry the same counter cost.
* One allocation per `Memory::alloc_static` (after the null check) and
  `alloc_aligned_static` (after `malloc` succeeds); one reallocation per
  `realloc_static` on an existing block with a size above 0 (a null block
  goes through `alloc_static` and counts there). Frees are not counted.
  `realloc_aligned_static` allocates a new block, so it counts as an
  allocation.
* 64 slots of two `std::atomic<uint64_t>`, each slot `alignas(64)`, both
  `constinit` so allocations during static initialization count too. A
  thread takes its slot once (`thread_local`, `fetch_add % 64` on a
  constant-initialized counter) and increments it with relaxed
  `fetch_add`; threads beyond 64 share slots, which stays correct. The
  totals sum all slots with relaxed loads.
* `Performance::Performance()` registers the custom monitors
  `fork/alloc_count` and `fork/realloc_count` (`add_custom_monitor` with
  `callable_mp_static`). No class, singleton or `Performance` enum entry is
  added; scripts call `Performance.get_custom_monitor(&"fork/alloc_count")`.

# The bracket in the scene workload

`scene.gd` checks `Performance.has_custom_monitor(&"fork/alloc_count")`
once. At the start of the first measured frame it reads both monitors twice
back to back (the difference is the cost of one read pair), and again after
the last measured frame, and prints `allocs=` and `reallocs=` as
`after - before - read cost` on the BENCH line. Without the monitor neither
field is printed, and the runner reads that binary as `n/a`, never 0.

# A/A determinism

Run `verify-03` (`bench.py aa --skip-build --workloads
scene:instance:10000,scene:churn:10000 --builds editor,template_release
--rounds 5 --max-load 50`): source `5cc9b65865` plus this change
(uncommitted at run time), MSVC 14.51, i9-14900K pinned to P-cores 1-7
(`0xfffc`), effective clock 5.2-5.3 GHz, 120 measured frames after 30
warm-up frames. Bench-CPU load was 23.6% (everyday apps open), so the run
used `--max-load 50`: its time floors are smoke values only, not floors.
Counts do not depend on load, and that is what this run checks.

| Workload | Count | editor | template_release | A/A |
| --- | --- | --- | --- | --- |
| `scene:instance:10000` | allocs | 4,961,487 | 4,961,727 | identical (control, twin, 5 rounds) |
| `scene:instance:10000` | reallocs | 245,775 | 245,775 | identical |
| `scene:churn:10000` | allocs | 96,924 | 97,164 | identical |
| `scene:churn:10000` | reallocs | 46 | 46 | identical |

Every one of the 8 rows printed `COUNTS ... identical`: the counts are
deterministic, so D-06 can flag any increase. A 60-frame instancing run of
the first counter build gave 2,501,367 allocations three times in a row.
`template_release` makes 240 more allocations than the editor in both
workloads (2 per measured frame); the rows are kept apart, never pooled.

A `compare` smoke run (`scene:churn:10000`, editor, candidate
`FORK_BACKEND_SELFTEST=1`, previous `bb24afbe44` from before the counter)
printed `previous n/a, candidate 96924, control 96924` and
`VERDICT ... allocs editor: NO-INCREASE`.

The read cost subtracted by the bracket is the GDScript call of
`get_custom_monitor` and its `Variant` result; it is the same in every
binary, so it would cancel in a comparison anyway.

# Runner rules

* Counts are their own class (`COUNT_METRICS`), never judged by the time
  or memory rules.
* `aa` prints `COUNTS <workload> <count> <build>: identical` when both
  binaries and all rounds agree, else `COUNT-NONDETERMINISTIC (min..max)`.
* `compare` prints the range of each binary and `VERDICT ... ALLOC-INCREASE`
  or `NO-INCREASE` (candidate against the control), `COUNT-NONDETERMINISTIC`
  when the control or the candidate varies across rounds, `n/a` when either
  has no counter. Rows go to `counts.csv`. See the
  [benchmark protocol](/tooling/benchmark-protocol.md).
