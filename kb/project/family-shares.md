---
type: Finding
title: Container family shares and build-or-skip decisions (i9-14900K, MSVC, GCC, Clang)
description: Each container family's share of every suite workload on editor and template_release with MSVC, GCC and Clang, its best plausible speedup, max gain against the target workload's A/A floor and the final decision; build allocation (34.0% of scene:instance on GCC), hash tables (15.7% of scene:reparent on GCC) and CowData (23.2% of script:script on Clang); Variant containers kept on the table (build) by the maintainer against the rule's skip, on two Linux workloads that clear their floors; skip formatting and trees/lists.
tags: [benchmark, profiling, containers, allocation, hashing, msvc, gcc, clang, linux, i9-14900K, phase-2]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T22:00:00Z }
sources:
  - id: floors
    resource: benchmark-noise-floors.md
    title: A/A noise floors on the i9-14900K (MSVC, GCC, Clang)
  - id: protocol
    resource: ../tooling/benchmark-protocol.md
    title: Benchmark protocol (Shares, Skip rule, Linux)
  - id: mimalloc
    resource: https://www.microsoft.com/en-us/research/publication/mimalloc-free-list-sharding-in-action/
    title: "Mimalloc: Free List Sharding in Action (Leijen, Zorn, de Moura; APLAS 2019, MSR-TR-2019-18)"
  - id: ryu
    resource: https://doi.org/10.1145/3192366.3192369
    title: "Ryu: fast float-to-string conversion (Adams; PLDI 2018)"
---

# How

`bench.py profile` on every suite workload, editor and template_release
(editor startup editor only), bench control builds of the
[floors](/project/benchmark-noise-floors.md)' source, under the floors'
conditions, frames workloads 15 s after 3 s of warm-up:

* **MSVC** (14.51.36231, microcode 0x12b): `sampler.exe` at 1 ms. Output
  `devtools/out/bench/profile/shares-msvc/` (gitignored).
* **GCC 16.2.1 and Clang 23.1.5** (CachyOS, kernel 7.3.5, microcode
  0x137): `perf record -e cpu_core/cycles/u -F 997 -g --user-callchains`
  (about 1 ms), `perf script` through `perf_to_tsv.py`. The family files
  were made again on Windows from the copied TSVs with two fixes to
  `analyze.py` (below); the MSVC files come out the same with them.

`analyze.py families` gives each busy sample to one family by its
innermost frame; the share is the larger of the busiest-thread and
all-busy-samples shares (equal on the single-threaded workloads, within
0.4 points on the level, whose work is split over the main and render
threads, and 1.3 points on editor startup, which loads on workers).

**Two Linux attribution fixes (02-12).** perf names glibc's internal
allocator frames `[unknown]` but gives their source line
(`malloc/malloc.c`), so up to 27 points per workload went to "other"; the
allocation rule now also takes glibc's `malloc/malloc.c` and
`malloc/arena.c` by file. Only allocation moved (0 to +27.3 points), no
other family. And GCC and Clang name translation units relative to the
repository (`core/os/memory.cpp`), which the directory summary filed as
"godot (no line)" (about 30% of every Linux profile); the families had
always read those paths. What is left in "godot (no line)" (about 7%)
is inlined libstdc++ code (`std::atomic`).

**Skip rule (D-07).** max gain = share x (1 - 1/speedup), with the share of
the family's **target workload**, the workload, build type and compiler
where its share is largest; `skip` when the max gain is strictly below
that workload's A/A floor for the same compiler and build type
(`skip_decision` in `bench.py`). `micro_engine` is never a target: it is
the container benchmark itself (Variant containers 45-56% of it by
construction).

# Shares per workload (% of busy samples)

## MSVC

