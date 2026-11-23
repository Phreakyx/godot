---
type: Decision
title: ALLOC-02 - mimalloc against the HashMap node pool (four-way compare)
description: The rules of the ALLOC-02 head-to-head, stated before any measurement - control, mimalloc only, pool only and both, same source, editor and template_release, every suite workload plus the micro tier and the tiny-maps case; target rows per compiler (MSVC script:script, GCC and Clang scene:instance:10000, template_release), the mechanical pass rule (bench.py verdict), the count rules, the tiny-maps report and the baseline rule for every later hash candidate. Also the exploratory Linux-only override variant (FORK_BACKEND_ALLOCATOR=3), reported next to the series and never part of the baseline. MSVC (2026-11-22): alloc and both read ACCEPT on the target (-23.1%, -23.9%), pool within noise, and all three fail on REGRESSION rows (hash lookups, peak memory), so the baseline is empty: final, since an MSVC rejection is final (2026-11-23); ALLOC-02 picks neither. The override builds and passes the suite, the Fork cases and the smoke on Clang (functional only, not measured).
tags: [core, allocation, mimalloc, hashmap, pool, benchmark, decision, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T09:00:00Z }
sources:
  - id: shares
    resource: family-shares.md
    title: Container family shares and the allocation target per compiler
  - id: floors
    resource: benchmark-noise-floors.md
    title: A/A noise floors on the i9-14900K (MSVC, GCC, Clang)
  - id: protocol
    resource: ../tooling/benchmark-protocol.md
    title: Benchmark protocol (accept, regression, count and skip rules; commands)
  - id: mimalloc
    resource: mimalloc-allocator.md
    title: The allocator candidate (FORK_BACKEND_ALLOCATOR=1)
  - id: pool
    resource: hashmap-node-pool.md
    title: The pool candidate (FORK_BACKEND_HASHMAP_POOL=1)
---

# What is compared

Four builds of one source (`71b2819fec` is the previous side of every
series):

| Side | Macros | bench variant |
| --- | --- | --- |
| control | both 0 | `bench` |
| alloc | `FORK_BACKEND_ALLOCATOR=1` | `bench.alloc` |
| pool | `FORK_BACKEND_HASHMAP_POOL=1` | `bench.pool` |
| both | `FORK_BACKEND_ALLOCATOR=1 FORK_BACKEND_HASHMAP_POOL=1` | `bench.both` |

Each candidate binary and its dev tests build passed
`--test --test-case="*Fork*"` before measuring, the new
`[Fork][Backends] Allocator scope` case included: with value 1 a raw
`malloc` block is not in mimalloc's heap, so the candidate overrides nothing.

# Rules stated before measuring

Written and committed before the first ALLOC-02 series; not changed after
it starts.

**Series, per compiler.** For each name in `alloc`, `pool`, `both`:

```
python devtools/bench/bench.py compare --name <name> --defines "<macros>" --prev 71b2819fec \
    --target <target workload> --workloads all
python devtools/bench/bench.py micro --name <name> --defines "<macros>" --prev 71b2819fec
```

against the same-source control with both macros 0, on editor and
template_release. On Linux add `--compiler gcc` or `--compiler clang`.

**Target rows (D-06)**, the workload with the largest allocation share per
compiler ([shares](/project/family-shares.md)), each with its A/A floor
([floors](/project/benchmark-noise-floors.md)):

| Compiler | Target row | Floor (`frame_ms_p50`) |
| --- | --- | --- |
| MSVC | `script:script@template_release` | 6.94% |
| GCC | `scene:instance:10000@template_release` | 1.33% |
| Clang | `scene:instance:10000@template_release` | 8.31% |

Every other row of the series gets the regression rule (time and
`peak_ws_mb`).

**Pass.** A series passes when

```
python devtools/bench/bench.py verdict --series <dir> --rows <the compiler's target row>
```

prints PASS: the target row reads ACCEPT and no row reads REGRESSION. A
micro series never passes or fails a candidate alone (protocol).

