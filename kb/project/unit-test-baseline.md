---
type: Finding
title: Unit-test baseline per stage of the groundwork phase
description: Full --test results of the tests build at each stage (pre-sync first), so a later failure is compared with the stage before it; pre-sync 143b8865d6 passes 1482/1482, post-sync, post-NVIDIA and C++20 (MSVC) 1497/1497, contracts (MSVC) 1564/1564, 3 skipped each; the bench cases (MSVC) add 10 skipped [Bench] cases, 1564/1564 with 13 skipped (6 cases, 9 skipped, since 02-10); contracts (GCC) and (Clang) on CachyOS 1570/1570 with 9 skipped, [Fork] 67/67 on GCC, Clang and the GCC dev build. Phase 3 final (MSVC): 1595/1595, 9 skipped; on Linux pending.
tags: [testing, baseline, phase-1]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-19T23:21:34Z }
---

# What it records

The whole unit-test suite (`--test`, Godot's doctest harness in `tests/`)
run on the tests build at each stage of the groundwork phase: the pre-sync
tree, then each later stage (upstream sync, compiler and language switches).
A failure at one stage is compared with the stage before it, so an old
failure is never taken for a new regression, and a new one is never written
off as old. No full `--test` run of this fork had been recorded before.

# Conditions

* Commit: `143b8865d6` (`dev`, release 0.10.3 plus its kb entry).
* Build: `devtools/build/build.ps1 -Tests`, which runs
  `-m SCons -j32 platform=windows target=editor production=yes tests=yes object_prefix=t_ extra_suffix=tests`
  through the Python that has SCons (`Get-SconsPython` in `devtools/common.ps1`).
  Output: `bin/godot.windows.editor.x86_64.tests.console.exe`.
* Compiler: Visual Studio 2026 (18.11.0), MSVC 14.51.36231, Windows SDK
  10.0.26100.0. PhysX CPU only (`physx_gpu=no`, no CUDA Toolkit), Blast and
  Flow on; AccessKit and ANGLE dependencies not installed.
* Machine: Intel Core i9-14900K (32 threads), 64 GB RAM, NVIDIA GeForce
  RTX 4080 SUPER (driver 621.14), Windows 11 IoT Enterprise LTSC 10.0.26100.
* Run: `bin/godot.windows.editor.x86_64.tests.console.exe --test`, about
  34 s wall time, exit code 0.

# Results

| Stage | Commit | Test cases | Passed | Failed | Skipped | Assertions | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| pre-sync | `143b8865d6` | 1482 | 1482 | 0 | 3 | 490282 passed, 0 failed | `SUCCESS!` |
| post-sync | `2b7e93d07e` | 1497 | 1497 | 0 | 3 | 594617 passed, 0 failed | `SUCCESS!` |
| post-NVIDIA | `bdb3398d56` | 1497 | 1497 | 0 | 3 | 594617 passed, 0 failed | `SUCCESS!` |
| C++20 (MSVC) | `dev` after `6f3534edba` | 1497 | 1497 | 0 | 3 | 594617 passed, 0 failed | `SUCCESS!` |
| contracts (MSVC) | `dev` after `c7dc4f6683` | 1564 | 1564 | 0 | 3 | 594947 passed, 0 failed | `SUCCESS!` |
| bench cases (MSVC) | `dev` after `d43457e8b2` | 1564 | 1564 | 0 | 13 | 594947 passed, 0 failed | `SUCCESS!` |
| contracts (GCC) | `7b81582fab` + fix | 1570 | 1570 | 0 | 9 | 595031 passed, 0 failed | `SUCCESS!` |
| contracts (Clang) | `7b81582fab` + fix | 1570 | 1570 | 0 | 9 | 595022 passed, 0 failed | `SUCCESS!` |
| fork tests (GCC dev) | `7b81582fab` + fix | 67 | 67 | 0 | 1512 | 329 passed, 0 failed | `SUCCESS!` |
| Phase 3 final (MSVC) | `dev` after `e2c08be5d2` | 1595 | 1595 | 0 | 9 | 601969 passed, 0 failed | `SUCCESS!` |

Doctest summary of the pre-sync run, verbatim:

```
[doctest] test cases:   1482 |   1482 passed | 0 failed | 3 skipped
[doctest] assertions: 490282 | 490282 passed | 0 failed |
[doctest] Status: SUCCESS!
```

# Failing cases

| Stage | Case | Message | Cause | Flaky |
| --- | --- | --- | --- | --- |
| pre-sync | none | | | |
| post-sync | none | | | |
| post-NVIDIA | none | | | |
| C++20 (MSVC) | none | | | |
| contracts (MSVC) | none | | | |
| bench cases (MSVC) | none | | | |
| contracts (GCC) | none | | | |
| contracts (Clang) | none | | | |
| fork tests (GCC dev) | none | | | |

The three skipped cases are skipped on purpose in the source, not by this
build: `Validate tests / Pending tests are skipped`
(`tests/core/test_validate_testing.cpp`, `TEST_CASE_PENDING`) and two DDS
saver cases marked `doctest::skip(true)` in `modules/dds/tests/test_dds.h`.

The log also holds three `ERROR: Drawing is only allowed inside this
node's _draw() ...` lines from `CanvasItem::draw_texture_rect_region`.
They are engine error prints during a test, not failed checks; the suite
still reports 0 failed. A later stage that changes their count is worth a
look, not a regression by itself.

The post-sync tree (Godot 4.8-beta4 merge and PhysX module `1302f909ca`,
see [the sync](/project/fork-sync-2026-11.md)) adds 15 cases, all
upstream's new tests (`tests/core/templates/test_sort_array.cpp`, and
additions to `tests/core/io/test_scenes.cpp` and
`tests/scene/test_button.cpp`). No case failed; the same three are
skipped, and the same three `ERROR: Drawing is only allowed ...` prints
appear. Same build command and machine, about 20 s wall time.