| Workload | Build | Busy samples | allocation | hash tables | Variant containers | CowData | formatting | trees/lists | bench counter |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `scene:instance:10000` | editor | 11699 | 18.8 | 12.1 | 1.4 | 1.6 | 0.0 | 0.8 | 2.3 |
| `scene:instance:10000` | template_release | 11723 | 21.1 | 6.6 | 1.3 | 1.4 | 0.0 | 0.9 | 2.6 |
| `scene:churn:10000` | editor | 11743 | 14.8 | 10.2 | 3.6 | 3.3 | 0.2 | 0.5 | 1.1 |
| `scene:churn:10000` | template_release | 11770 | 16.5 | 6.6 | 3.4 | 3.4 | 0.3 | 0.6 | 1.3 |
| `scene:reparent:10000` | editor | 11714 | 4.9 | 14.9 | 1.8 | 0.8 | 0.0 | 0.0 | 0.6 |
| `scene:reparent:10000` | template_release | 11781 | 6.1 | 9.2 | 1.7 | 0.5 | 0.0 | 0.0 | 0.7 |
| `scene:script:10000` | editor | 11750 | 21.0 | 7.0 | 0.3 | 0.6 | 0.0 | 0.0 | 1.7 |
| `scene:script:10000` | template_release | 11773 | 23.2 | 9.1 | 0.3 | 0.0 | 0.0 | 0.0 | 2.2 |
| `script:nodes` | editor | 11653 | 7.2 | 7.5 | 1.5 | 1.7 | 0.0 | 0.1 | 0.4 |
| `script:nodes` | template_release | 11709 | 7.6 | 6.6 | 1.6 | 1.3 | 0.0 | 0.1 | 0.7 |
| `script:script` | editor | 11753 | 23.6 | 4.7 | 3.6 | 7.6 | 1.1 | 0.0 | 2.5 |
| `script:script` | template_release | 11793 | 25.9 | 4.7 | 3.8 | 8.0 | 1.4 | 0.0 | 3.0 |
| `script:instance` | editor | 11759 | 21.7 | 12.2 | 1.5 | 1.5 | 0.0 | 0.6 | 2.7 |
| `script:instance` | template_release | 11793 | 23.0 | 11.0 | 1.5 | 1.2 | 0.0 | 0.6 | 2.8 |
| `gdbench` | editor | 10363 | 20.6 | 4.2 | 2.3 | 8.6 | 0.6 | 0.2 | 2.7 |
| `gdbench` | template_release | 9292 | 22.6 | 4.1 | 2.5 | 8.7 | 0.7 | 0.3 | 2.7 |
| `editor_startup` | editor | 2037 | 14.9 | 7.6 | 2.0 | 4.2 | 0.0 | 0.5 | 1.0 |
| `level` | editor | 3093 | 1.4 | 0.8 | 0.1 | 0.9 | 0.0 | 0.0 | 0.1 |
| `level` | template_release | 3653 | 1.3 | 1.5 | 0.1 | 1.1 | 0.0 | 0.0 | 0.1 |
| `micro_engine` | editor | 5864 | 11.2 | 10.7 | 44.6 | 2.7 | 0.0 | 0.0 | 1.4 |
| `micro_engine` | template_release | 5542 | 12.4 | 11.3 | 46.6 | 3.1 | 0.0 | 0.0 | 1.4 |

## GCC

