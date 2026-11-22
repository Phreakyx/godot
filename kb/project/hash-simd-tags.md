---
type: Finding
title: SIMD tag probing for AHashMap and HashSet (HASH-06 candidate)
description: AHashMap and HashSet behind FORK_BACKEND_HASH_TAGS (default 0, robin-hood probing) - one control byte per slot (EMPTY, DELETED or a 7-bit tag of the hash_fmix32-mixed hash) matched 16 at a time with SSE2 on x86-64 and an 8-byte SWAR fallback elsewhere, 15 mirrored control bytes after the last slot, power-of-two capacities from 16. AHashMap keeps its dense elements and index rules, HashSet its one-block layout (D013); HashMap is not touched. MSVC (provisional): FAIL (engine), target scene:script REGRESSION; micro no evidence.
tags: [core, containers, ahashmap, hashset, hashing, simd, phase-3]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashtags
    resource: ../../core/templates/hashfuncs.h
    title: HashTags, the group match, probe, mirror and erase helpers both containers share
  - id: ahashmap
    resource: ../../core/templates/a_hash_map.h
    title: The tag path of AHashMap (control block, slots, element hashes)
  - id: hashset
    resource: ../../core/templates/hash_set.h
    title: The tag path of HashSet inside the D013 block
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_HASH_TAGS (default 0)
  - id: pins
    resource: ../../tests/fork/test_fork_hash_contracts.cpp
    title: The one-hash, growth-point, empty-table and group-match pins
---

# Design

- **Switch:** `FORK_BACKEND_HASH_TAGS` in `core/templates/fork_backends.h`.
  0 is upstream's robin-hood probing over one 32-bit hash per slot (HashSet
  with the slot arithmetic of `FORK_BACKEND_HASH_POW2`). 1 is the candidate.
- **Control bytes:** one per slot. `EMPTY` (0x80), `DELETED` (0xFE), or a
  full slot's tag, the low 7 bits of the hash (high bit clear). The home slot
  is `(hash >> 7) & mask`. A table has `capacity + 16` control bytes: the 15
  after the last slot mirror slots 0 to 14 (every write goes to the slot and
  to `((slot - 15) & mask) + 15`, which is the slot itself from 15 on), and the
  last byte is padding. A 16-byte group read at any slot therefore stays inside
  the allocation.
- **Group match:** `_mm_loadu_si128`, `_mm_cmpeq_epi8` and `_mm_movemask_epi8`
  on x86-64 (the `defined(__x86_64__) || defined(_M_X64)` gate of
  `DYNAMIC_BVH_SSE`; SSE2 is the x86-64 baseline), else two 8-byte SWAR words
  with an exact zero-byte test, `~(((x & 0x7F..) + 0x7F..) | x | 0x7F..)`
  (the borrow trick `(x - 0x01..) & ~x & 0x80..` has false matches, and a
  false match would read a stale slot index). Set bits are walked with
  `std::countr_zero`. The `[Fork][HashTags]` case checks both against a byte
  loop on x86-64, so the SWAR code is tested where the tests run.
- **Probe:** groups at triangular offsets from the home slot (`pos += 16`,
  `+= 32`, ...), which visit every group start of a power-of-two table. A
  lookup checks every tag match of a group with the full key compare (the
  map's `Comparator`), then stops at the first group with an `EMPTY` byte.
  Insertion takes the first `EMPTY` or `DELETED` slot on the same sequence.
- **Mixer:** `hash_table_mix<Hasher, TKey, 2>()`, that is `hash_fmix32` on
  `hash_table_key()`, so the tag and the home slot use different, mixed bits;
  byte-hashed String and CharString keys (wyhash/XXH3) are not mixed again.
  `hash_table_mix()` takes the mixer as a template argument now; HashMap's
  pow2 path still uses `FORK_BACKEND_HASH_POW2`'s choice.
- **Deletion:** the slot becomes `EMPTY` when the run of non-`EMPTY` bytes
  around it is shorter than 16 (no group was ever full across it, so no probe
  went past it), else `DELETED` (the SwissTable rule; one group before and
  one at the slot). When `size + deleted` reaches the growth point, the table
  grows if it is full or fewer than a quarter as many bytes as keys are
  `DELETED`, else it is rebuilt in place at the same capacity, which drops
  every `DELETED` byte. At least a quarter of the slots stay `EMPTY`, so every
  probe ends.
- **Growth point:** AHashMap's, unchanged: 3/4 of the capacity (the element
  array holds `capacity * 3 / 4` elements). HashSet uses the same 3/4 (its
  `MAX_OCCUPANCY`).