**Counts.**

- The alloc series must report allocation counts identical to the control
  on every workload: `Memory::` calls are unchanged (D-03), so any
  difference is a bug to fix, not a result.
- The pool and both series must read NO-INCREASE. Any ALLOC-INCREASE is
  explained here or rejects the series.

**Tiny maps (D-05).** For the pool and both micro series, the `live_bytes`
and `allocs` of every `*_tiny_*` row are reported against the control. If
a series passes on time but raises tiny-map `live_bytes`, this entry says
so next to the decision.

**Baseline rule**, applied per compiler, then across compilers:

- A macro is in the baseline only when its single series passes.
- When both singles pass and the both series passes, the baseline is both.
- When both singles pass but both fails, the baseline is the single with
  the more negative target median.
- When neither single passes, the baseline is empty.
- On MSVC the result is provisional (D-07). The final baseline (from the
  Linux session) keeps a macro only when its series passes on MSVC, GCC and
  Clang, and applies the both rule with the worst compiler's target median.

**n and budget.** The runner's planned rounds; each series stays under the
30-minute budget.

**Noise of a baseline control.** The A/A floors were measured on the
default control. If a series shows a control whose own rounds vary clearly
more than the floor, the series is repeated once before its verdict is
used, and the spread is recorded here.

## Exploratory, outside the baseline rule (D-17)

The override variant `FORK_BACKEND_ALLOCATOR=3` makes mimalloc the
process-wide malloc (Linux only; every other compile stops at an `#error`).
It runs in the Linux session on GCC and Clang after the four-way series,
and only once its full `--test`, the Fork cases (a `malloc`, an
`operator new` and a glibc-internal `realpath` block inside mimalloc's
heap) and the smoke test pass:

```
python3 devtools/bench/bench.py compare --name override --defines "FORK_BACKEND_ALLOCATOR=3" \
    --prev 71b2819fec --target scene:instance:10000 --workloads all --compiler <gcc|clang>
```

against the same-source control. Its verdict reading and target median are
reported next to the alloc series (same control), so the difference shows
what the allocations outside `Memory::` add. Its counts must equal the
control's (`Memory::` calls unchanged, D-03). It never enters the baseline
rule, is never accepted and never becomes a default in this phase.

# Results (MSVC)

**Conditions.** Source `cf6a07f1f8` (binaries from 03-04, measured with
`--skip-build`; the runner tree carried only the LF fix of
`--write-floors`), previous `71b2819fec`. MSVC toolset 14.51.36231,
Visual Studio 18.11.0, i9-14900K, BIOS 1.K0, microcode 0x12b, Windows
build 26100.9711, power plan `fe1217fb-3f66-46bf-adc6-e48c27825f80`
(core parking off), Game Mode on, bench CPUs 0xfffc. 2026-11-22,
07:26-08:49 UTC, PC idle, Inventory and Compatibility Appraisal service
stopped ([mistake](/mistakes/windows-appraiser-busies-bench-cpus.md)).
Default `--max-load 10`; bench CPUs 2.3-3.5% busy at the start of each
series. Effective clocks 4177-4503 MHz (suite), 3707-4113 MHz (micro). No
round clock-flagged and no failed process in any of the six series; the
micro A/A (step 1) flagged rounds 0 and 7 of 12. Planned 20.5, 19.2 and
18.8 minutes (budget 30). Micro floors refreshed just before
([floors](/project/benchmark-noise-floors.md)).

**Control noise.** On the target row the control's 12 rounds span 5.0-5.9%
of its median in the three series, the previous build's 5.1-7.1%: not
clearly above the 6.94% floor, so no series was repeated.

## Verdict per series (`bench.py verdict --rows script:script@template_release`)