| Workload | Build | Busy samples | allocation | hash tables | Variant containers | CowData | formatting | trees/lists | bench counter |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `scene:instance:10000` | editor | 14949 | 28.2 | 9.5 | 4.8 | 1.4 | 0.0 | 0.8 | 3.1 |
| `scene:instance:10000` | template_release | 14948 | 34.0 | 4.1 | 5.5 | 1.4 | 0.0 | 0.9 | 0.0 |
| `scene:churn:10000` | editor | 14941 | 4.9 | 10.2 | 7.6 | 6.9 | 0.3 | 0.1 | 2.0 |
| `scene:churn:10000` | template_release | 14938 | 3.1 | 8.5 | 9.0 | 7.7 | 0.2 | 0.2 | 0.1 |
| `scene:reparent:10000` | editor | 14944 | 1.3 | 15.7 | 5.2 | 1.0 | 0.0 | 0.0 | 0.8 |
| `scene:reparent:10000` | template_release | 14940 | 1.8 | 8.9 | 6.1 | 1.5 | 0.0 | 0.0 | 0.1 |
| `scene:script:10000` | editor | 14957 | 7.3 | 7.8 | 0.5 | 0.4 | 0.0 | 0.0 | 5.1 |
| `scene:script:10000` | template_release | 14955 | 8.9 | 7.7 | 0.5 | 0.5 | 0.0 | 0.0 | 0.2 |
| `script:nodes` | editor | 14948 | 7.9 | 5.2 | 2.3 | 2.8 | 0.0 | 0.1 | 1.0 |
| `script:nodes` | template_release | 14963 | 8.8 | 5.2 | 2.2 | 1.8 | 0.0 | 0.0 | 0.0 |
| `script:script` | editor | 14942 | 15.5 | 3.5 | 3.1 | 17.7 | 1.3 | 0.0 | 5.2 |
| `script:script` | template_release | 14947 | 19.5 | 4.2 | 3.5 | 20.6 | 1.9 | 0.0 | 0.1 |
| `script:instance` | editor | 14965 | 26.4 | 9.5 | 4.1 | 1.5 | 0.0 | 0.4 | 3.8 |
| `script:instance` | template_release | 14945 | 29.7 | 9.0 | 5.0 | 1.2 | 0.0 | 0.6 | 0.0 |
| `gdbench` | editor | 6328 | 15.8 | 3.0 | 5.7 | 14.1 | 0.6 | 0.4 | 3.7 |
| `gdbench` | template_release | 5246 | 17.8 | 3.6 | 5.6 | 16.7 | 0.9 | 0.2 | 0.1 |
| `editor_startup` | editor | 1370 | 18.5 | 8.0 | 6.8 | 6.1 | 0.1 | 0.3 | 2.6 |
| `level` | editor | 25740 | 1.1 | 1.4 | 0.1 | 1.6 | 0.0 | 0.0 | 0.1 |
| `level` | template_release | 25086 | 1.3 | 1.5 | 0.2 | 1.3 | 0.0 | 0.1 | 0.0 |
| `micro_engine` | editor | 4266 | 14.4 | 9.3 | 54.2 | 5.2 | 0.0 | 0.0 | 1.8 |
| `micro_engine` | template_release | 3951 | 14.2 | 11.3 | 55.7 | 4.5 | 0.0 | 0.0 | 0.0 |

## Clang

| Workload | Build | Busy samples | allocation | hash tables | Variant containers | CowData | formatting | trees/lists | bench counter |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `scene:instance:10000` | editor | 14948 | 30.5 | 9.0 | 5.0 | 1.0 | 0.0 | 0.5 | 0.0 |
| `scene:instance:10000` | template_release | 14930 | 29.9 | 5.7 | 6.6 | 0.7 | 0.0 | 1.3 | 0.0 |
| `scene:churn:10000` | editor | 14949 | 6.7 | 9.7 | 8.0 | 5.9 | 0.2 | 0.1 | 0.0 |
| `scene:churn:10000` | template_release | 14943 | 2.2 | 7.3 | 10.5 | 6.4 | 0.3 | 0.1 | 0.0 |
| `scene:reparent:10000` | editor | 14941 | 2.1 | 15.4 | 6.4 | 0.8 | 0.0 | 0.0 | 0.1 |
| `scene:reparent:10000` | template_release | 14946 | 0.4 | 9.6 | 8.1 | 1.2 | 0.0 | 0.0 | 0.1 |
| `scene:script:10000` | editor | 14948 | 6.1 | 10.5 | 1.7 | 0.6 | 0.0 | 0.0 | 0.0 |
| `scene:script:10000` | template_release | 14945 | 8.1 | 4.9 | 2.1 | 1.7 | 0.0 | 0.0 | 0.0 |
| `script:nodes` | editor | 14952 | 8.2 | 5.4 | 2.8 | 2.3 | 0.0 | 0.1 | 0.0 |
| `script:nodes` | template_release | 14950 | 7.7 | 4.6 | 3.3 | 1.8 | 0.0 | 0.2 | 0.0 |
| `script:script` | editor | 14949 | 17.4 | 3.8 | 3.0 | 16.5 | 1.4 | 0.0 | 0.1 |
| `script:script` | template_release | 14957 | 12.1 | 4.3 | 3.8 | 23.2 | 1.7 | 0.0 | 0.0 |
| `script:instance` | editor | 14942 | 28.0 | 9.4 | 4.2 | 1.8 | 0.0 | 0.4 | 0.0 |
| `script:instance` | template_release | 14943 | 25.0 | 10.0 | 5.4 | 1.8 | 0.0 | 0.9 | 0.0 |
| `gdbench` | editor | 6165 | 17.8 | 3.8 | 6.1 | 13.3 | 0.7 | 0.4 | 0.1 |
| `gdbench` | template_release | 5116 | 13.7 | 4.7 | 7.1 | 17.1 | 0.9 | 0.5 | 0.1 |
| `editor_startup` | editor | 1364 | 19.8 | 8.8 | 6.9 | 6.7 | 0.1 | 0.7 | 0.0 |
| `level` | editor | 32079 | 1.6 | 1.5 | 0.7 | 1.8 | 0.0 | 0.0 | 0.0 |
| `level` | template_release | 31449 | 1.5 | 1.5 | 0.5 | 1.3 | 0.0 | 0.0 | 0.0 |
| `micro_engine` | editor | 4234 | 15.2 | 9.6 | 53.1 | 5.5 | 0.0 | 0.0 | 0.0 |
| `micro_engine` | template_release | 4010 | 12.3 | 11.5 | 55.4 | 5.9 | 0.0 | 0.0 | 0.0 |

