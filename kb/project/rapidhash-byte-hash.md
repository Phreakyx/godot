---
type: Finding
title: rapidhash V3 against wyhash for short byte keys (HASH-04 candidate), xxHash 0.8.4
description: hash_buffer64() behind FORK_BACKEND_BYTE_HASH (default 0, wyhash final v4.3) - with 1 rapidhash V3 (rapidhash_withSeed) hashes buffers below HASH_XXH3_THRESHOLD (256 bytes); XXH3 above it either way. Table-internal only. xxHash taken from 0.8.3 to 0.8.4 as a plain update with the same outputs. Built and tested; timings to come from 03-10.
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

# Results

Not measured yet (03-10).
