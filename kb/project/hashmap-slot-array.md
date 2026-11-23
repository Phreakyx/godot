---
type: Finding
title: One {hash, node index} slot array for HashMap (HASH-02 candidate)
description: Rejected by the D-10 gate before building. The design replaces HashMap's two index arrays (4-byte hash plus 8-byte node pointer, 12 bytes per slot) with one 8-byte Slot { hash, node } array, the node being the stable index of the per-map node pool. It needs that pool; the provisional MSVC baseline of ALLOC-02 is empty, and without a stable node index every layout raises the index bytes or adds a second index, which is what sank the 2026-11-15 attempt. Phase 3 final: rejected, never built.
tags: [core, containers, hashmap, memory, layout, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T00:00:00Z }
sources:
  - id: hashmap
    resource: ../../core/templates/hash_map.h
    title: _hashes and _elements, and the pool's _pool_node() decode
  - id: baseline
    resource: alloc-vs-pool.md
    title: ALLOC-02 "Decision (MSVC, provisional)", baseline empty
  - id: storage
    resource: core-template-storage.md
    title: The 2026-11-15 packed storage and robin-hood index, dropped
  - id: pool
    resource: hashmap-node-pool.md
    title: The per-map node pool (HASH-01) and its stable node indices
---

# Design and the 2026-11-15 result

Today a HashMap slot lives in two arrays: `_hashes` (4 bytes) and
`_elements` (an 8-byte node pointer), 12 bytes per slot, and a probe that
finds a matching hash touches a second cache line for the pointer. The
candidate keeps one array of `Slot { uint32_t hash; uint32_t node; }`, 8 bytes
per slot, so a probe reads one cache line and the index shrinks by a third.

- **No second index.** The slot is still the only index; the in-tree
  robin-hood loop (`_lookup_idx_unchecked()`, `_insert_element()`, backward
  shift in `erase()`) is kept and moves slots, never nodes.
- **No per-entry side state.** The node keeps its `next`/`prev` links and
  data and nothing else; no back-pointer to its slot, no dense array.
- **Per-entry bytes unchanged,** index bytes 12 to 8 per slot.
- **The node index comes from the pool** (HASH-01, [node pool](hashmap-node-pool.md)):
  node `i` is the `i`-th node the bump pointer handed out and never moves, so
  `_pool_node(i)` decodes it to (page, offset) in O(1). The free list must
  then hand back nodes whose index is known (03-03's open item: store node
  indices in the free list, or decode the freed pointer).

The 2026-11-15 attempt ([packed storage](core-template-storage.md)) added a
shared robin-hood index next to the containers' own storage, with packed
`{hash, next}` entries: a second index and per-entry bookkeeping, and
integer-keyed maps came out 1.3-2.6x slower with 50-57% more live bytes. This
design only shrinks the existing index.

The D-10 micro gate it would have to pass in 03-10: no time REGRESSION on
`hashmap_i64` and `hashmap_ptr` insert, hit, miss, erase and churn at N 16
to 1M, and `live_bytes` lower at every N from 16 up.

# Decision: rejected by the D-10 gate before building

The design needs `FORK_BACKEND_HASHMAP_POOL=1` in the baseline (D-08). The
provisional MSVC baseline of ALLOC-02 is empty
([decision](alloc-vs-pool.md)): the pool was within noise on its target and
failed on 82 REGRESSION rows. Without the pool, a node is one allocation of
its own and has no stable small index, so the only layouts left are:

- **Interleaved `{hash, pad, pointer}` slot:** 4 + 4 + 8 = 16 bytes per slot
  instead of 12, +33% index bytes. One cache line per probe, but more bytes at
  every N.
- **Dense node-pointer array with swap-on-erase:** an 8-byte `{hash, index}`
  slot plus an 8-byte pointer per element in a dense array; at 0.75 occupancy
  that is 8 + 0.75 x 8 = 14 bytes per slot, +17%, and more once the dense
  array's own spare capacity is counted. It is also a second index, and
  swap-on-erase must find the moved element's slot again (one more probe, or
  a back-index per entry).

Both raise `live_bytes`, which the gate forbids, and the second repeats the
2026-11-15 shape. No code was written. If a later baseline (GCC and Clang,
03-11) takes the pool in, this entry is the design to build.

# Rules stated before measuring (MSVC)

Written before the first 03-10 series. HASH-02 has no binary: it was
rejected before building (above), so 03-10 does not measure it and records
it as "not measured". If a later baseline takes the pool in and the
candidate is built as `slots` (`FORK_BACKEND_HASHMAP_SLOTS=1`), these are
its rules.

- **Micro gate (D-10):**
  `python devtools/bench/bench.py micro --base-defines "<BASE>" --name slots --defines "FORK_BACKEND_HASHMAP_SLOTS=1" --prev 71b2819fec --out devtools/out/bench/hash-micro-slots`.
  No `hashmap_i64` or `hashmap_ptr` row of `insert`, `hit`, `miss`,
  `erase` or `churn` at N 16 and up reads REGRESSION against the
  same-source control, and `live_bytes` is lower than the control's at each
  of those rows. If either fails, the result is FAIL (gate). The entry
  records the `LAYOUT-SENTINEL: <k> of <m> control rows moved` line. If k
  is above 0, the series is repeated once with the same command and both
  lines are recorded. The gate is then read on the repeat.
- **Engine series (D-09), only after the gate passes:**
  `python devtools/bench/bench.py compare --skip-build --base-defines "<BASE>" --name slots --defines "FORK_BACKEND_HASHMAP_SLOTS=1" --prev 71b2819fec --target scene:reparent:10000,scene:script:10000 --workloads all --out devtools/out/bench/hash-slots`,
  then
  `bench.py verdict --series devtools/out/bench/hash-slots --rows scene:reparent:10000@editor,scene:script:10000@template_release`
  must print PASS. Every `ALLOC-INCREASE` or `INCREASE` count row is
  explained here. A series that refuses, or that has more than a fifth of
  its rounds clock-flagged, is repeated.

# Results (MSVC)

**Not measured.** HASH-02 has no binary (rejected before building,
above), so 03-10 ran no micro or engine series for it. Its rules above
stand for a later baseline that takes the node pool in.

# Results (GCC, Clang)

Not measured (user decision 2026-11-23): acceptance needs every compiler,
so an MSVC rejection is final.

# Decision (final)

**Rejected**, tier: rejected by the D-10 gate before building (needs the node pool, which ALLOC-02 left out).

Code: none was ever written (rejected before building); nothing to delete in
Phase 3 final.