The post-NVIDIA tree (NVIDIA groups N1 and N2, see
[the sync](/project/fork-sync-2026-11.md)) adds no test case and changes
no result: same counts, same three skipped, same three `ERROR: Drawing is
only allowed ...` prints, about 21 s wall time.

The C++20 (MSVC) tree ([the C++20 switch](/project/cpp20-switch.md),
`/std:c++20`, every C++ file recompiled) changes no result either: same
counts, same three skipped, same three prints.

The contracts (MSVC) tree ([container contracts](/project/container-contracts.md))
adds 67 `[Fork]` cases in `tests/fork/` (43 for the hash, tree and list
containers, 20 for CowData, String, Packed arrays, Array, Dictionary and
StringName, 4 layout reads) and one GDScript script under
`modules/gdscript/tests/scripts/runtime/fork/`. 1564 cases (1497 plus 67),
0 failed, the same three skipped and the same three prints; no case that
passed before fails. Log: `devtools/out/tests/contracts-msvc.log`.

The benchmark cases (`dev` after `d43457e8b2`, plan 02-07) add 10
`[Bench]` cases in `tests/fork/test_fork_bench_variant_keys.cpp`, every
one `doctest::skip()` by design: `bench.py` runs them as workload
`micro_engine` with `--test --test-case="*Bench*" --no-skip` on the bench
tests builds (see the [benchmark protocol](/tooling/benchmark-protocol.md)).
So the skipped total grows by 10 and nothing else changes: `--test`
reports 1564 test cases, 1564 passed, 0 failed, 13 skipped (594947
assertions, the same as before), and `--test --test-case="*Fork*"` still runs the 67
`[Fork]` cases (no `[Bench]` name contains "Fork"). `--test
--test-case="*Bench*"` alone reports 0 passed, 0 failed, 1577 skipped.
Plan 02-10 merged them into 6 cases (one StringName key set for HashMap,
AHashMap and Dictionary, one name set for the three StringName
operations; the same 298 `BENCH` lines): `--test --test-case="*Bench*"`
now reports 1573 skipped, and with `--no-skip` 6 passed, so the full
`--test` skips 9 instead of 13.

The allocator audit (plan 03-02, [allocator audit](/project/allocator-audit.md))
adds five `[Fork][Memory]` cases in `tests/fork/test_fork_memory.cpp`: MSVC
`--test` now reports 1569 cases, 1569 passed, 0 failed, 9 skipped (594982
assertions) on the production and dev tests builds. The `template_release`
tests build, first recorded here, reports 1545, 1545 passed, 0 failed, 9
skipped (486685 assertions); it has no editor tests and no TOOLS_ENABLED
case. The `MI_DEBUG=1` allocator candidate gives the same counts in all three.

The HashMap node pool (plan 03-03, [node pool](/project/hashmap-node-pool.md))
adds seven `[Fork][HashMap]` cases (one of them only
with `DEBUG_ENABLED`): MSVC `--test` now reports 1576 cases, 1576 passed, 0
failed, 9 skipped (595396 assertions) on the production control, the
production `FORK_BACKEND_HASHMAP_POOL=1` build and the dev pool build;
`--test-case="*Fork*"` runs 79 cases (778 assertions).

The public hash outputs pin and the HASH-04 and HASH-05 candidates (plan
03-07, [table key hash](/project/table-key-hash.md),
[rapidhash](/project/rapidhash-byte-hash.md)) add three `[Fork][Hash]`
cases: MSVC `--test` reports 1584 cases, 1584 passed, 0 failed, 9 skipped
(601388 assertions) on the production control, `tkh`
(`FORK_BACKEND_TABLE_KEY_HASH=1`), `rapid` (`FORK_BACKEND_BYTE_HASH=1`) and
`hashpow2` (both with `FORK_BACKEND_HASH_POW2=1`);
`--test-case="*Fork*"` runs 87 cases (6725 assertions) on the dev `tkh` and
`rapid` builds.