**The bench counter** (`core/os/fork_alloc_counters`, in bench builds
only) is its own excluded row: 0.1-3.0% on MSVC, 0.1-5.2% on GCC editor
builds. On Clang and on GCC template_release it is near 0: with LTO the
counter is inlined and its samples can't be told apart from its callers'
(on Clang `scene:instance` editor only 1.0% of samples have a counter
frame at all), so they count in other families, most likely allocation.
It also slows the allocation path itself (string copy/detach 7-8% slower
with `FORK_ALLOC_COUNTERS=1`,
[migration record](/project/benchmark-driver-migration.md)), so
allocation shares from bench builds are high by up to about the
counter's row; the allocation decision holds with that taken off
(below).

**Allocation and CowData split differently on Linux.** On `script:script`
and gdbench the Linux CowData shares are about twice MSVC's (16-23% of
`script:script` against 8%) and allocation is lower there, while the two
together are close (MSVC 33.9%, GCC 40.1%, Clang 35.3% of `script:script`
template_release). Where the compilers put the boundary between the two
was not looked into; the decisions below do not depend on it.

# Decisions (final)

| Family | Target workload (largest share) | Share | Best plausible speedup | Max gain | Target floor | Decision |
| --- | --- | --- | --- | --- | --- | --- |
| allocation | `scene:instance:10000` template_release, GCC | 34.0% | 2x (assumed; see below) | 17.0% | 1.33% | **build** |
| hash tables | `scene:reparent:10000` editor, GCC | 15.7% | 3.61x (GCC micro) | 11.4% | 2.38% | **build** |
| Variant containers | `scene:churn:10000` template_release, Clang | 10.5% | 6.93x (Clang micro, upper bound) | 9.0% | 53.45% | rule: skip; **build** (kept on the table, see below) |
| CowData | `script:script` template_release, Clang | 23.2% | 2x (assumed; see below) | 11.6% | 2.20% | **build** |
| formatting | `script:script` template_release, GCC | 1.9% | any (3x shown) | 1.3% | 3.00% | **skip** |
| trees/lists | `scene:instance:10000` template_release, Clang | 1.3% | 1.47x (micro) | 0.4% | 8.31% | **skip** |