- **AHashMap:** `_ctrl` is one block: control bytes, then the element index
  of each slot (`uint32_t`), then the hash of each dense element. Elements
  stay dense in insertion order, `erase()` still moves the last element into
  the hole, and the moved element's slot is found by probing with its stored
  hash for the slot holding its old index (no key hash, no key compare).
  Resizing re-places every element from the stored hashes. Capacities are
  powers of two from 16 (the default is already 16; an explicit capacity or
  `reserve()` below 16 gives 16, where the control path allows 4 and 8). A
  moved-from map (capacity 1) is raised to 16 on its first insert.
- **HashSet:** the D013 block holds, in order, the hash of each key, the key
  index of each slot, the control bytes, then the keys at their alignment;
  `_hashes` is the block, `_allocate_block()` is the only allocation. The
  block holds `capacity * 3 / 4` keys (the control path sizes the key array
  at the full capacity). There is no key-to-slot index: `erase()` finds the
  moved last key's slot by its stored hash, as in AHashMap, and the
  `_deleted` count takes that pointer's place, so `sizeof(HashSet)` stays 40.
  Capacities are `4 << capacity_idx` (16 at the default index 2), with or
  without `FORK_BACKEND_HASH_POW2`, which this path ignores.
- **Bytes:** AHashMap index 8 per slot plus 16 per table, as the control
  (`Metadata` is 8 per slot), and `sizeof(AHashMap)` grows from 24 to 32
  bytes (the `_deleted` count). HashSet 8 per slot plus 16 per table plus
  `0.75 * sizeof(TKey)` per slot, against 12 plus `sizeof(TKey)` per slot on
  prime capacities (23, 47, 97, ...): a default `HashSet<int>` holds 192
  bytes instead of 368.
- **HashMap is out:** REQUIREMENTS rules out SwissTable or dense storage for
  HashMap and Dictionary (their elements must not move on growth). HashMap
  keeps its code; `git diff` of `hash_map.h` is empty for this candidate.

# Tests

New `[Fork][AHashMap]` and `[Fork][HashSet]` pins, passing on the control:
40 keys with one hash (found; every other one erased; the survivors found;
the dense order follows erase-moves-last), every key found after each insert
up to and past four growth points (a mixed and a one-hash table, so probe
groups wrap past the last slot), and empty, reserved, copied, moved and
emptied tables that find nothing (and allocate nothing before the first
insert). `[Fork][HashTags]` (tag builds only) compares the SSE2 and SWAR
matches with a byte loop.

MSVC, 2026-11-22: the production control passes the full `--test` (1590
passed, 0 failed, 9 skipped), `tags` and `tagsall` (every earlier hash
candidate on too) pass 1591 each, and the dev `tags` build passes
`--test-case="*Fork*"`, 94 of 94 (see
[unit-test baseline](/project/unit-test-baseline.md)). A scratch model check
against `std::unordered_map` (random insert, erase, `replace_key`,
`erase_by_index`, copy, move, `clear` and `reserve` with a mixed, a constant,
a four-value and an identity hasher) also passed with 0, 1 and 1 plus every
earlier candidate. Not built on GCC or Clang yet (03-11).

# Gate (03-10)

The micro decides: no time regression on any `ahashmap_*` or `hashset_*` row
against the same-source control, and `live_bytes` reported for both (the
gate rejects a rise on integer and pointer keys). The misses (`*_miss_*`) and
hits at large N are the rows the tags are meant to win. The `-Bench` builds
needed: `tags` (`FORK_BACKEND_HASH_TAGS=1`) on BASE and its control.

# Rules stated before measuring (MSVC)

Written before the first 03-10 series and not changed after it starts.
BASE is the provisional MSVC baseline of ALLOC-02, which is empty (no macro,
[decision](/project/alloc-vs-pool.md)). The control is therefore the plain
`bench` variant of the same source, and the previous side is `71b2819fec`.
Each series has this one macro on top of BASE (D-08).

**Micro gate (D-10), run first:**

```
python devtools/bench/bench.py micro --base-defines "" --name tags --defines "FORK_BACKEND_HASH_TAGS=1" \
    --prev 71b2819fec --out devtools/out/bench/hash-micro-tags
```

- **Gate:** no row starting with `ahashmap_` or `hashset_` (i64, ptr and
  str keys, and the hashset tiny rows) reads REGRESSION against the
  same-source control. A REGRESSION is FAIL (gate). `live_bytes` and
  `allocs` are recorded for both containers.