| Series | Target median (control to candidate) | Wins | Target | REGRESSION rows | Verdict |
| --- | --- | --- | --- | --- | --- |
| alloc | 2.315 to 1.772 ms, -23.1% | 11/11 | ACCEPT | 47 (30 time, 17 `peak_ws_mb`) | FAIL |
| pool | 2.311 to 2.357 ms, +2.4% | 2/11 | WITHIN-NOISE | 82 (69 time, 13 `peak_ws_mb`) | FAIL |
| both | 2.311 to 1.757 ms, -23.9% | 11/11 | ACCEPT | 62 (44 time, 18 `peak_ws_mb`) | FAIL |

`script:script` editor: alloc -12.1%, pool -0.6%, both -20.8%. For
information (these rows get the regression rule only):
`scene:instance:10000` template_release alloc -38.5%, pool -10.5%, both
-36.0%; level `load_ms` alloc -12.8%, both -13.3%, pool -1.0%.

## REGRESSION rows

**alloc, time (30).** Every one is a hash lookup or a call that allocates
nothing in its timed loop:

- micro_engine, editor: `hashmap_sname_hit_4096` +28.8%,
  `hashmap_sname_miss` at 1, 16, 256, 32768 and 1M (+7.7 to +23.9%),
  `ahashmap_sname_miss_16` +20.1% and `_256` +13.7%,
  `hashmap_variant_miss_4096` +12.8%, `dict_i64_hit` at 16, 256, 32768
  (+6.1 to +8.2%), `dict_sname_hit` at 16, 256, 4096 (+4.0 to +7.8%),
  `dict_sname_miss_256` +2.8%, `dict_str_hit_16` +7.9%,
  `dict_variant_hit_256` +4.6%;
- micro_engine, template_release: the `*_miss_4096` rows of
  `ahashmap_sname` +11.9%, `dict_i64` +8.3%, `dict_sname` +10.9%,
  `dict_str` +5.1%, `dict_variant` +7.0%, `hashmap_sname` +22.0%,
  `hashmap_variant` +11.5%; `sname_from_string_1` +20.3%, `_16` +9.7%;
- gdbench: `gdbench_dictionary.benchmark_iterate_string_1k` editor +26.6%,
  `benchmark_lookup_hit_string_1k` editor +18.1%,
  `signal.benchmark_emission_params_10` template_release +16.0%.

**alloc, `peak_ws_mb` (17).** gdbench +71.6% editor, +55.5%
template_release (338 to 581 MB, 319 to 497 MB); level +2.9% and +2.7%;
`scene:*` +3.0 to +4.9%; `script:instance` +2.3/+1.8%, `script:nodes`
+2.1/+3.0%, `script:script` editor +1.2%. Within noise: `script:script`
template_release -0.3%, micro_engine, `editor_startup` (+12.7%, 2 of 5
rounds lower).

**pool, time (69).**

- micro_engine: `hashmap_sname_copy_0` +43.6% editor and +50.2%
  template_release, `hashmap_variant_copy_0` +50.3/+45.5% (copying an
  empty map); `dict_*` hit and miss rows at 1 to 1M on editor (+2.3 to
  +17.8%) and the `*_miss_4096` rows on template_release (+6.7 to +10.6%);
  `dict_*_copy_0` and `dict_*_insert_0` template_release (+2.6 to +5.9%);
  `hashmap_variant_miss` at 16, 4096, 32768 and `hit_4096`
  template_release (+3.3 to +14.9%); `hashmap_sname_miss_0` +22.7%;
  `sname_from_cstr_1` +22.5%.
- gdbench, rows whose timed loops do not touch HashMap: 13 `array.*`
  cases on editor (+3.8 to +11.5%), `string_manipulation.*` (`similarity`
  +36.4% editor, `validate_node_name` +31.1% template_release, 11 more
  +2.2 to +18.8%), `string_format.benchmark_simple_constant_percent`
  +5.1%, `string_name.benchmark_create` +1.6%,
  `gdbench_dictionary.benchmark_lookup_miss_string_1k` +5.2%.
- `scene:script:10000` `frame_ms_p50` template_release +3.5%.

