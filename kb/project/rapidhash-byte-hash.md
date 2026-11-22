---
type: Finding
title: rapidhash V3 against wyhash for short byte keys (HASH-04 candidate), xxHash 0.8.4
description: hash_buffer64() behind FORK_BACKEND_BYTE_HASH (default 0, wyhash final v4.3) - with 1 rapidhash V3 (rapidhash_withSeed) hashes buffers below HASH_XXH3_THRESHOLD (256 bytes); XXH3 above it either way. Table-internal only. xxHash taken from 0.8.3 to 0.8.4 as a plain update with the same outputs. MSVC (provisional): FAIL (gate), slower at 32 and 128 bytes; xxHash 0.8.4 no slowdown.
tags: [core, containers, hashing, thirdparty, phase-3]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashfuncs
    resource: ../../core/templates/hashfuncs.cpp
    title: hash_buffer64(), hash_buffer() and hash_table_string()
  - id: rapidhash
    resource: ../../thirdparty/rapidhash/rapidhash.h
    title: rapidhash V3 (tag rapidhash_v3, bc4b4baa48a15ff52ff4725e1ccdcda62815221c)
  - id: xxhash
    resource: ../../thirdparty/xxhash/xxhash.h
    title: xxHash 0.8.4 (c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0)
  - id: test
    resource: ../../tests/core/templates/test_hash_buffer.cpp
    title: Byte hash dispatch against the selected reference
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_BYTE_HASH (default 0)
---

# Design

- **Switch:** `FORK_BACKEND_BYTE_HASH` in `core/templates/fork_backends.h`.
  0 is the fork's current byte hash, wyhash final v4.3 below 256 bytes
  (upstream has no byte hash; this fork added `hash_buffer()` for String and
  CharString table keys). 1 is rapidhash V3: `hash_buffer64()` calls
  `rapidhash_withSeed(data, size, seed)` below `HASH_XXH3_THRESHOLD`; at 256
  bytes and up `XXH3_64bits_withSeed()` is used either way. `hashfuncs.cpp`
  includes only the header its value needs.
- **Scope:** table-internal only. `hash_buffer*()` is called by
  `hash_table_key()` (String and CharString keys with the default hasher)
  and by the container micro; no public hash, saved format or API
  fingerprint reads it. Only the slot a key lands in changes; iteration
  order is insertion order (HashMap, HashSet) or AHashMap's dense order.
- **Vendored:** `thirdparty/rapidhash/rapidhash.h` and `LICENSE` (MIT,
  "Copyright 2025 Nicolas De Carli") at the commit of tag `rapidhash_v3`
  (`bc4b4baa48a15ff52ff4725e1ccdcda62815221c`; a lightweight tag, so the
  tag ref is the commit), unchanged but for its CRLF line endings, stored as LF. Only `rapidhash_withSeed` is used, not
  the Micro or Nano variants. Its `_likely_`/`_unlikely_` macros match
  wyhash's, and `hashfuncs.cpp` includes only one of the two headers.
- **xxHash 0.8.4:** the tracked `xxhash.h` was byte-identical to upstream
  0.8.3 (`e626a72bc232`), so it was replaced by 0.8.4 (`c87183a77d67`,
  `LICENSE` unchanged) in its own commit, with no macro. The XXH3 outputs
  are the same: `test_hash_buffer` checks `hash_buffer64()` against the
  vendored `XXH3_64bits_withSeed()` at 256, 257 and 1024 bytes, and the
  full suite passes on it.

# Tests

`tests/core/templates/test_hash_buffer.cpp` "[Hashing] Byte hash dispatch"
compares `hash_buffer64()` and `hash_buffer()` with the selected short-key
reference (`rapidhash_withSeed` with 1, wyhash with 0) at 0 to 255 bytes
(lengths around the 16, 48, 64, 80 and 112 byte branches) and three seeds,
and with XXH3 from 256 bytes, one byte off alignment. The String key cases
check that `hash_table_key()` of String and CharString equals
`hash_buffer()` of their bytes with either value.

# Micro gate (03-10)

Rows: `hash_bytes_*` below 256 bytes and `hashmap_str_*`. Accept when at
least one `hash_bytes` row below 256 bytes reads ACCEPT against the
same-source control and none reads REGRESSION.

# Rules stated before measuring (MSVC)

Written before the first 03-10 series and not changed after it starts.
BASE is the provisional MSVC baseline of ALLOC-02, which is empty (no macro,
[decision](/project/alloc-vs-pool.md)). The control is therefore the plain
`bench` variant of the same source, and the previous side is `71b2819fec`.
Each series has this one macro on top of BASE (D-08).

**Micro gate (D-10), run first:**

```
python devtools/bench/bench.py micro --base-defines "" --name rapid --defines "FORK_BACKEND_BYTE_HASH=1" \
    --prev 71b2819fec --target micro/hash_bytes_8,micro/hash_bytes_32,micro/hash_bytes_128 \
    --out devtools/out/bench/hash-micro-rapid
```

