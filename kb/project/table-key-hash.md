---
type: Finding
title: StringName and pointer table hashes (HASH-05 candidate)
description: hash_table_key() behind FORK_BACKEND_TABLE_KEY_HASH (default 0, HashMapHasherDefault's output) - with 1 and the default hasher a StringName key is hashed by its data pointer and a pointer key by one 64-bit multiply (high half). Table-internal only; HashMapHasherDefault and every public hash keep their outputs. Built and pinned; timings to come from 03-10.
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

# Results

Not measured yet (03-10).
