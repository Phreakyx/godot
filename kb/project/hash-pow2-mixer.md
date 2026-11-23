---
type: Finding
title: Power-of-two capacity with a mixer for HashMap and HashSet (HASH-03 candidate)
description: HashMap and HashSet behind FORK_BACKEND_HASH_POW2 (default 0, prime capacity with fastmod) - power-of-two capacities from 8, the table hash mixed once in _hash() (1 multiply then xorshift, 2 hash_fmix32; byte-hashed String and CharString keys are not mixed again), slot and probe distance with a mask. Storage unchanged, HashSet keeps its one-block layout (D013); hasher outputs unchanged. MSVC (provisional): FAIL (engine), 70 REGRESSION rows; micro no evidence (layout sentinel). Phase 3 final: rejected, code deleted (last at e2c08be5d2).
tags: [core, containers, hashmap, hashset, hashing, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashmap
    resource: ../../core/templates/hash_map.h
    title: _capacity_of(), _home(), _increment_mod() and _hash() of HashMap (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: hashset
    resource: ../../core/templates/hash_set.h
    title: The same slot arithmetic inside the D013 block (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: hashfuncs
    resource: ../../core/templates/hashfuncs.h
    title: hash_table_mix(), applied after hash_table_key() (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_HASH_POW2 (default 0) (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: pins
    resource: ../../tests/fork/test_fork_hash_contracts.cpp
    title: The constant-hasher and key-type pins (the candidate part removed in Phase 3 final, last at e2c08be5d2)
---

# Design

- **Switch:** `FORK_BACKEND_HASH_POW2` in `core/templates/fork_backends.h`.
  0 is upstream: capacities from `hash_table_size_primes` (5, 13, 23, 47, ...),
  slot = `fastmod(hash)`, recomputed for every slot a probe passes (two 64-bit
  multiplies each). 1 and 2 are the candidate with its two mixers.
- **Capacity:** `2 << capacity_idx`, so the default index 2 is 8 slots (23
  with primes) and each growth doubles at `MAX_OCCUPANCY` (0.75), as today.
  The explicit-capacity constructors (and a moved-from HashSet) start at index
  2 instead of 0, so no table is smaller than 8.
- **Slot arithmetic:** home slot = `hash & mask`, the probe increment wraps
  with `& mask`, HashSet's probe distance is `(idx - home) & mask`. HashMap's
  `_probe_length_cmp()` compares positions and works for any capacity.
- **Mixer, once per key:** `_hash()` takes `hash_table_key()` and passes it
  through `hash_table_mix()` before the `EMPTY_HASH` fix-up. A mask keeps
  only the low bits, so they must depend on every input bit: a pointer key
  has zero low bits from alignment and many hashers leave structure in the
  low bits (for example `HashHasher` is the identity). Primes mix that in
  through the modulo; a mask does not.
  - **1:** `h *= 0x9E3779B1u; h ^= h >> 16;` (one multiply; the multiply moves
    low bits up, the shift brings the high half down).
  - **2:** `hash_fmix32(h)` (MurmurHash3 finalizer: two multiplies, three
    shifts; full avalanche).
  The micro (03-10) picks one; the other is deleted.
- **Not mixed again:** String and CharString keys with the default hasher,
  which `hash_table_key()` already hashes as bytes with wyhash/XXH3.
- **What does not change:** per-entry and per-slot bytes (HashMap 12 per slot,
  HashSet 12 per slot plus the key), the D013 block (`_allocate_block()`,
  `_free_block()`, `_keys_offset()`), iteration order (insertion order; the
  HashSet order after erase still comes from its key array), the outputs of
  `HashMapHasherDefault`, `String::hash()` and `Variant::hash()` (the mixer
  is table-internal, D-11), and key equality (the map's `Comparator`).
- **What changes:** `get_capacity()` of HashMap and HashSet returns powers of
  two. No engine code or test reads it for these two containers (only
  AHashMap's tests compare capacities, and AHashMap is untouched). A small
  map's index shrinks from 23 to 8 slots.
- **Risk (T-03-13):** a mask with a weak key hash clusters. The mixer runs
  for every key that is not byte-hashed; the constant-hasher pins check that
  the worst case (every key in one probe run) stays correct, and the micro
  rows for int, pointer and String keys check the speed. Per-process seeding
  stays deferred (FEAT-03).

# Tests

`[Fork][HashMap]` and `[Fork][HashSet]` pins added with the candidate, passing
on the control and both mixers: a constant hasher (50 keys found, every other
erased, survivors found and iterated in insertion order; for HashSet without
erase, insertion order), empty containers that find nothing and allocate
nothing, and String, StringName, `int64_t` and pointer keys (insert, find,
erase half, iterate). The `[Fork][Dictionary]` String and StringName pin
runs in every build.

MSVC, 2026-11-22: the production tests builds (control, `pow2` with 1,
`pow2b` with 2) pass the full `--test`, 1581 passed, 0 failed, 9 skipped
each; the dev tests builds of both mixers pass `--test-case="*Fork*"`, 84 of
84, `[Fork][Backends]` printing the value. No new compiler warning.

# Micro gate (03-10)

No time REGRESSION on the `hashmap_*` and `hashset_*` int, ptr and str rows
against the same-source control. `live_bytes` is reported per N but not
gated: power-of-two steps differ from the primes by design (8, 16, 32 against
23, 47, 97), so some N are smaller and some larger.

# Rules stated before measuring (MSVC)

Written before the first 03-10 series and not changed after it starts.
BASE is the provisional MSVC baseline of ALLOC-02, which is empty (no macro,
[decision](/project/alloc-vs-pool.md)). The control is therefore the plain
`bench` variant of the same source, and the previous side is `71b2819fec`.
Each series has one value of this macro on top of BASE and nothing else
(D-08). Both values are measured: `pow2` (`FORK_BACKEND_HASH_POW2=1`) and
`pow2b` (`FORK_BACKEND_HASH_POW2=2`).

**Micro gate (D-10), run first, once per value:**

```
python devtools/bench/bench.py micro --base-defines "" --name pow2 --defines "FORK_BACKEND_HASH_POW2=1" \
    --prev 71b2819fec --target <T> --out devtools/out/bench/hash-micro-pow2
python devtools/bench/bench.py micro --base-defines "" --name pow2b --defines "FORK_BACKEND_HASH_POW2=2" \
    --prev 71b2819fec --target <T> --out devtools/out/bench/hash-micro-pow2b
```

`<T>` is the comma list of `micro/<case>` for every harness case that starts
with `hashmap_` or `hashset_`: the i64, ptr and str matrix and the tiny ptr
rows, 146 cases each, taken from the control's case list. The accept rule
still judges a slower target row as REGRESSION, the same as any other row
(`verdict()` in `bench.py`), so the target list leaves the gate unchanged.
It only lets a faster row read ACCEPT, which the value choice counts.

- **Gate, per value:** no `ns_per_op` row starting with `hashmap_` or
  `hashset_` (i64, ptr and str keys) reads REGRESSION against the
  same-source control. A value with a REGRESSION there is FAIL (gate).
  `live_bytes` and `allocs` are recorded per N where they change, but not
  gated (power-of-two steps differ from the primes by design).
- **Value choice:** among the values that pass the gate, take the one with
  more ACCEPT rows on those rows. A tie goes to 1. Only the chosen value gets
  an engine series. The other value's code is deleted when the candidate is
  decided (03-13), not in 03-10.
- **Layout sentinel:** the entry records the
  `LAYOUT-SENTINEL: <k> of <m> control rows moved` line of each series. If
  k is above 0, the series is repeated once with the same command (`--out`
  with a `-2` suffix) and both lines are recorded. The gate and the value
  choice are then read on the repeat.

**Engine series (D-09), only for the chosen value, after its gate:**

```
python devtools/bench/bench.py compare --skip-build --base-defines "" --name <pow2 or pow2b> \
    --defines "FORK_BACKEND_HASH_POW2=<1 or 2>" --prev 71b2819fec \
    --target scene:reparent:10000,scene:script:10000 --workloads all --out devtools/out/bench/hash-<name>
python devtools/bench/bench.py verdict --series devtools/out/bench/hash-<name> \
    --rows scene:reparent:10000@editor,scene:script:10000@template_release
```

- **Pass:** `verdict` prints PASS. That means one of the two target rows
  (`frame_ms_p50`, A/A floors 3.13% for `scene:reparent:10000@editor` and
  1.79% for `scene:script:10000@template_release`) reads ACCEPT, and no row
  of the series (time and `peak_ws_mb`, every workload, editor and
  template_release) reads REGRESSION.
- **Counts:** every `ALLOC-INCREASE` or `INCREASE` count row is explained
  in this entry, for example more resizes from a smaller first capacity (8
  slots against 23).
- **Repeat:** a series that refuses, or that has more than a fifth of its
  rounds clock-flagged, is repeated.
- **Outcome:** PASS, FAIL (gate) or FAIL (engine), provisional until the
  GCC and Clang numbers (D-07). No default changes in `fork_backends.h`.

**Amendments before measuring (user, 2026-11-22).** Accepted before the
first series; the rules are frozen from here on.

- **Layout sentinel (B):** a micro gate is read only on a run whose
  `LAYOUT-SENTINEL` line reads 0 rows moved. If k is above 0, the series is
  repeated once (`--out` with a `-2` suffix). If the repeat also has k above
  0, the MSVC micro result is "no evidence (layout sentinel k1, k2)", neither
  PASS nor FAIL, and the micro gate is decided by GCC and Clang in 03-11. The
  value choice then falls back to its default, 1.
  The engine series still runs and its verdict is recorded. Both sentinel
  lines are recorded here.

# Results (MSVC)

**Micro, `pow2` (`FORK_BACKEND_HASH_POW2=1`):**
`LAYOUT-SENTINEL: 4 of 256 control rows moved`, repeat
`LAYOUT-SENTINEL: 6 of 256 control rows moved`: **micro: no evidence on
MSVC (layout sentinel 4, 6)**. For the record (not a gate reading), on the
292 `hashmap_*`/`hashset_*` rows the runs read 105 and 111 ACCEPT, 21 and
16 REGRESSION. Wins: HashMap insert at N 1 and 16 (`hashmap_ptr_insert_1`
1976 to 110 ns, -94%; `hashmap_i64_insert_16` -70%), misses at every N
(-17% to -41%), churn and large-N hits (-13% to -31%). Losses, the same in
both runs: `hashset_{i64,ptr}_copy_32768` +330% to +382% (1.2 to 5.2 ns),
`hashset_*_insert_16` +11% to +58%, `hashset_*_insert_32768` +8% to +12%,
the tiny ptr rows at 8 (`hashmap_tiny_ptr_hit_8` +20%,
`hashset_tiny_ptr_build_8` +61%) and `hashmap_ptr_iterate_4096` +9%.

**Micro, `pow2b` (`FORK_BACKEND_HASH_POW2=2`):**
`LAYOUT-SENTINEL: 5 of 256 control rows moved`, repeat
`LAYOUT-SENTINEL: 6 of 256 control rows moved`: **no evidence (5, 6)**.
80 and 84 ACCEPT, 34 and 32 REGRESSION: the losses of `pow2` plus hits and
misses at N 0 to 16 (`hashmap_i64_miss_1` +43%), where the second
`hash_fmix32` costs more than it saves.

**Value:** no value has a readable gate, so the choice falls back to its
default (amendment B): **1** (`pow2`). `pow2b` gets no engine series.

**Engine, `pow2`** (`devtools/out/bench/hash-pow2`):
`verdict` prints `FAIL devtools\out\bench\hash-pow2: REGRESSION in ...`
(70 rows). Targets, `frame_ms_p50` control to candidate:
`scene:reparent:10000@editor` 0.065 to 0.067 ms (+1.6%, 1 win of 11,
WITHIN-NOISE), `scene:script:10000@template_release` 1.176 to 1.180 ms
(-0.6%, WITHIN-NOISE); `scene:script:10000@editor` reads REJECT (-2.4%,
8 of 11 wins, floor 1.2%). REGRESSION rows: Dictionary duplicate, erase
and insert in gdbench (+7% to +20%, `benchmark_duplicate_int_100k`
+20%), the `micro_engine` Dictionary copies (+5% to +15%) and small-N
misses (`hashmap_variant_miss_1` +19% and +26%), several
`string_manipulation` rows (+5% to +17%), `array.benchmark_fill_loop`
+9% (editor) and `micro_engine` `peak_ws_mb` +1.0% on both builds. Peak
memory of the targets, `level` and `editor_startup`: WITHIN-NOISE.
**Counts:** `allocs` ALLOC-INCREASE on the gdbench Dictionary rows
(`benchmark_insert_int_1k` 101501 to 101901), the `micro_engine` dict and
hashmap insert and copy rows (`dict_i64_insert_16` 155648 to 188416,
`dict_i64_copy_256` 132608 to 133632), `scene:instance:10000` (+1.0%),
`script:instance` and `gdbench/alloc`. The likely cause is the one this
entry named: the first capacity is 8 slots, so a growing table takes
more resize steps, and a copy that rebuilds by inserting takes them too.
No reallocs change.

**Conditions.** Source `f26a9de41f` (bench binaries built in 03-10
Task 1; the runner tree carried this plan's kb edits and the
`MICRO_HEADERS` line), previous `71b2819fec`, BASE empty, control the
plain `bench` build of the same source. MSVC toolset 14.51.36231 (micro
`cl` 19.51.36260), Visual Studio 18.11.0, i9-14900K, BIOS 1.K0, microcode
0x12b, Windows build 26100.9711, power plan
`fe1217fb-3f66-46bf-adc6-e48c27825f80` (core parking off), Game Mode on,
bench CPUs 0xfffc. 2026-11-22, 19:10-22:15 UTC, PC idle, Inventory and
Compatibility Appraisal service stopped. Default `--max-load`; bench CPUs
1.9-6.2% busy at the start of each series; no series refused. Effective
clocks 3594-4164 MHz (micro), 4114-4443 MHz (engine). Clock-flagged rounds:
only some warm-up rounds (round 0, discarded) of the micro series; no
measured round flagged in any series. Micro: 11 rounds after the warm-up
pair; engine: 11 rounds per workload, 54 workload rounds per series, none
flagged.

**Layout sentinel (amendment B).** 12 of the 14 MSVC micro series moved 4
to 16 of the 256 control rows; only `rapid` and the xxHash check read 0.
Every candidate except `rapid` therefore has no MSVC micro evidence, and
its micro gate is decided by GCC and Clang (03-11).

**Outcome: FAIL (engine)** (micro: no evidence on MSVC). Rejected on
MSVC; acceptance needs every compiler, so the GCC and Clang micro
(03-11) can only confirm the rejection or record numbers.

# Results (GCC, Clang)

Not measured (user decision 2026-11-23): acceptance needs every compiler,
so an MSVC rejection is final.

# Decision (final)

**Rejected**, tier: FAIL (engine), 70 REGRESSION rows; micro no evidence (layout sentinel).

Code: last at `e2c08be5d2`, deleted in Phase 3 final.