- **Gate:** at least one `hash_bytes` row below 256 bytes (`hash_bytes_8`,
  `hash_bytes_32`, `hash_bytes_128`, passed as `--target`) reads ACCEPT
  against the same-source control, and none of them reads REGRESSION.
  Otherwise the result is FAIL (gate). The `hashmap_str_*` rows and the
  other rows are recorded. `live_bytes` and `allocs` are recorded where
  they change.
- **Layout sentinel:** the entry records the
  `LAYOUT-SENTINEL: <k> of <m> control rows moved` line. If k is above 0,
  the series is repeated once with the same command (`--out` with a `-2`
  suffix) and both lines are recorded. The gate is then read on the repeat.

**Engine series (D-09), only after the gate passes:**

```
python devtools/bench/bench.py compare --skip-build --base-defines "" --name rapid \
    --defines "FORK_BACKEND_BYTE_HASH=1" --prev 71b2819fec \
    --target scene:reparent:10000,scene:script:10000 --workloads all --out devtools/out/bench/hash-rapid
python devtools/bench/bench.py verdict --series devtools/out/bench/hash-rapid \
    --rows scene:reparent:10000@editor,scene:script:10000@template_release
```

- **Pass:** `verdict` prints PASS. That means one of the two target rows
  (`frame_ms_p50`, A/A floors 3.13% for `scene:reparent:10000@editor` and
  1.79% for `scene:script:10000@template_release`) reads ACCEPT, and no row
  of the series (time and `peak_ws_mb`, every workload, editor and
  template_release) reads REGRESSION. A target that reads WITHIN-NOISE is a
  rejection ("rejected, within noise"), recorded with the numbers of both
  tiers.
- **Counts:** every `ALLOC-INCREASE` or `INCREASE` count row is explained
  in this entry. The layout does not change, so none is expected.
- **Repeat:** a series that refuses, or that has more than a fifth of its
  rounds clock-flagged, is repeated.
- **Outcome:** PASS, FAIL (gate) or FAIL (engine), provisional until the
  GCC and Clang numbers (D-07). No default changes in `fork_backends.h`.

**xxHash 0.8.4 check (no macro):** the micro of the xxHash commit's parent
against the tree,
`micro --prev 4a4ea1e189^ --name xxh --defines "" --out devtools/out/bench/hash-micro-xxh`.
It reads previous against control on `hash_bytes_256`, `hash_bytes_4096`
and `hash_bytes_65536`. A control that is slower than previous by more than
the row's floor is a regression to report here.

**Amendments before measuring (user, 2026-11-22).** Accepted before the
first series; the rules are frozen from here on.

- **Layout sentinel (B):** a micro gate is read only on a run whose
  `LAYOUT-SENTINEL` line reads 0 rows moved. If k is above 0, the series is
  repeated once (`--out` with a `-2` suffix). If the repeat also has k above
  0, the MSVC micro result is "no evidence (layout sentinel k1, k2)", neither
  PASS nor FAIL, and the micro gate is decided by GCC and Clang in 03-11.
  The engine series still runs and its verdict is recorded. Both sentinel
  lines are recorded here.
- **xxHash check (A):** `thirdparty/xxhash/xxhash.h` is in `MICRO_HEADERS`
  of `bench.py`, so `micro --prev` compiles the previous side's xxHash and
  the check above compares 0.8.3 with 0.8.4.

# Results (MSVC, provisional until GCC and Clang)

**Micro, `rapid` (`FORK_BACKEND_BYTE_HASH=1`, targets `hash_bytes_8`,
`_32`, `_128`):** `LAYOUT-SENTINEL: 0 of 256 control rows moved`, so the
gate is read on this run. `hash_bytes_8` 2.219 to 1.848 ns (-16.1%, 11 wins,
floor 14.3%, ACCEPT); `hash_bytes_32` 2.219 to 2.371 ns (+7.2%, 0 wins,
floor 6.8%, **REGRESSION**); `hash_bytes_128` 4.116 to 4.360 ns (+5.9%,
1 win, floor 5.7%, **REGRESSION**). `hash_bytes_256` and up (xxHash, not
changed) WITHIN-NOISE. `hashmap_str_hit_16` reads REGRESSION (+15.6%);
the other `hashmap_str_*` rows WITHIN-NOISE. No `live_bytes` or `allocs`
change. **FAIL (gate)**: no engine series.

**xxHash 0.8.4 check** (`hash-micro-xxh`, `--prev c1753151ec`, the
xxHash commit's parent; its previous side compiled the 0.8.3 header,
sha256 `17973c0d...`): `LAYOUT-SENTINEL: 0 of 256 control rows moved`.
Control (0.8.4) against previous (0.8.3): `hash_bytes_256` 22.96 to
22.89 ns (-0.3%, floor 7.8%), `hash_bytes_4096` 107.03 to 105.86 ns
(-1.1%, floor 5.9%), `hash_bytes_65536` 1535.1 to 1540.9 ns (+0.4%,
floor 5.1%). **No slowdown beyond the floor**; the update stays.

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

**Outcome: FAIL (gate)**. Rejected on MSVC: rapidhash V3 wins only on
8-byte buffers here and loses at 32 and 128 bytes, the lengths most
String keys have.
