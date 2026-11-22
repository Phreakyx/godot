---
type: Finding
title: Power-of-two capacity with a mixer for HashMap and HashSet (HASH-03 candidate)
description: HashMap and HashSet behind FORK_BACKEND_HASH_POW2 (default 0, prime capacity with fastmod) - power-of-two capacities from 8, the table hash mixed once in _hash() (1 multiply then xorshift, 2 hash_fmix32; byte-hashed String and CharString keys are not mixed again), slot and probe distance with a mask. Storage unchanged, HashSet keeps its one-block layout (D013); hasher outputs unchanged. Built and pinned; timings to come from 03-10.
tags: [core, containers, hashmap, hashset, hashing, phase-3]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashmap
    resource: ../../core/templates/hash_map.h
    title: _capacity_of(), _home(), _increment_mod() and _hash() of HashMap
  - id: hashset
    resource: ../../core/templates/hash_set.h
    title: The same slot arithmetic inside the D013 block
  - id: hashfuncs
    resource: ../../core/templates/hashfuncs.h
    title: hash_table_mix(), applied after hash_table_key()
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_HASH_POW2 (default 0)
  - id: pins
    resource: ../../tests/fork/test_fork_hash_contracts.cpp
    title: The constant-hasher and key-type pins
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

# Results

Not measured yet (03-10).