The HASH-06 candidate (plan 03-08, [tag probing](/project/hash-simd-tags.md))
adds three `[Fork][AHashMap]` and three `[Fork][HashSet]` cases (two of them
only with `DEBUG_ENABLED`), and `[Fork][HashTags]` in tag builds only: MSVC
`--test` reports 1590 cases, 1590 passed, 0 failed, 9 skipped (601808
assertions) on the production control, and 1591 (601809 assertions) on
`tags` (`FORK_BACKEND_HASH_TAGS=1`) and `tagsall` (with
`FORK_BACKEND_HASH_POW2=1`, `FORK_BACKEND_TABLE_KEY_HASH=1` and
`FORK_BACKEND_BYTE_HASH=1`); `--test-case="*Fork*"` runs 93 cases on the
control and 94 (7146 assertions) on the dev `tags` build.

The HASH-07 candidate (plan 03-09, [small map](/project/hashmap-small-map.md))
adds five `[Fork][HashMap]` threshold cases: MSVC `--test` reports 1595
cases, 1595 passed, 0 failed, 9 skipped (601973 assertions) on the
production control and on `small8` (`FORK_BACKEND_HASHMAP_SMALL=8`), and
1597 (601975 assertions) on `allhash` (every Phase 3 macro on:
`FORK_BACKEND_ALLOCATOR=1`, `HASHMAP_POOL=1`, `HASH_POW2=1`,
`TABLE_KEY_HASH=1`, `BYTE_HASH=1`, `HASH_TAGS=1`, `HASHMAP_SMALL=8`; the two
extra cases are `[Fork][HashTags]` and `[Fork][Backends] Allocator scope`);
`--test-case="*Fork*"` runs 98 cases (7310 assertions) on the control and
the dev `small8` build, and 100 (7312) on the dev `allhash` build.

Phase 3 final (plan 03-13, 2026-11-23): nothing was accepted, so the
defaults stay 0; the node pool, power-of-two, rapidhash and tag candidates
are deleted (with their `[Fork][Backends]` lines and `[Fork][HashTags]`),
mimalloc, the table-key hash and the small map are kept off by default.
MSVC `--test` on the production tests build: 1595 cases, 1595 passed,
0 failed, 9 skipped (601969 assertions), the same cases as the last control
(1595, 9 skipped); the four fewer assertions are the deleted
`[Fork][Backends]` checks. `--test-case="*Fork*"`: 98 cases (7306
assertions) on the production and dev tests builds, 92 (7219) on the
template_release tests build; the `keep3` tests build
(`FORK_BACKEND_ALLOCATOR=1`, `FORK_BACKEND_TABLE_KEY_HASH=1`,
`FORK_BACKEND_HASHMAP_SMALL=8`) runs 99 (7307, with
`[Fork][Backends] Allocator scope`) and its full suite 1596, 1596 passed,
9 skipped (601970). The public outputs pin (`*Fork*Hash*outputs*`, 3
cases) passes on the production and `keep3` builds.

The Linux rows ([Linux builds](/project/linux-build.md)) ran on CachyOS on
the same PC: GCC 16.2.1 and Clang 23.1.5, `production=yes tests=yes`
builds, at `7b81582fab` plus the `-> uint64_t` fix in
`tests/core/templates/test_hash_set.cpp` that GCC needed. They have six
more cases than MSVC, upstream's `[UDSServer]` cases (built only with
`UNIX_ENABLED`); the same nine are skipped. `--test-case="*Fork*"` runs the
67 `[Fork]` cases on GCC, Clang and the GCC `dev_build=yes` build
(`DEV_ASSERT` compiled in): 67 passed, 329 assertions each.

**Phase 3 final tree on Linux: pending.** The Phase 3 final commit has
not been built or tested on GCC or Clang yet. The next CachyOS session
tests exactly this commit (GCC and Clang builds, `--test` with the full
suite and `*Fork*`, the GCC dev tests build's `*Fork*` cases,
`devtools/test/smoke_linux.sh`), and 03-11 Task 3 records the result
here. Until then nothing here says Linux passed on it.

# Smoke on the same tree

`devtools/test/smoke.ps1` passed on the editor build of the same commit:
PhysX game and editor (`PhysX 5.10.0 initialized.`, CPU only on this
machine), path tracer on Vulkan (noisy path-traced room), DDGI interior.

# Covers

* `tests/` (the suite itself)
* `devtools/build/build.ps1` (`-Tests` builds the binary with its own
  objects, so it never invalidates the production build)
