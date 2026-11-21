---
type: Finding
title: Container family shares and build-or-skip decisions (i9-14900K, MSVC)
description: Each container family's share of every suite workload on editor and template_release, its best plausible speedup, max gain against the target workload's A/A floor and the decision; build allocation (25.9% of script:script), hash tables (14.9% of scene:reparent) and CowData (8.7% of gdbench); skip Variant containers, formatting and trees/lists; all provisional until GCC and Clang (02-12).
tags: [benchmark, profiling, containers, allocation, hashing, msvc, i9-14900K, phase-2]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T03:00:00Z }
sources:
  - id: floors
    resource: benchmark-noise-floors.md
    title: A/A noise floors on the i9-14900K (MSVC)
  - id: protocol
    resource: ../tooling/benchmark-protocol.md
    title: Benchmark protocol (Shares, Skip rule)
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
[floors](/project/benchmark-noise-floors.md)' source, same conditions
(MSVC 14.51.36231, BIOS 1.K0, microcode 0x12b, bench CPUs 0xfffc, default
`--max-load 10`, PC idle), 1 ms sampling, frames workloads 15 s after 3 s
of warm-up. `analyze.py families` gives each busy sample to one family by
its innermost frame; the share is the larger of the busiest-thread and
all-busy-samples shares (equal on the single-threaded workloads, within
0.4 points on the level, whose work is split over the main and render
threads, and 1.3 points on editor startup, which loads on workers). Output:
`devtools/out/bench/profile/shares-msvc/` (gitignored).

**Skip rule (D-07).** max gain = share x (1 - 1/speedup), with the share of
the family's **target workload**, the workload where its share is largest;
`skip` when the max gain is strictly below that workload's A/A floor for
the same build type (`skip_decision` in `bench.py`). `micro_engine` is
never a target: it is the container benchmark itself (Variant containers
45-47% of it by construction). Every decision is **provisional until the
GCC and Clang numbers (02-12)**.

# Shares per workload (MSVC, % of busy samples)

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
| Linux (GCC, Clang) | | pending (02-12) | | | | | | | |

**The bench counter** (`core/os/fork_alloc_counters`, in bench builds
only) is its own excluded row: 0.1-3.0%. It also slows the allocation
path itself (string copy/detach 7-8% slower with `FORK_ALLOC_COUNTERS=1`,
[migration record](/project/benchmark-driver-migration.md)), so
allocation shares from bench builds are high by up to about the counter's
row; the allocation decision holds with that taken off (below).

# Decisions

| Family | Target workload (largest share) | Share | Best plausible speedup | Max gain | Target floor | Decision |
| --- | --- | --- | --- | --- | --- | --- |
| allocation | `script:script` template_release | 25.9% | 2x (assumed; see below) | 13.0% | 6.94% | **build** (provisional) |
| hash tables | `scene:reparent:10000` editor | 14.9% | 2.57x (micro) | 9.1% | 3.13% | **build** (provisional) |
| Variant containers | `script:script` template_release | 3.8% | 7.22x (micro, upper bound) | 3.3% | 6.94% | **skip** (provisional) |
| CowData | `gdbench` template_release | 8.7% | 2x (assumed; see below) | 4.4% | 3.08% (median case) | **build** (provisional) |
| formatting | `script:script` template_release | 1.4% | any (3x shown) | 0.9% | 6.94% | **skip** (provisional) |
| trees/lists | `scene:instance:10000` template_release | 0.9% | 1.47x (micro) | 0.3% | 10.80% | **skip** (provisional) |

## Allocation

Largest share: `script:script` template_release 25.9% (editor 23.6%);
the scene and script workloads are 15-26% except `scene:reparent` (5-6%),
`script:nodes` (7-8%) and the level (1.4%). Almost all of it is the Windows
heap (`RtlAllocateHeap` 15.5%, `RtlFreeHeap` 8.6% of the busiest thread);
its largest caller is the String `%` operator
(`OperatorEvaluatorStringFormat::do_mod`, 12.5 points), then `Array`
construction, destruction and resize (8.7 points).

Best plausible speedup: the suite has no allocator control. A faster
general-purpose allocator (mimalloc[^mimalloc] is the published
reference; its report compares it with system and other allocators on
allocation-heavy benchmarks) is the candidate; the report gives no factor
for the Windows heap on this workload, so **2x on the allocator's own time
is an assumption**, not a measured number. The decision does not hinge on
it: the max gain reaches the 6.94% floor from a speedup of 1.37x on, and
with the bench counter's 3.0% taken off the share (22.9%) from 1.43x on.
On editor the floor is 1.04%. Decision: **build**, provisional.

## Hash tables

Largest share: `scene:reparent:10000` editor 14.9% (template_release
9.2%); 10-12% on `scene:instance`, `scene:churn` and `script:instance`.
Nearly all lookups (`AHashMap::_lookup_idx_with_hash` 5.1%,
`HashMap::_lookup_idx_unchecked` 4.1%), from signal emission
(`Object::emit_signalp`, 7.5 points) and `Node::remove_child`,
`_validate_child_name` and `_add_child_nocheck` (6.6 points).

