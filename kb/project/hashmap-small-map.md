---
type: Finding
title: Small-map HashMap without an index (HASH-07 candidate)
description: HashMap behind FORK_BACKEND_HASHMAP_SMALL (default 0, every map with elements has its index; the value is the threshold, 1 to 16, first 8, alternative 4) - a map of at most threshold elements allocates no hash and element arrays and finds keys by walking its element list with the Comparator; the index is built from the list when an insert passes the threshold or reserve() asks for more, kept when erases take the map back below it, dropped by clear(). Nodes never move, so pointers and insertion order hold; sizeof(HashMap) unchanged. Built and pinned, alone and with every other Phase 3 candidate; timings to come from 03-10.
tags: [core, containers, hashmap, small-map, phase-3]
status: draft
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

Results (MSVC 2026-11-22). `--test` passed on the control, `small8` and
`allhash`, which has every Phase 3 macro on (1595, 1595 and 1597 cases).
The Fork cases passed in the dev `small8` and dev `allhash` builds, and the
public hash outputs pin and the layout cases passed in all of them. The
first `small8` build stopped at an MSVC internal compiler error (C1001,
access violation) in `modules/enet/enet_multiplayer_peer.cpp`. The same
object compiled cleanly on the next run with no source change, and so did
`allhash`, which compiles the same code with more macros on. A repeat gets
a retry before anything is changed in the code.
