---
type: Finding
title: Small-map HashMap without an index (HASH-07 candidate)
description: HashMap behind FORK_BACKEND_HASHMAP_SMALL (default 0, every map with elements has its index; the value is the threshold, 1 to 16, first 8, alternative 4) - a map of at most threshold elements allocates no hash and element arrays and finds keys by walking its element list with the Comparator; the index is built from the list when an insert passes the threshold or reserve() asks for more, kept when erases take the map back below it, dropped by clear(). Nodes never move, so pointers and insertion order hold; sizeof(HashMap) unchanged. Built and pinned, alone and with every other Phase 3 candidate. MSVC (provisional): threshold 8, FAIL (engine), 68 REGRESSION rows; micro no evidence. Phase 3 final: rejected; code kept, off by default, revised in Phase 3.1.
tags: [core, containers, hashmap, small-map, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashmap
    resource: ../../core/templates/hash_map.h
    title: _small_find, _small_append, _small_build_index and the small branches of the lookups, insert, erase, replace_key, reserve, clear and copy
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_HASHMAP_SMALL (default 0)
  - id: pins
    resource: ../../tests/fork/test_fork_hash_contracts.cpp
    title: The five threshold pins (FORK_SMALL_THRESHOLD_FOR_TESTS = 8)
---

# Design

- **Switch:** `FORK_BACKEND_HASHMAP_SMALL` in `core/templates/fork_backends.h`.
  0 is upstream: a map allocates its index (`_hashes` and `_elements`) on the
  first insert. A value from 1 to 16 is the threshold. 8 is the first value;
  4 is the alternative.
- **No index at or below the threshold.** `_elements == nullptr` means there
  is no index. `get`, `getptr`, `has`, `find`, `operator[]`, `insert`, `erase`
  and `replace_key` then walk the element list from `_head_element` and
  compare keys with the map's `Comparator`. They do not compute a hash. A
  pointer, integer or StringName key costs one compare per element. A String
  key costs a length check and then a memcmp.
- **The index is built from the list.** An insert that would make the map
  larger than the threshold allocates the arrays at `_capacity_idx`, grown
  until `size + 1` fits under `MAX_OCCUPANCY`. It then puts every list node
  in with `_insert_element(_hash(key), node)`, and the insert continues on
  the normal index path. No node is moved or reallocated, so element
  pointers and the insertion order do not change.
- **Hysteresis.** An erase on a map that has an index keeps the index
  (upstream's erase, unchanged). The map goes back to having no index only
  through `clear()`.
- **`reserve(n)`** with `n` above the threshold builds the index right away,
  sized by the reserved capacity. That includes `HashMap(n)` and the
  initializer-list constructor. Upstream defers that allocation to the first
  insert. With `n` at or below the threshold, only the capacity index is
  set.
- **`clear()`** frees the index and keeps `_capacity_idx`, so the next index
  is built at the old size, with no chain of resizes. Upstream keeps the
  arrays and clears them with `memset`.
- **Copies.** The copy constructor and copy assignment reserve
  `min(source capacity, threshold)` when the source has no index, so a copy
  of a small map is small too. The assignment's "nothing to copy" test reads
  `_size`, because a small map has elements and no index. A move takes the
  null arrays with the list.
- **`debug_get_hash()`, `debug_get_element()`** return 0 and an empty
  iterator for a map without an index.
- **Layout:** there are no new members, so `sizeof(HashMap)`, the Dictionary
  and GDExtension sizes and the C# layout asserts do not change. The pool
  (`FORK_BACKEND_HASHMAP_POOL`), power-of-two capacity
  (`FORK_BACKEND_HASH_POW2`) and the table-key hashes work unchanged on both
  sides of the threshold. `_small_build_index()` goes through
  `_capacity_of()` and `_hash()`. HASH-02 (`FORK_BACKEND_HASHMAP_SLOTS`) was
  never built, see the [slot array](/project/hashmap-slot-array.md).
- **HashMap only.** AHashMap and HashSet have no per-element node, so the
  pointer-stability question of HASH-07 does not apply to them. Their
  dense-array scan stays an optional sub-candidate that has not been built.

# Semantics to watch

Below the threshold a key matches on `Comparator` alone. Upstream matches on
the stored hash and then the comparator. The two agree whenever the hasher
and comparator agree, which a hash table already requires:
`HashMapComparatorDefault` and `HashMapHasherDefault` normalize the same
float cases (NaN, -0.0). `StringLikeVariantComparator` (Dictionary) checks
the Variant type first, and String and StringName hash alike. A custom
comparator that is true for keys with different hashes would find more keys
below the threshold than above it. There is no such pair in the tree.

Expensive comparators (Variant keys that hold Arrays or Dictionaries, long
String keys with a shared prefix) cost up to threshold full compares on a
miss, where upstream usually rejects on the 32-bit hash. That is what the
`str` rows of the micro measure.

# How 03-10 decides the value

- Compare `small8` (`FORK_BACKEND_HASHMAP_SMALL=8`) with its same-source
  control on the `hashmap_tiny_*` rows and the `hashmap_*_{hit,miss,insert,churn,copy}`
  rows up to N = 16, for `live_bytes` and `allocs` as well as time.
- If `hashmap_tiny_ptr_hit_8` reads REGRESSION and the 1, 2 and 3 rows do
  not, measure 4 (`small4`).
- If the `str` rows (`hashmap_str_hit_16` and below) read REGRESSION and the
  pointer and integer rows win, leave String keys out of the small path
  (`std::is_same_v` on the key type) and record that. Do not lower the
  threshold for every key type.

# Tests

The pins in `tests/fork/test_fork_hash_contracts.cpp` cross
`FORK_SMALL_THRESHOLD_FOR_TESTS` (8), so they cross 8 and 4 in every build,
the control included:

- Growth past the threshold one insert at a time, with int, int64 and String
  keys. After every insert each earlier `getptr()` pointer is unchanged, the
  iteration order and the iterated value addresses match, and the next key
  is absent.
- Erasing back down to 2 elements, then inserting again, with a front
  insert. A copy of a small map and a copy of a large one keep the order.
- `reserve(threshold + 10)` on an empty map, `HashMap(n)`, and the
  initializer list.
- Erase of the head, the tail and a not-yet-visited element during
  iteration below the threshold, then `replace_key()` and `remove(it)`.
- `clear()` past the threshold, then inserts below and past it again.

The node-memory pin reads `cleared == empty` with the macro set: a
one-element map has no index. A scratch model check, not committed, ran
random insert, front insert, `operator[]`, erase, `replace_key`, `remove`
during iteration, `clear`, `reserve`, copy, assignment and move against a
`std::vector` model, with the default hasher, a constant hasher and a
four-value hasher. It checked every pointer after every step, for the
values 0, 1, 4, 8 and 16, with the pool, with every Phase 3 macro on, and
with `DEV_ENABLED`. A planted relink bug failed it.

Test results (MSVC 2026-11-22). `--test` passed on the control, `small8` and
`allhash`, which has every Phase 3 macro on (1595, 1595 and 1597 cases).
The Fork cases passed in the dev `small8` and dev `allhash` builds, and the
public hash outputs pin and the layout cases passed in all of them. The
first `small8` build stopped at an MSVC internal compiler error (C1001,
access violation) in `modules/enet/enet_multiplayer_peer.cpp`. The same
object compiled cleanly on the next run with no source change, and so did
`allhash`, which compiles the same code with more macros on. A repeat gets
a retry before anything is changed in the code.

# Rules stated before measuring (MSVC)

Written before the first 03-10 series and not changed after it starts.
BASE is the provisional MSVC baseline of ALLOC-02, which is empty (no macro,
[decision](/project/alloc-vs-pool.md)). The control is therefore the plain
`bench` variant of the same source, and the previous side is `71b2819fec`.
Each series has one threshold on top of BASE and nothing else (D-08).

**Micro gate (D-10), run first, for both thresholds:**

```
python devtools/bench/bench.py micro --base-defines "" --name small8 --defines "FORK_BACKEND_HASHMAP_SMALL=8" \
    --prev 71b2819fec --out devtools/out/bench/hash-micro-small8
python devtools/bench/bench.py micro --base-defines "" --name small4 --defines "FORK_BACKEND_HASHMAP_SMALL=4" \
    --prev 71b2819fec --out devtools/out/bench/hash-micro-small4
```

- **Threshold:** 8, unless `hashmap_tiny_ptr_hit_8` of `small8` reads
  REGRESSION while `hashmap_tiny_ptr_hit_1`, `_2` and `_3` do not. Then it
  is 4.
- **Gate, on the chosen threshold's series:** no `hashmap_tiny_*` row and
  no HashMap N 1 row (`hashmap_{i64,ptr,str}_<op>_1`) reads REGRESSION
  against the same-source control. A REGRESSION is FAIL (gate).
  `live_bytes` and `allocs` are recorded where they change. Small maps
  should drop both, because they have no index allocation. The `str` rows
  up to N 16 are recorded for the String-key question above.
- **Layout sentinel:** the entry records the
  `LAYOUT-SENTINEL: <k> of <m> control rows moved` line of each series. If
  k is above 0, the series is repeated once with the same command (`--out`
  with a `-2` suffix) and both lines are recorded. The threshold and the
  gate are then read on the repeat.

**Engine series (D-09), only for the chosen threshold, after its gate.**
`small4` is built for the engine (`compare --build-only`, then its Fork
cases) only if the threshold rule picks it.

```
python devtools/bench/bench.py compare --skip-build --base-defines "" --name <small8 or small4> \
    --defines "FORK_BACKEND_HASHMAP_SMALL=<8 or 4>" --prev 71b2819fec \
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
  in this entry, for example a `reserve()` past the threshold that
  allocates the index at once, or a `clear()` that frees the index and
  builds it again on the next insert past the threshold.
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
  threshold then falls back to its default, 8.
  The engine series still runs and its verdict is recorded. Both sentinel
  lines are recorded here.
- **String keys (C):** the "leave String keys out" option of "How 03-10
  decides the value" is not built in this window. If the `str` rows regress
  while the ptr and i64 rows win, that is recorded, and a String-key-excluded
  small map is named as a later candidate.

# Results (MSVC)

**Micro, `small8` (`FORK_BACKEND_HASHMAP_SMALL=8`):**
`LAYOUT-SENTINEL: 7 of 256 control rows moved`, repeat
`LAYOUT-SENTINEL: 5 of 256 control rows moved`: **micro: no evidence on
MSVC (layout sentinel 7, 5)**. For the record, the gated rows
(`hashmap_tiny_*` and HashMap N 1) read no REGRESSION in either run; the
tiny hits are 56% to 79% faster (`hashmap_tiny_ptr_hit_1` -77% to -79%,
`_8` -56% to -58%), but rows outside `--target` can only read REGRESSION or
WITHIN-NOISE. Counts, the same in both runs: N 1 `live_bytes` 308 to 32
and `allocs` 24576 to 8192 (`hashmap_i64_insert_1`), `hashmap_tiny_ptr_*`
`live_bytes` 3080000 to 320000 at 1 element. REGRESSION outside the gate
in both runs: `hashmap_{i64,ptr}_hit_4096` +50% to +54% and
`hashmap_{ptr,str}_miss_0` +22% to +29%.

**Micro, `small4` (`FORK_BACKEND_HASHMAP_SMALL=4`):**
`LAYOUT-SENTINEL: 12 of 256 control rows moved`, repeat
`LAYOUT-SENTINEL: 12 of 256 control rows moved`: **no evidence (12, 12)**.
No gated REGRESSION in the repeat (one in the first run); the same
`live_bytes` and `allocs` drops at 1 to 3 elements.

**Threshold:** with no readable gate the threshold falls back to its
default (amendment B): **8**. (`hashmap_tiny_ptr_hit_8` of `small8` read
faster in both runs, so the stated rule would also have kept 8.) `small4`
was not built for the engine.

**String keys (amendment C):** of the `str` rows up to N 16, only
`hashmap_str_miss_0` reads REGRESSION, and `hashmap_ptr_miss_0` does too,
so the loss is not String-specific; a String-key-excluded small map is
not indicated by these numbers.

**Engine, `small8`** (`devtools/out/bench/hash-small8`): `verdict`
prints `FAIL devtools\out\bench\hash-small8: REGRESSION in ...` (68
rows). Targets: `scene:reparent:10000@editor` 0.066 to 0.067 ms
(WITHIN-NOISE), `scene:script:10000@template_release` 1.185 to 1.114 ms
(-5.2%, floor 1.8%, **ACCEPT**); `scene:script:10000@editor` ACCEPT too
(-4.4%). REGRESSION rows: crossing the threshold, `micro_engine`
`dict_i64_insert_16` and `dict_variant_insert_16` template +144% and
+133% (45.8 to 111.3 ns), `dict_str_insert_16` editor +21%;
`hashmap_sname_copy_0` +44% (both builds), `hashmap_sname_hit_4096` +34%,
`dict_*_erase_0` and `_miss_0` +6% to +8%, `sname_from_cstr_1` +19%;
`script:script` `frame_ms_p50` editor +5.3%; gdbench Dictionary lookups
and misses (+3% to +13%), array rows on template_release (+3% to +8%) and
`string_manipulation` rows (+6% to +33%). Peak memory: WITHIN-NOISE on
every workload. **Counts:** no ALLOC-INCREASE or INCREASE row.

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
MSVC. The insert at 16 builds the index from the list once the map passes
8 elements, which is where the engine pays for the small path.

# Results (GCC, Clang)

Not measured (user decision 2026-11-23): acceptance needs every compiler,
so an MSVC rejection is final.

# Decision (final)

**Rejected**, tier: FAIL (engine) at threshold 8: scene:script ACCEPT, 68 REGRESSION rows; micro no evidence (layout sentinel).

Rejected on MSVC (Phase 3); code kept, off by default, revised in Phase 3.1 (ALLOC-03, HASH-08, HASH-09).