**pool, `peak_ws_mb` (13).** `editor_startup` +12.9%; `script:script`
+2.3/+3.2%; `script:nodes` +1.8/+2.4%; `scene:*` +1.1 to +2.1%; gdbench
template_release +0.7%.

**both, time (44).** The pool's `hashmap_*_copy_0` rows (+45.5 to
+51.6%), `dict_*` hit and miss rows (+2.3 to +44.0%),
`hashmap_sname_miss_4096` and `hashmap_variant_miss_4096`
template_release as in alloc; gdbench `array.benchmark_fill_loop`,
`packed_float32_array`, `packed_int32_array` editor (+5.7 to +7.8%),
`string_manipulation.benchmark_similarity` template_release +31.6%,
`ends_with` editor +13.1%, `nocasecmp_to` +11.8%,
`string_name.benchmark_create` +0.9%; level `main_ms_p50`
template_release +5.7% (0 of 5 rounds faster, floor 0.3%).

**both, `peak_ws_mb` (18).** gdbench +71.8/+68.9%, `editor_startup`
+20.1%, `scene:*` +4.1 to +6.8%, `script:*` +2.7 to +6.0%, level
+3.0/+3.3%.

## Counts

- **alloc:** identical to the control on every deterministic count. The
  four level rows are COUNT-NONDETERMINISTIC with ranges overlapping the
  control's and the previous build's (fly 693,189-693,205 against
  693,195-693,241), as in the A/A run. The rule holds.
- **pool and both:** 1,140 rows NO-INCREASE (allocations fall, for example
  `gdbench_dictionary.benchmark_duplicate_int_100k` 100,003 to 4 and
  level load 1.12M to 1.04M) and 26 ALLOC-INCREASE, every one a `reallocs`
  row: the `gdbench_dictionary.benchmark_insert_*` cases (0 to 5 at 100k,
  0 to 200 at 1k), `alloc.benchmark_fragmentation` 311 to 349,
  `alloc.benchmark_wide_tree` 23 to 26, `scene:instance:10000` 245,775 to
  257,875, `script:instance` 4,124 to 4,608, level `fly_reallocs` +4.
  Explained: from its third page on the pool keeps a page table, which
  `_pool_add_page()` grows with `Memory::realloc_static` at every power of
  two (`core/templates/hash_map.h`). The new reallocs replace far more
  node allocations; they do not reject the series.

## Tiny maps (D-05)

`live_bytes` and `allocs` of the untimed build pass, 10,000 live maps
(the `build` and `hit` rows read the same). alloc: every row equal to the
control.

| Case | Control live_bytes | pool, both | Control allocs | pool, both |
| --- | --- | --- | --- | --- |
| `hashmap_tiny_ptr_*_1` | 3,080,000 | 3,080,000 | 30,000 | 30,000 |
| `hashmap_tiny_ptr_*_2` | 3,400,000 | 3,720,000 (+9.4%) | 40,000 | 40,000 |
| `hashmap_tiny_ptr_*_3` | 3,720,000 | 3,720,000 | 50,000 | 40,000 (-20%) |
| `hashmap_tiny_ptr_*_8` | 5,320,000 | 7,880,000 (+48%) | 100,000 | 70,000 (-30%) |
| `hashset_tiny_ptr_*_<N>` | 4,640,000 | 4,640,000 | 10,000 | 10,000 |

Tiny-map times (micro, never a verdict): alloc builds HashMap tiny maps
46-47% faster (HashSet 21-42%), lookups 8-25% faster; pool builds -16% at
3 and 8 elements and within +8% at 1 and 2, lookups within +5%; both
like alloc (builds -47 to -54%). The pool raises tiny-map `live_bytes` at
2 and 8 elements and does not pass on time.

## Micro series (information, never a verdict)

