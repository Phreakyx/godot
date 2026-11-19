---
type: Finding
title: Unit-test baseline per stage of the groundwork phase
description: Full --test results of the tests build at each stage (pre-sync first), so a later failure is compared with the stage before it; pre-sync 143b8865d6 passes 1482/1482 with 3 skipped.
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

The three skipped cases are skipped on purpose in the source, not by this
build: `Validate tests / Pending tests are skipped`
(`tests/core/test_validate_testing.cpp`, `TEST_CASE_PENDING`) and two DDS
saver cases marked `doctest::skip(true)` in `modules/dds/tests/test_dds.h`.

The log also holds three `ERROR: Drawing is only allowed inside this
node's _draw() ...` lines from `CanvasItem::draw_texture_rect_region`.
They are engine error prints during a test, not failed checks; the suite
still reports 0 failed. A later stage that changes their count is worth a
look, not a regression by itself.

# Smoke on the same tree

`devtools/test/smoke.ps1` passed on the editor build of the same commit:
PhysX game and editor (`PhysX 5.10.0 initialized.`, CPU only on this
machine), path tracer on Vulkan (noisy path-traced room), DDGI interior.

# Covers

* `tests/` (the suite itself)
* `devtools/build/build.ps1` (`-Tests` builds the binary with its own
  objects, so it never invalidates the production build)