- **Layout sentinel:** the entry records the
  `LAYOUT-SENTINEL: <k> of <m> control rows moved` line. If k is above 0,
  the series is repeated once with the same command (`--out` with a `-2`
  suffix) and both lines are recorded. The gate is then read on the repeat.

**Engine series (D-09), only after the gate passes:**

```
python devtools/bench/bench.py compare --skip-build --base-defines "" --name tags \
    --defines "FORK_BACKEND_HASH_TAGS=1" --prev 71b2819fec \
    --target scene:reparent:10000,scene:script:10000 --workloads all --out devtools/out/bench/hash-tags
python devtools/bench/bench.py verdict --series devtools/out/bench/hash-tags \
    --rows scene:reparent:10000@editor,scene:script:10000@template_release
```

- **Pass:** `verdict` prints PASS. That means one of the two target rows
  (`frame_ms_p50`, A/A floors 3.13% for `scene:reparent:10000@editor` and
  1.79% for `scene:script:10000@template_release`) reads ACCEPT, and no row
  of the series (time and `peak_ws_mb`, every workload, editor and
  template_release) reads REGRESSION.
- **Counts:** every `ALLOC-INCREASE` or `INCREASE` count row is explained
  in this entry, for example a capacity of 16 where the control allows 4 or
  8, or HashSet's key array at 3/4 of the capacity.
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
  PASS nor FAIL, and the micro gate is decided by GCC and Clang in 03-11.
  The engine series still runs and its verdict is recorded. Both sentinel
  lines are recorded here.
- **Stricter older gate (C):** where the "Gate (03-10)" section above is
  stricter, it applies: a `live_bytes` rise on any i64 or ptr `ahashmap_*`
  or `hashset_*` row is FAIL (gate).

# Results (MSVC, provisional until GCC and Clang)

**Micro, `tags` (`FORK_BACKEND_HASH_TAGS=1`):**
`LAYOUT-SENTINEL: 16 of 256 control rows moved`, repeat
`LAYOUT-SENTINEL: 11 of 256 control rows moved`: **micro: no evidence on
MSVC (layout sentinel 16, 11)**. The counts do not depend on layout and
are the same in both runs: `live_bytes` rises by 16 bytes per table on
all 82 i64 and ptr `ahashmap_*` rows (`ahashmap_i64_insert_1` 320 to 336,
`_1048576` 41943040 to 41943056), which amendment C names a FAIL (gate)
once a gate is read; HashSet `live_bytes` drops (`hashset_i64_insert_1`
464 to 240, `_1048576` 31457384 to 29360144) while its `allocs` rise
(`hashset_*_insert_16` 8192 to 16384, `_1048576` 17 to 18). For the
record, 70 of the 284 gated rows read REGRESSION in both runs: AHashMap
and HashSet hits, misses, churn and erase at N 0 to 256 (+25% to +162%,
`hashset_i64_churn_1` +95% and +102%, `ahashmap_i64_miss_16` +69% and
+75%, `hashset_i64_iterate_16` +103% and +162%), `ahashmap_*_hit_1048576` +24% to +27%
and `ahashmap_str_*` at N 0 to 256 (+9% to +23%). No row read faster
beyond its floor in a way the gate counts (no target rows).

**Engine** (`devtools/out/bench/hash-tags`): `verdict` prints
`FAIL devtools\out\bench\hash-tags: REGRESSION in ...` (48 rows).
Targets: `scene:reparent:10000@editor` 0.066 to 0.065 ms (WITHIN-NOISE),
`scene:script:10000@template_release` 1.146 to 1.191 ms (+2.3%, floor
1.8%, **REGRESSION**). Other REGRESSION rows: the `micro_engine`
`ahashmap_sname_*` rows at every size (+8% to +51%,
`ahashmap_sname_hit_16` +51%), `hashmap_sname_miss_*` editor (+8% to
+23%), `script:script` `frame_ms_p50` editor +1.5%, gdbench signal
emission (+2% to +4%) and Dictionary rows, `peak_ws_mb` of
`editor_startup` (+5.7%, the same two-valued reading as in
[table-key-hash](/project/table-key-hash.md)), `scene:reparent:10000` and
`scene:churn:10000` template_release (+0.4% to +0.6%) and `gdbench`
(+0.1%). **Counts:** no ALLOC-INCREASE or INCREASE row in the engine
series.

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
MSVC. The small-N losses are the cost of a group probe on tables that
robin-hood answers in one or two compares.