**Against the MSVC-only decisions (02-10).** The rule changes no
decision; the maintainer overrode one, keeping Variant containers on the
table on the Linux results (below). Every family's largest share is now a
Linux one, so the targets moved:
allocation from MSVC `script:script` template_release (25.9%) to GCC
`scene:instance:10000` template_release (34.0%; glibc's allocator is a
larger share than the Windows heap on instancing), hash tables from
MSVC 14.9% to GCC 15.7% on the same workload, CowData from MSVC gdbench
(8.7%) to Clang `script:script` (23.2%), Variant containers from MSVC
`script:script` (3.8%) to Clang `scene:churn` (10.5%), formatting from
1.4% to 1.9%, trees/lists from 0.9% to 1.3%. The three builds got
stronger; formatting and trees/lists stay skips.

## Allocation

Largest share: GCC `scene:instance:10000` template_release 34.0% (Clang
29.9-30.5%, MSVC 21.1%); `script:instance` 25-30% on Linux. On MSVC
almost all of it is the Windows heap (`RtlAllocateHeap`, `RtlFreeHeap`),
its largest caller the String `%` operator
(`OperatorEvaluatorStringFormat::do_mod`); on Linux most of it is
glibc's `malloc.c` (22.4 of the 28.2 points on GCC `scene:instance`
editor).

Best plausible speedup: the suite has no allocator control. A faster
general-purpose allocator (mimalloc[^mimalloc] is the published
reference; its report compares it with system and other allocators on
allocation-heavy benchmarks) is the candidate; the report gives no factor
for the Windows heap or glibc on these workloads, so **2x on the
allocator's own time is an assumption**, not a measured number. The
decision does not hinge on it: against the 1.33% floor the max gain
passes it from 1.04x on, and with the GCC editor build's 3.1% counter
row taken off the share (30.9%) still from 1.05x on. On MSVC the same
holds from 1.37x (`script:script` template_release, 6.94% floor).
Decision: **build**.

## Hash tables

Largest share: GCC `scene:reparent:10000` editor 15.7% (Clang 15.4%,
MSVC 14.9%); 9-12% on `scene:instance`, `scene:churn` and
`script:instance` everywhere. Nearly all lookups
(`AHashMap::_lookup_idx_with_hash`, `HashMap::_lookup_idx_unchecked`),
from signal emission (`Object::emit_signalp`) and `Node::remove_child`,
`_validate_child_name` and `_add_child_nocheck`.

