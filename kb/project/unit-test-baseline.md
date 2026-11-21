---
type: Finding
title: Unit-test baseline per stage of the groundwork phase
description: Full --test results of the tests build at each stage (pre-sync first), so a later failure is compared with the stage before it; pre-sync 143b8865d6 passes 1482/1482, post-sync, post-NVIDIA and C++20 (MSVC) 1497/1497, contracts (MSVC) 1564/1564, 3 skipped each; the bench cases (MSVC) add 10 skipped [Bench] cases, 1564/1564 with 13 skipped (6 cases, 9 skipped, since 02-10).
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

# Smoke on the same tree

`devtools/test/smoke.ps1` passed on the editor build of the same commit:
PhysX game and editor (`PhysX 5.10.0 initialized.`, CPU only on this
machine), path tracer on Vulkan (noisy path-traced room), DDGI interior.

# Covers

* `tests/` (the suite itself)
* `devtools/build/build.ps1` (`-Tests` builds the binary with its own
  objects, so it never invalidates the production build)