| Series | REGRESSION rows | LAYOUT-SENTINEL |
| --- | --- | --- |
| alloc | 11: `hashset_ptr_iterate_4096` +5.2%, `vector_i64_iterate_1048576` +13.0%, nine third-party `ctl_*_insert` rows (+7.9 to +161%, `ctl_unordered_dense_i64_insert_4096` the largest) | 16 of 256 control rows moved |
| pool | 17: `hashmap_*_miss` at 0, 1, 16, 256 (+7.4 to +28.2%), `ahashmap_i64_copy` at 256 to 32768 (+7.9 to +45.7%), `rbmap`/`rbset` `str` rows at 16 and `rbset_i64_copy` (+10.5 to +24.0%), `localvector_i64_copy_4096` +14.0% | 3 of 256 |
| both | 23: the pool's `hashmap_*_miss` rows, `hashset_i64_churn_32768`, three `rb*_str_*_16` rows, `vector_i64_iterate_1048576`, 12 `ctl_*` rows | 18 of 256 |

The third-party controls allocate through the C runtime, which mimalloc
does not serve with value 1, yet their insert rows slow down whenever
mimalloc is linked (alloc and both, not pool). Not explained yet; a
question for Phase 3.1 (ALLOC-03), not a verdict.

# Decision (MSVC, provisional)

**Baseline: empty** (no macro: `FORK_BACKEND_ALLOCATOR=0`,
`FORK_BACKEND_HASHMAP_POOL=0`). Rule step: "When neither single passes,
the baseline is empty." The alloc series reads ACCEPT on its target row
but has 47 REGRESSION rows; the pool series' target row is within noise
and it has 82. The both series (target -23.9%, ACCEPT) fails too, and
under the rule a failing single stays out of the baseline whatever both
reads; the pool adds 0.8 points to the alloc target gain.

Provisional until the GCC and Clang series (D-07, 03-11); the HASH-02..07
candidates are measured on this empty baseline until then. No default
changed in `fork_backends.h`.

**Peak memory fallback (step 5) not taken.** The alloc series fails
through 30 time rows as well as its `peak_ws_mb` rows, so the purge-delay
variant (`FORK_BACKEND_ALLOCATOR=2`) was not built. The gdbench +56-72%
peak is still the case for it once the lookup regressions are understood.

# Results (GCC, Clang)

Not measured (user decision 2026-11-23). Under the baseline rule the MSVC
FAIL decides: a candidate is accepted only when it passes on every
compiler, so no Linux series can bring mimalloc or the pool back into the
baseline.

# Decision (final)

**FINAL_BASE: empty** (`FORK_BACKEND_ALLOCATOR=0`, no node pool). ALLOC-02
picks **neither**: mimalloc fails on 47 REGRESSION rows, the pool reads
within noise on its target and fails on 82. The provisional MSVC baseline
above is the final one, not reversed. The node pool was deleted in Phase 3
final (last at `e2c08be5d2`); mimalloc stays in the tree, off by default,
for Phase 3.1 (ALLOC-03), which revises it with the peak-memory and lookup
regressions in view.

# Results (override, GCC, Clang)

Exploratory (D-17): no decision, no effect on the baseline above or on any
candidate. Functional only; the measured series is ALLOC-04 in Phase 3.1.

**Clang** (CachyOS, 2026-11-23, Clang 23.1.5, source `19e5e4ad26`,
`production=yes tests=yes cppdefines="FORK_BACKEND_ALLOCATOR=3"
extra_suffix=override`): full `--test` 1602 cases, 1602 passed, 0 failed,
9 skipped (602048 assertions); `--test-case="*Fork*"` 99 passed (7310
assertions); `[Fork][Backends] Allocator scope` 4 of 4: a raw `malloc`
block, an `operator new` block and a glibc-internal `realpath` block are
in mimalloc's heap; `devtools/test/smoke_linux.sh` passes (editor and game
on Vulkan, PhysX CPU). So the process-wide override starts, runs the suite
and opens the editor without a mismatched free.

**GCC**: not built (short session, user decision 2026-11-23).
