---
type: Finding
title: mimalloc under Memory:: (ALLOC-01 candidate)
description: mimalloc 3.7.0, vendored and compiled from one guarded C file, called directly by Memory::alloc_static, realloc_static, free_static and the aligned functions when FORK_BACKEND_ALLOCATOR is 1; nothing is overridden, the DEBUG_ENABLED accounting and the allocation counters are unchanged, and the mismatch audit passed before any measurement. On MSVC (ALLOC-02) it is 23% faster on script:script template_release but fails on hash-lookup and peak-memory regressions; GCC and Clang follow. Phase 3 final: rejected; code kept, off by default, revised in Phase 3.1.
tags: [core, memory, allocator, mimalloc, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: readme
    resource: ../../thirdparty/README.md
    title: mimalloc section (version, commit, license, files)
  - id: wrapper
    resource: ../../core/os/fork_mimalloc.c
    title: The guarded translation unit and its pinned switches
  - id: memory
    resource: ../../core/os/memory.cpp
    title: The _fork_sys_* helpers
  - id: switch
    resource: ../../core/templates/fork_backends.h
    title: FORK_BACKEND_ALLOCATOR (default 0)
  - id: upstream
    resource: https://github.com/microsoft/mimalloc/tree/v3.7.0
    title: mimalloc v3.7.0 (tag commit 1ed67b5156a763441d2ec3c20221a7b3cd866129)
---

# What it is

- **Version:** mimalloc v3.7.0, commit `1ed67b5156a763441d2ec3c20221a7b3cd866129`
  (the `^{}` of the annotated tag), MIT, in `thirdparty/mimalloc/` (only
  `include/`, `src/` and `LICENSE`; no local patch).
- **Why v3, not v2:** the project recommends v3 and calls v2 "stable, legacy";
  v3 still has the single-file build (`src/static.c`). v2.5.3 shipped the same
  fixes the same day and is the fallback, after v3.6.1.
- **Direct calls, no override (D-01):** with `FORK_BACKEND_ALLOCATOR=1`
  (`core/templates/fork_backends.h`, default 0), `core/os/memory.cpp` calls
  `mi_malloc`, `mi_calloc`, `mi_realloc` and `mi_free` in four `_fork_sys_*`
  helpers. `malloc`, `free`, `operator new` and `operator delete` stay the C
  runtime's; `fork_mimalloc.c` refuses `MI_MALLOC_OVERRIDE`. Aligned
  allocations keep Godot's own offset header on top of `mi_malloc`, so
  `realloc_aligned_static` and its `p_prev_bytes` copy are unchanged.
- **The wrapper and why:** `core/os/fork_mimalloc.c` includes `src/static.c`
  only when the macro is 1 and is empty otherwise, so the control binary links
  no mimalloc code. Once linked, mimalloc's Windows TLS callbacks run on every
  thread attach and detach, which would change the control. SCons compiles the
  file unconditionally (`core/SCsub`, its own `env_mimalloc` with the include
  path), so the default can later change in the header alone.
- **Pinned switches (Pitfall 2):** without `NDEBUG`, which every `dev_build`
  lacks, mimalloc compiles `MI_DEBUG 2`, statistics and guarded sampling. The
  wrapper pins `MI_DEBUG`, `MI_SECURE`, `MI_STATS`, `MI_PROFILE` and
  `MI_GUARDED` to 0 unless a `-Defines` value sets them, so dev and production
  builds get the same allocator. The audit variant sets `MI_DEBUG=1`.
- **0-byte realloc (Pitfall 1):** `mi_realloc(p, 0)` returns a live zero-size
  block, while the C runtime frees and returns `nullptr`, which
  `realloc_static` relies on in release. `_fork_sys_realloc` frees and returns
  `nullptr` for 0; `[Fork][Memory]` pins it (`tests/fork/test_fork_memory.cpp`).
- **Accounting unchanged (D-03):** the prepad header, `get_mem_usage()`, the
  maximum-usage tracking, the `FORK_ALLOC_COUNTERS` calls and the
  `GodotProfileAlloc`/`Free` calls are byte for byte upstream's, so allocation
  counts mean the same on both sides of a compare.
- **Purge delay and transparent huge pages (Pitfall 8):** left at mimalloc's
  defaults (`purge_delay` 1000 ms in v3; `allow_thp` full on Linux). Peak
  memory is judged on `peak_ws_mb`; if it regresses, a tuned option is a second
  candidate value of the same macro, not a hidden setting.
- **Safety:** the [mismatch audit](/project/allocator-audit.md) found no block
  crossing the `Memory::` boundary to another allocator; with `MI_DEBUG=1` the
  full unit tests and the smoke test pass in production, template_release and
  dev builds. It found and fixed a double free and an uninitialized read
  that are bugs under any allocator (the emoji and bracket stacks in
  `modules/text_server_adv/script_iterator.cpp`).

# Results (MSVC)

ALLOC-02 on 2026-11-22, value 1 against the same-source control, every
suite workload on editor and template_release, then the micro tier
(conditions and every row in [alloc-vs-pool](/project/alloc-vs-pool.md)).

- **Target** `script:script@template_release`: 2.315 to 1.772 ms, -23.1%,
  11 of 11 rounds faster, ACCEPT (floor 6.94%). Editor -12.1%.
  `scene:instance:10000` template_release -38.5% and level `load_ms`
  -12.8% (regression rule only on those rows).
- **Verdict: FAIL**, 47 REGRESSION rows. 30 are time rows on hash lookups
  that allocate nothing in their timed loop (`hashmap_sname_miss`,
  `dict_*_hit` and `_miss`, `sname_from_string`, +2.8 to +28.8%, mostly
  editor) and three gdbench cases (`gdbench_dictionary` string iterate and
  lookup on editor +26.6%/+18.1%, `signal.benchmark_emission_params_10`
  +16.0%). The other 17 are `peak_ws_mb`: gdbench +71.6%/+55.5%, the level
  +2.7-2.9%, scene and script workloads +1.2 to +4.9%. Freed memory that
  mimalloc keeps for its purge delay is the likely share of the gdbench
  peak (Pitfall 8).
- **Counts** identical to the control on every deterministic workload
  (D-03 holds); the level's nondeterministic counts overlap the control's.
- **Both series** (with the pool): target -23.9%, also FAIL (62 rows).
- **Micro:** tiny HashMap builds 46-47% faster, tiny-map bytes unchanged;
  third-party `ctl_*_insert` rows, which allocate through the C runtime
  and not through mimalloc, read slower (+7.9 to +161%) and 16 of 256
  control rows moved. Not explained yet.
- **Step 5 not taken:** the purge-delay value 2 is for a failure through
  peak memory alone; this one also fails on time rows.

Not in the provisional MSVC baseline (empty).

# Results (GCC, Clang)

Not measured (user decision 2026-11-23): acceptance needs every compiler,
so an MSVC rejection is final.

# Decision (final)

**Rejected**, tier: FAIL (engine) in ALLOC-02: ACCEPT on its target, 47 REGRESSION rows (hash lookups, gdbench, peak memory).

Rejected on MSVC (Phase 3); code kept, off by default, revised in Phase 3.1 (ALLOC-03, HASH-08, HASH-09).
