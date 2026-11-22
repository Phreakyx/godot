---
type: Finding
title: StringName and pointer table hashes (HASH-05 candidate)
description: hash_table_key() behind FORK_BACKEND_TABLE_KEY_HASH (default 0, HashMapHasherDefault's output) - with 1 and the default hasher a StringName key is hashed by its data pointer and a pointer key by one 64-bit multiply (high half). Table-internal only; HashMapHasherDefault and every public hash keep their outputs. MSVC (provisional): FAIL (engine), both targets faster but 38 REGRESSION rows; micro no evidence.
tags: [core, containers, hashmap, hashset, hashing, phase-3]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashfuncs
    resource: ../../core/templates/hashfuncs.h
    title: hash_table_key(), the table-internal key hash of HashMap, AHashMap and HashSet
  - id: stringname
    resource: ../../core/string/string_name.h
    title: StringName::hash(), operator== and data_unique_pointer()
  - id: dictionary
    resource: ../../core/variant/dictionary.cpp
    title: Dictionary keys are Variant with StringLikeVariantComparator
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_TABLE_KEY_HASH (default 0)
  - id: pins
    resource: ../../tests/fork/test_fork_hash_outputs.cpp
    title: Golden public hash outputs
---

# Design

- **Switch:** `FORK_BACKEND_TABLE_KEY_HASH` in `core/templates/fork_backends.h`.
  0 keeps `HashMapHasherDefault`'s output as the table hash. 1 changes two
  key types in `hash_table_key()`, and only when the table uses
  `HashMapHasherDefault`; `HashMapHasherDefault` itself is not edited.
- **StringName keys:** hashed by `data_unique_pointer()` through the pointer
  hash below. StringName equality is `_data` identity (`operator==`
  compares the pointers), so equal keys get equal hashes. The cached hash
  (`_data->hash`) is a dependent load into the StringName data on every
  lookup; the pointer is in the key itself. An empty StringName has a null
  data pointer and hashes as pointer 0.
- **Pointer keys** (`std::is_pointer_v<T>`): `uint32_t((uint64_t(p) *
  0x9E3779B97F4A7C15ull) >> 32)`, one multiply, against Thomas Wang's
  64-to-32 mix in `hash_one_uint64()` (about nine dependent operations). The
  high half of the product depends on every bit of the pointer, so 8- and
  16-byte alignment does not leave the result with zero low bits.
  `char *` and `const char *` keys keep `HashMapHasherDefault`'s djb2 of
  the text, so a table with a text comparator stays consistent.
- **What does not change:** `HashMapHasherDefault`, `StringName::hash()`,
  `String::hash()`, `Variant::hash()` (pinned in
  `tests/fork/test_fork_hash_outputs.cpp`), key equality, iteration order.
  `Dictionary` keys are `Variant` with `StringLikeVariantComparator`, not
  StringName, so the String and StringName key equivalence there is
  untouched. Tables with their own hasher keep it.
- **With `FORK_BACKEND_HASH_POW2`:** both key types still go through
  `hash_table_mix()` (only byte-hashed String and CharString keys skip it).
- **Risk:** a weak spread of aligned pointers would cluster probe runs. The
  micro `*_ptr_*` rows and AHashMap's DEV_ENABLED collision warning would
  show it. Hashes stay deterministic (no per-process seed; FEAT-03 is
  deferred).

# Tests

`tests/core/templates/test_hash_buffer.cpp` "[Hashing] Table keys" checks
the table hash of a pointer and a StringName against the formula with 1 and
against `HashMapHasherDefault` with 0, and that `const char *` keys keep
djb2. The `[Fork][HashMap]` and `[Fork][HashSet]` pins with StringName and
pointer keys (insert, find, erase half, iterate) and the `[Fork][Hash]`
outputs pins run in every build.

# Micro gate (03-10)

Rows to watch: `hashmap_ptr_*`, `ahashmap_ptr_*` and the `micro_engine`
`sname_keys` cases. Accept only with no time REGRESSION on the pointer rows
and `live_bytes` unchanged (the layout does not change).

# Rules stated before measuring (MSVC)

Written before the first 03-10 series and not changed after it starts.
BASE is the provisional MSVC baseline of ALLOC-02, which is empty (no macro,
[decision](/project/alloc-vs-pool.md)). The control is therefore the plain
`bench` variant of the same source, and the previous side is `71b2819fec`.
Each series has this one macro on top of BASE (D-08).

**Micro gate (D-10), run first:**

```
python devtools/bench/bench.py micro --base-defines "" --name tkh --defines "FORK_BACKEND_TABLE_KEY_HASH=1" \
    --prev 71b2819fec --out devtools/out/bench/hash-micro-tkh
```

- **Gate:** no `*_ptr_*` row reads REGRESSION against the same-source
  control. That covers every Godot case with `_ptr_` in its name: hashmap,
  ahashmap and hashset, their tiny rows, rbmap and rbset. The `ctl_*` rows
  are third-party code and serve as the layout sentinel. A REGRESSION is
  FAIL (gate). `live_bytes` and `allocs` are recorded where they change.
- **Layout sentinel:** the entry records the
  `LAYOUT-SENTINEL: <k> of <m> control rows moved` line. If k is above 0,
  the series is repeated once with the same command (`--out` with a `-2`
  suffix) and both lines are recorded. The gate is then read on the repeat.

**Engine series (D-09), only after the gate passes:**

```
python devtools/bench/bench.py compare --skip-build --base-defines "" --name tkh \
    --defines "FORK_BACKEND_TABLE_KEY_HASH=1" --prev 71b2819fec \
    --target scene:reparent:10000,scene:script:10000 --workloads all --out devtools/out/bench/hash-tkh
python devtools/bench/bench.py verdict --series devtools/out/bench/hash-tkh \
    --rows scene:reparent:10000@editor,scene:script:10000@template_release
```

- **Pass:** `verdict` prints PASS. That means one of the two target rows
  (`frame_ms_p50`, A/A floors 3.13% for `scene:reparent:10000@editor` and
  1.79% for `scene:script:10000@template_release`) reads ACCEPT, and no row
  of the series (time and `peak_ws_mb`, every workload, editor and
  template_release, the `micro_engine` `sname_keys` cases included) reads
  REGRESSION.
- **Counts:** every `ALLOC-INCREASE` or `INCREASE` count row is explained
  in this entry. The layout does not change, so none is expected.
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
- **Stricter older gate (C):** where the "Micro gate (03-10)" section above
  is stricter, it applies: `live_bytes` must not change on any row, in
  addition to no REGRESSION on the `*_ptr_*` rows.

# Results (MSVC, provisional until GCC and Clang)

**Micro, `tkh` (`FORK_BACKEND_TABLE_KEY_HASH=1`):**
`LAYOUT-SENTINEL: 9 of 256 control rows moved`, repeat
`LAYOUT-SENTINEL: 7 of 256 control rows moved`: **micro: no evidence on
MSVC (layout sentinel 9, 7)**. `live_bytes` and `allocs` did not change on
any row (amendment C holds). For the record, 13 and 16 of the 246
`*_ptr_*` rows read REGRESSION; the same rows in both runs, 0 wins of 11:
`ahashmap_ptr_{insert,hit,churn,erase}_1048576` +125% to +250%
(`ahashmap_ptr_hit_1048576` 25.4 to 81.4 ns), `ahashmap_ptr_iterate_16`
+96% and `hashmap_ptr_hit_4096` +18%. The 1M AHashMap rows look like a
real effect, not layout: a pointer hash of one multiply leaves the bits
AHashMap's power-of-two mask reads poorly mixed for aligned pointers, so
probes cluster. 03-11 checks it on GCC and Clang. RBMap and RBSet rows
(not hashed) moved too (`rbset_ptr_copy_32768` +24%), the layout effect
the sentinel reports.

**Engine** (`devtools/out/bench/hash-tkh`): `verdict` prints
`FAIL devtools\out\bench\hash-tkh: REGRESSION in ...` (38 rows).
Targets: `scene:reparent:10000@editor` 0.066 to 0.061 ms (-7.5%, 11 wins
of 11, floor 3.1%, **ACCEPT**), `scene:script:10000@template_release`
1.169 to 1.125 ms (-3.8%, 10 of 11, floor 1.8%, **ACCEPT**);
`scene:script:10000@editor` ACCEPT too (-2.3%). REGRESSION rows:
`editor_startup` `peak_ws_mb` 687 to 730 MB (+6.0%; see below), `level`
`main_ms_p50` template_release 2.543 to 2.721 ms (+7.0%), the
`micro_engine` dict hits on editor (`dict_sname_hit_256` +7%,
`dict_i64_hit_16` +9%), `hashmap_variant_{hit,miss}_{1,16}` template
+11% to +32%, `ahashmap_sname_insert_16` +9%, gdbench Dictionary rows
(+1% to +4%) and `string_manipulation` rows (+6% to +13%). The
`editor_startup` peak reads 683-687 MB for the control in all four engine
series of 03-10 and 624-731 MB for the previous build and the
candidates, so the reading looks two-valued across binaries rather than
caused by this macro; under the frozen rule it still counts, and the
other rows fail the series on their own. **Counts:** no ALLOC-INCREASE or
INCREASE row, as expected.

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
MSVC, with the two targets faster: a later candidate that keeps the
StringName pointer hash and mixes pointer keys (or limits the multiply to
HashMap) is the variant worth measuring, named here, not built.