Best plausible speedup: **2.57x**, the micro ratio of `hashmap_i64_hit_4096`
to the fastest control (`boost::unordered_flat_map`), n = 11; engine keys
are mostly StringName and pointers, hashed like integers. With String keys
the ratio is 1.36-1.75x (`hashmap_str_*` at 4096 and 32768), which still
gives 5.0% at 1.51x. Max gain 9.1% against the 3.13% floor: **build**,
provisional. On template_release the largest share is `script:instance`
11.0% against a 14.82% floor (6.7% max gain): skip there, so the
candidate is judged on editor first and needs the Linux floors to count
on release.

## Variant containers

Largest share outside `micro_engine`: `script:script` template_release
3.8% (editor 3.6%, `scene:churn` 3.4-3.6%, `gdbench` 2.3-2.5%).

Best plausible speedup: **7.22x**, `micro_engine` `dict_i64_hit_4096`
(18.0 ns, template_release) against the fastest int-key control in the
micro tier (`ctl_boost_flat_i64_hit_4096`, 2.5 ns). That is an upper
bound: a Dictionary must still hash and compare Variants
(`HashMap<Variant, Variant>` hits take 20 ns, about as long). Max gain
3.3% against 6.94%: **skip**, provisional. This is the most fragile
decision: on editor the same workload has a 1.04% floor and the max gain
is 3.1%, and the 6.94% release floor is the systematic layout offset of
the A/A twin ([floors](/project/benchmark-noise-floors.md)). If the Linux
release floors of `script:script` are near the editor's, it flips to
build.

## CowData

Largest share: `gdbench` template_release 8.7% (editor 8.6%), then
`script:script` 7.6-8.0%. `CowData::size`, `_insert_uninitialized`,
`_alloc_exact`, `_ref`/`_unref` from bound method calls and the String
`%` operator.

Best plausible speedup: no control and no published number fits (the
cost is reference counts, size reads and copy-on-write checks), so **2x
is an assumption**. Max gain 4.4% against the median gdbench case floor
3.08%; the gain reaches the floor from 1.55x on. Decision: **build**,
provisional, and the weakest of the three builds: the gdbench case floors
spread to 10% at p90, so the candidate needs a target case with a narrow
floor.

## Formatting

Largest share: `script:script` template_release 1.4% (editor 1.1%),
`gdbench` 0.6-0.7%, elsewhere 0-0.3%. The max gain is below the share
whatever the speedup, and the share is below the 6.94% floor, so no
published factor (Ryu[^ryu] for floats, say) can change it: **skip**,
provisional. The cost of formatting is in its allocations instead (the
`%` operator is 12.5 points of the allocation family above); a fix there
is an allocation candidate.

## Trees/lists

Largest share: `scene:instance:10000` template_release 0.9%. RBMap and
RBSet are within 0.9-1.5x of the fastest tree control (`std::map`,
`gtl::btree`) in the micro tier (`rbmap_str_insert_256` 1.47x the best).
Max gain 0.3% against 10.80%: **skip**, provisional.

# Against the Ryzen hypothesis and the near-noise concern

The [Ryzen profiles](/project/core-cpu-profiles.md) (other hardware,
editor builds, different workloads; hypotheses only) put hash tables at
3-16% with lookups dominating, the Dictionary map at 3.5% and `sprintf`
at 3.6% of the script workload. Here hash tables are 1-15%, also mostly
lookups; Dictionary (Variant containers) is 3.6-3.8% of `script:script`,
close to the Ryzen value and below its release floor; formatting is 1.1-1.4%,
below every floor of its workload except the editor's 1.04%, so the
concern that both sit near the noise holds for formatting and, on release,
for Dictionary. Allocation was not a family in the Ryzen table (heap calls
were 14% of instancing there; 19-21% here).

# Editor startup: do Theme lookups still dominate?

No. Profiled with the measured command (`--headless --editor --quit`, 2037
busy samples, three runs), hash tables are 7.2-8.8% of startup and hash
lookups 5.1% of the busiest thread. Theme callers (`has_icon`,
`has_color`, `has_constant`, `has_stylebox`, `get_type_variation_base`,
`EditorTheme::get_stylebox`, `get_constant`) account for about 1.9 points
of those, the largest caller group, ahead of signals (0.9 points) and the
text server. On the Ryzen, hash tables were 10.9% of startup, "mostly
Theme lookups". Allocation is the largest family at startup (14-16%).
The first profile of this plan ran `--quit-after 3000`, whose frames after
startup are mostly `OS_Windows::add_frame_delay`; `profile` now uses the
measured command.

[^mimalloc]: Leijen, Zorn, de Moura, "Mimalloc: Free List Sharding in Action", APLAS 2019 (MSR-TR-2019-18).
[^ryu]: Adams, "Ryu: fast float-to-string conversion", PLDI 2018, doi:10.1145/3192366.3192369.
