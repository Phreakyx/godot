---
type: Finding
title: Per-map HashMap node pool (HASH-01 candidate)
description: HashMap nodes from a pool owned by each map, behind FORK_BACKEND_HASHMAP_POOL (default 0) - geometric pages from one node that never move, two page pointers in the map and a page table from the third page, an intrusive free list, the pool moved with the map and released by clear() and the destructor; insertion order, element addresses and every pinned contract kept. The map object grows from 40 to 72 bytes; tiny maps of 1 and 3 elements cost the control's bytes with one allocation fewer at 3, while 2 and 8 elements cost more bytes; allocations drop by orders of magnitude, but power-of-two sizes hold up to 2N-1 nodes and erased nodes stay until clear(). On MSVC (ALLOC-02) it is within noise on script:script template_release and fails on 82 regression rows; GCC and Clang follow. Phase 3 final: rejected, code deleted (last at e2c08be5d2).
tags: [core, containers, hashmap, memory, pool, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashmap
    resource: ../../core/templates/hash_map.h
    title: The pool block (_pool_*) and the hooks at the node sites (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_HASHMAP_POOL (default 0) (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: pins
    resource: ../../tests/fork/test_fork_hash_contracts.cpp
    title: The [Fork][HashMap] contract pins, including the pool pins (the candidate part removed in Phase 3 final, last at e2c08be5d2)
  - id: micro
    resource: ../../devtools/bench/micro/containers_bench.cpp
    title: The tiny-maps case (hashmap_tiny_*, hashset_tiny_*)
  - id: paged
    resource: ../../core/templates/paged_allocator.h
    title: PagedAllocator, the building block not reused
---

# Design

- **Switch:** `FORK_BACKEND_HASHMAP_POOL` in `core/templates/fork_backends.h`,
  default 0 (one `Allocator::new_allocation` per node, upstream). With 1, a
  map whose `Allocator` is the default `DefaultTypedAllocator<HashMapElement>`
  takes its nodes from its own pool (`static constexpr bool _USE_POOL`, an
  `if constexpr` at each node site). Maps with another `Allocator` (the
  `PagedAllocator` user in `renderer_canvas_render_rd.h`) keep their path;
  they still carry the pool fields (unused) because the fields depend on the
  macro, not on the `Allocator`.
- **Inside HashMap, not through `Allocator`:** the move constructor and move
  assignment never move the `Allocator` base, so a stateful allocator would
  stay with the source and its nodes would be freed with it (Pitfall 5). The
  pool fields are moved with `_elements`, `_hashes`, head and tail.
- **Pages:** raw `Memory::alloc_static` blocks; nodes are placement-constructed
  (`memnew_placement`). Page p holds `F << p` nodes, F = 1 by default. When
  `reserve(n)` runs on a pool with no node handed out yet, F is the next power
  of two of n (the copy constructor and copy assignment then set it to the
  source's size, not its capacity, so a copy of a 1-element map gets a
  1-node page). Growth stops when a page reaches 64 KiB of nodes (2048 nodes
  of 32 bytes); every later page has that size, so the last page wastes at
  most 64 KiB instead of half the map.
- **Page pointers:** pages 0 and 1 are fields of the map, so a pool of up to
  three nodes (F = 1) has no page table. From page 2 on, one field is the page
  table (all pages; 4 slots, then doubled by `realloc` at every power of two).
- **Stable index:** node i is the i-th node the bump pointer handed out. It
  decodes to (page, offset) with one bit scan (`std::bit_width`) in the
  growing pages and two shifts past the cap (one predictable branch):
  `page = bit_width(i / F + 1) - 1`, `offset = i - F * (2^page - 1)`.
  `_pool_node(i)` returns the node; the bump path's index is `_pool_next - 1`.
  A node taken from the free list has no index at hand: HASH-02 (index slots)
  must keep the index of a freed node in the free list (it knows it from the
  slot at erase time).
- **Free list:** intrusive. An erased node is destroyed and its first word,
  the `next` field, links the free list; insert takes the free list first,
  then bumps, then adds a page. No side array.
- **Release:** `clear()` and the destructor destroy the live nodes by walking
  the element list (skipped when the node is trivially destructible) and
  free every page and the table at once, so memory returns as with
  upstream's `clear()`. `clear()` of a map whose elements were all erased
  frees its pages too.
- **Copies:** the copy constructor and copy assignment build a fresh pool
  (reserve, then insert in order, as upstream).
- **Invariants (dev builds):** `DEV_ASSERT` that a decoded index is below the
  bump count, that every bumped node decodes back to itself, and that a freed
  node is not the free-list head (an immediate double free). A full
  double-free check is not cheap and is left out.

# Why not PagedAllocator

`core/templates/paged_allocator.h` has a 4096-node default page
(`DEFAULT_PAGE_SIZE`) and keeps an 8-byte-per-element side array of free
pointers (`available_pool`), and it includes `core/string/ustring.h`. A
1-element map would pay for 4096 nodes. The pool here starts at one node and
has no side state.

# Contracts kept

Iteration is the prev/next list, unchanged; pages never move, so element
addresses are stable; `replace_key`, `sort_custom` and erase during iteration
are unaffected. The `[Fork][HashMap]` pins pass on the control and the pool,
in production and dev tests builds (with `DEV_ASSERT`): the 11 existing ones
and 7 new ones, written before the pool and passing on the control:

- element pointers survive a move of the map after the source is destroyed
  (move constructor and move assignment into a map that holds elements);
- `clear()` then insert keeps insertion order;
- a copy (constructor and assignment) is independent of its source;
- `reserve(64)` then 64 inserts keeps every pointer;
- pointers stay valid across page boundaries (40 inserts), and survivors stay
  put when erased nodes are reused;
- re-inserting an existing key at sizes 1, 2, 3, 4, 7, 8, 15 and 16 keeps the
  size and the element's address;
- (`DEBUG_ENABLED`) a default-constructed map allocates nothing, one element
  costs one node's bytes, three elements three nodes' bytes, and `clear()`
  returns every node byte.

`DictionaryPrivate` holds a HashMap; its `static_assert`s (leading fields
only) still compile with the pool on, and no GDExtension builtin size
changes (`Dictionary` is one pointer).

# Sizes (MSVC x64, throwaway sizeof print)

| Type | Control | Pool |
| --- | --- | --- |
| `HashMap<StringName, int>` | 40 | 72 |
| `HashMap<DependencyTracker *, uint32_t>` | 40 | 72 |
| `HashMapElement<void *, uint32_t>` | 32 | 32 |
| `HashMapElement<StringName, int>` | 32 | 32 |

The 32 extra bytes per map object (two page pointers, the free-list head,
the bump count, the first-page shift) count in every `Object` and `Node`
that embeds HashMaps; they show in `peak_ws_mb` of the engine workloads, not
in the micro's `live_bytes` (the map objects there are not Godot memory).

# Tiny maps (D-05)

Requested bytes of one `HashMap<const uint64_t *, uint32_t>` (32-byte nodes,
the first index of 23 slots: 23 x 4 hash bytes + 23 x 8 pointer bytes = 276),
before allocator overhead:

| Elements | Control | Pool |
| --- | --- | --- |
| 1 | 32 + 276 = 308 bytes, 3 blocks | page 0 (1 node): 308 bytes, 3 blocks |
| 2 | 64 + 276 = 340 bytes, 4 blocks | pages 0, 1 (3 nodes): 372 bytes, 4 blocks |
| 3 | 96 + 276 = 372 bytes, 5 blocks | pages 0, 1: 372 bytes, 4 blocks |
| 8 | 256 + 276 = 532 bytes, 10 blocks | pages 0-3 (15 nodes) + 32-byte table: 788 bytes, 7 blocks |

The micro smoke (`bench.py micro --name pool --defines
"FORK_BACKEND_HASHMAP_POOL=1" --prev 71b2819fec --samples 5 --max-load 100`,
MSVC, 2026-11-22; a smoke, timings not judged) measured exactly these
numbers for 10,000 live maps (untimed build pass, the same on the `build`
and `hit` rows):

| Case | Control live_bytes | Pool live_bytes | Control allocs | Pool allocs |
| --- | --- | --- | --- | --- |
| `hashmap_tiny_ptr_*_1` | 3,080,000 | 3,080,000 | 30,000 | 30,000 |
| `hashmap_tiny_ptr_*_2` | 3,400,000 | 3,720,000 (+9.4%) | 40,000 | 40,000 |
| `hashmap_tiny_ptr_*_3` | 3,720,000 | 3,720,000 | 50,000 | 40,000 (-20%) |
| `hashmap_tiny_ptr_*_8` | 5,320,000 | 7,880,000 (+48%) | 100,000 | 70,000 (-30%) |
| `hashset_tiny_ptr_*_<N>` | 4,640,000 | 4,640,000 | 10,000 | 10,000 |

HashSet is untouched by the pool (one block per table, D013); its rows are
the reference.

The rest of the matrix (same smoke) shows the cost of the geometric pages
at larger N. Allocations in the timed regions drop by orders of magnitude
(`hashmap_i64_insert_1048576`: 1,048,610 to 558; `insert_4096`: 131,648 to
1,024), while `live_bytes` grows where N is a power of two just past a page
boundary: N = 16 holds 31 nodes plus a 64-byte table (788 to 1,332 bytes,
+69%), N = 4096 holds 6,143 nodes (204,884 to 270,516, +32%), N = 1M +0.14%
(past the 64 KiB cap pages are fixed). Erased nodes stay on the free list
until `clear()` or destruction, so a map that erases most of its elements
keeps its peak node memory (`hashmap_*_erase_4096`: 73,812 to 204,884
bytes after the erases; upstream frees each node). Only `hashmap_*` rows
print `COUNTS` lines; every other container matches the control.

`live_bytes` counts requested bytes only: per-block allocator overhead and
size-class rounding are not in it (Pitfall 6), and the pool saves blocks, so
its real advantage at 3 and 8 elements is larger than `live_bytes` says and
its 2- and 8-element byte cost is partly offset. Allocator memory is judged
on `peak_ws_mb` in the engine workloads. The index (276 of 308 bytes at one
element) is the larger tiny-map lever: HASH-03's smaller first capacity and
HASH-07's small-map path, not this pool. If the pool wins time, the 2- and
8-element byte cost, the power-of-two waste and the memory kept after
erases stand next to it (D-05).

# Results (MSVC)

ALLOC-02 on 2026-11-22, value 1 against the same-source control (all
rows and conditions in [alloc-vs-pool](/project/alloc-vs-pool.md)).

- **Target** `script:script@template_release`: 2.311 to 2.357 ms, +2.4%,
  2 of 11 rounds faster, WITHIN-NOISE (floor 6.94%). Editor -0.6%.
  `scene:instance:10000` template_release -10.5% (regression rule only).
- **Verdict: FAIL**, 82 REGRESSION rows: copying an empty map
  (`hashmap_sname_copy_0`, `hashmap_variant_copy_0`, +43.6 to +50.3%),
  `dict_*` and `hashmap_*` hit and miss rows (+2.3 to +22.7%), gdbench
  `array.*` and `string_manipulation.*` cases that do not use HashMap in
  their timed loops (+2.2 to +36.4%, code layout of a larger inlined
  HashMap the likely cause), `scene:script:10000` template_release +3.5%,
  and 13 `peak_ws_mb` rows (`editor_startup` +12.9%, scene and script
  +1.1 to +3.2%).
- **Counts:** allocations fall everywhere (`benchmark_duplicate_*_100k`
  100,003 to 4, level load 1.12M to 1.04M); 26 ALLOC-INCREASE rows, all
  `reallocs`, from the page table that `_pool_add_page()` grows with
  `Memory::realloc_static` (explained, not a rejection).
- **Tiny maps (D-05):** the live numbers match the smoke above exactly:
  `live_bytes` +9.4% at 2 elements and +48% at 8, equal at 1 and 3;
  allocations -20% at 3 and -30% at 8. Builds -16% at 3 and 8 elements,
  within +8% at 1 and 2. The pool raises tiny-map bytes and does not win
  time on its target, so neither half of D-05's trade is in its favor.
- **Both series** (with mimalloc): target -23.9% against alloc's -23.1%,
  FAIL (62 rows); the pool's `copy_0` and `miss` regressions stay in it.

Not in the provisional MSVC baseline (empty).

# Results (GCC, Clang)

Not measured (user decision 2026-11-23): acceptance needs every compiler,
so an MSVC rejection is final.

# Decision (final)

**Rejected**, tier: FAIL (engine) in ALLOC-02: WITHIN-NOISE on its target, 82 REGRESSION rows.

Code: last at `e2c08be5d2`, deleted in Phase 3 final.