Best plausible speedup: the micro ratio of `hashmap_i64_hit_4096` to the
fastest control, n = 11: **3.61x** on GCC (`emhash8`), 6.38x on Clang,
2.57x on MSVC (`boost::unordered_flat_map`); engine keys are mostly
StringName and pointers, hashed like integers. Clang's HashMap hit is
twice as slow as GCC's (8.25 against 4.06 ns) while the controls stay
within 15%. With String keys the ratios are 1.0-1.2x on Linux (1.36-1.75x
on MSVC), which alone would not pass the floor. Max gain 11.4% against
the 2.38% floor: **build**. On template_release the candidate can be
judged too: GCC `scene:script:10000` (7.7% share, 3.44% floor, 5.6% max
gain) or Clang `script:instance` (10.0%, 7.22%, 8.4% at Clang's ratio).

## Variant containers

Largest share outside `micro_engine`: Clang `scene:churn:10000`
template_release 10.5% (GCC 9.0%). The Linux shares are higher than
MSVC's on most workloads (`scene:instance` 4.8-6.6% against 1.3-1.4%)
and about the same on `script:script` (3.0-3.8%).

Best plausible speedup: `micro_engine` `dict_i64_hit_4096` against the
fastest int-key control (`ctl_boost_flat_i64_hit_4096`):
**6.93x** on Clang (14.1 against 2.0 ns), 8.28x on GCC, 7.22x on MSVC.
That is an upper bound: a Dictionary must still hash and compare Variants
(`HashMap<Variant, Variant>` hits take about as long). Max gain 9.0%
against the 53.45% floor of a 0.12 ms frame: by the rule, **skip**.

Decision: **build, kept on the table by the maintainer (2026-11-21)**
against the rule's skip. The rule hangs on the target it picks, and that
target's floor is one round on a 0.12 ms frame. On two other workloads
the upper-bound gain is above the floor: GCC `scene:churn:10000` editor
(7.6% share, 6.7% max gain at 8.28x, 1.32% floor) and Clang
`script:script` template_release (3.8%, 3.3% at 6.93x, 2.20%). (02-10's
own test, a Linux `script:script` release floor near the editor's
1.04%, did not pass: it is 3.00% on GCC and 2.20% on Clang.) A candidate
is judged on those two workloads, or on a heavier `scene:churn` variant
whose floor is not set by single rounds, and it must still be faster than
its control under the acceptance rule; the speedup is an upper bound, so
it can fail there.

## CowData

Largest share: Clang `script:script` template_release 23.2% (GCC 20.6%,
MSVC 8.0%), then gdbench 13-17% on Linux. `CowData::size`,
`_insert_uninitialized`, `_alloc_exact`, `_ref`/`_unref` from bound
method calls and the String `%` operator.

Best plausible speedup: no control and no published number fits (the
cost is reference counts, size reads and copy-on-write checks), so **2x
is an assumption**. Max gain 11.6% against the 2.20% floor; the gain
passes the floor from 1.10x on. Decision: **build**, no longer the
weakest of the three: on MSVC the target was a gdbench case with a 3.08%
median floor, now it is a frame workload with a narrow floor on both
Linux compilers (GCC 3.00%).

## Formatting

Largest share: GCC `script:script` template_release 1.9% (Clang 1.7%,
MSVC 1.4%), gdbench 0.6-0.9%, elsewhere 0-0.3%. The max gain is below
the share whatever the speedup, and the share is below the 3.00% floor,
so no published factor (Ryu[^ryu] for floats, say) can change it:
**skip**. The cost of formatting is in its allocations instead (the
`%` operator is the largest allocation caller on MSVC); a fix there is an
allocation candidate.

## Trees/lists

Largest share: Clang `scene:instance:10000` template_release 1.3% (GCC
and MSVC 0.9%). RBMap and RBSet are within 0.9-1.5x of the fastest tree
control (`std::map`, `gtl::btree`) in the micro tier
(`rbmap_str_insert_256` 1.47x the best on MSVC). Max gain 0.4% against
8.31% (and on GCC 0.3% against 1.33%): **skip**.

# Against the Ryzen hypothesis and the near-noise concern

The [Ryzen profiles](/project/core-cpu-profiles.md) (other hardware,
editor builds, different workloads; hypotheses only) put hash tables at
3-16% with lookups dominating, the Dictionary map at 3.5% and `sprintf`
at 3.6% of the script workload. Here hash tables are 1-16%, also mostly
lookups; Dictionary (Variant containers) is 3.0-3.8% of `script:script`
on every compiler, close to the Ryzen value and below or near its
release floors (MSVC 6.94%, GCC 3.00%, Clang 2.20%); formatting is
1.1-1.9%, below every floor of its workload except the MSVC editor's
1.04%. So the concern that both sit near the noise holds for formatting
and, on release, for Dictionary on `script:script`. Allocation was not a
family in the Ryzen table (heap calls were 14% of instancing there;
19-34% here).

# Editor startup: do Theme lookups still dominate?

No. Profiled on MSVC with the measured command (`--headless --editor
--quit`, 2037 busy samples, three runs), hash tables are 7.2-8.8% of
startup and hash lookups 5.1% of the busiest thread. Theme callers
(`has_icon`, `has_color`, `has_constant`, `has_stylebox`,
`get_type_variation_base`, `EditorTheme::get_stylebox`, `get_constant`)
account for about 1.9 points of those, the largest caller group, ahead
of signals (0.9 points) and the text server. On the Ryzen, hash tables
were 10.9% of startup, "mostly Theme lookups". Allocation is the largest
family at startup on every compiler (14.9% MSVC, 18.5% GCC, 19.8%
Clang); hash tables are 8.0-8.8% on Linux.

[^mimalloc]: Leijen, Zorn, de Moura, "Mimalloc: Free List Sharding in Action", APLAS 2019 (MSR-TR-2019-18).
[^ryu]: Adams, "Ryu: fast float-to-string conversion", PLDI 2018, doi:10.1145/3192366.3192369.
