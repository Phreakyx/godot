---
type: Finding
title: mimalloc under Memory:: (ALLOC-01 candidate)
description: mimalloc 3.7.0, vendored and compiled from one guarded C file, called directly by Memory::alloc_static, realloc_static, free_static and the aligned functions when FORK_BACKEND_ALLOCATOR is 1; nothing is overridden, the DEBUG_ENABLED accounting and the allocation counters are unchanged, and the mismatch audit passed before any measurement. Results are filled in by the ALLOC-02 compares (MSVC, then GCC and Clang).
tags: [core, memory, allocator, mimalloc, phase-3]
status: draft
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

Not measured yet (ALLOC-02, four-way compare on `script:script`
template_release and the suite).

# Results (GCC, Clang)

Not measured yet (the combined CachyOS session).
