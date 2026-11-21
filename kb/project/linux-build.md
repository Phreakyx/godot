---
type: Finding
title: Linux builds, tests and smoke on CachyOS (GCC and Clang)
description: The groundwork tree 7b81582fab builds on CachyOS with GCC 16.2.1 and Clang 23.1.5 (all five builds), passes --test 1570/1570 on both, the 67 [Fork] cases on GCC, Clang and the dev build, and the Linux smoke test; one GCC-only test compile error and one Clang warning flood were fixed, and the smoke now runs the editor before the game.
tags: [build, linux, gcc, clang, testing, phase-1]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T22:00:00Z }
sources:
  - id: setup
    resource: ../../devtools/setup/setup_dev_cachyos.sh
    title: CachyOS setup script (tools, remotes, SDKs, custom.py, the five builds)
  - id: smoke
    resource: ../../devtools/test/smoke_linux.sh
    title: Linux smoke test
---

# Conditions

* Commit: `7b81582fab` (`dev`), the tree that passed the Windows push gate
  before the session; the fixes below were applied locally on Linux and
  committed afterwards from Windows.
* Machine: the same PC as the MSVC numbers, dual-booted. `lscpu`: Intel
  Core i9-14900K; BIOS 1.K0, microcode 0x137.
* System: CachyOS, kernel 7.3.5-2-cachyos, GCC 16.2.1 20260810, Clang
  23.1.5 (lld), SCons 4.11.1 from pacman, Python 3.14.10, git 2.56.1.
  The compilers were held with `IgnorePkg` for the session.
* File system: the repository, the SDKs and the builds on the ext4 root
  partition (grown to 159 GB for the session). A first try on an ext4 image
  file kept on the NTFS drive failed mid-build: see
  [the mistake](/mistakes/ext4-image-on-ntfs-build-disk.md).

# Builds

`devtools/setup/setup_dev_cachyos.sh --build` checked the tools, set the
remotes, built CPU PhysX, Blast and Flow at `ovphysx-0.5.11` (no CUDA),
wrote `custom.py`, and ran the five builds of
[Build (Linux)](../../CUSTOM_BUILD.md#build-linux):

| Build | Binary | Result |
| --- | --- | --- |
| GCC editor (`production=yes`, full LTO) | `bin/godot.linuxbsd.editor.x86_64` | OK |
| Clang editor (`use_llvm=yes linker=lld`, thin LTO) | `bin/godot.linuxbsd.editor.x86_64.llvm` | OK |
| GCC tests | `bin/godot.linuxbsd.editor.x86_64.tests` | OK after fix 1 |
| Clang tests | `bin/godot.linuxbsd.editor.x86_64.llvm.tests` | OK (built after fix 1) |
| GCC `dev_build=yes tests=yes` | `bin/godot.linuxbsd.editor.dev.x86_64` | OK |

`production=yes` turns on LTO on Linux (full with GCC, thin with Clang),
unlike MSVC, where it stays off. The dev build has no `extra_suffix`, so
its name has no `.tests`; `.llvm` comes before `.tests`.

All eight bundled PhysX SDK patches were applied and built, including
0003 (linux-clang-no-werror), 0005 (blast-linux-host-gcc) and 0007
(flow-linux-arch); checked with `git apply --reverse --check`. The script's
untested assumptions held: `pacman -T libpulse` exits 0 with
pipewire-pulse installed, Blast lands in
`blast/_build/linux-x86_64/release/blast-sdk`, `libnvflow.so` in
`flow/_build/linux-x86_64/release/`, and the NTFS warning fires on an NTFS
mount (`stat -f` reports `ntfs`).

## Warnings

* GCC and Clang print the same 17 C++20 deprecations, all
  `[=]` lambdas that capture `this` implicitly in
  `modules/godot_physx/godot_physx_server_3d.cpp` (GCC `-Wdeprecated`,
  Clang `-Wdeprecated-this-capture`). Review finding WR-06 of the
  groundwork phase is confirmed and left open: the module is vendored, and
  only a `werror` build fails on it. No other C++20 deprecation
  (enum arithmetic, `volatile`) appears. See
  [the C++20 switch](/project/cpp20-switch.md).
* Clang printed 762 `-Winconsistent-missing-override` lines from
  `servers/rendering/renderer_scene_cull.h`: the fork's planar reflection
  work had marked `free_mirror_views()` `override` in a class whose other
  overrides are not marked, so Clang flagged every other one in each file
  that includes it. Fixed (fix 2).

# Tests

| Run | Test cases | Passed | Failed | Skipped | Assertions |
| --- | --- | --- | --- | --- | --- |
| GCC `--test` | 1570 | 1570 | 0 | 9 | 595031 |
| Clang `--test` | 1570 | 1570 | 0 | 9 | 595022 |
| GCC `--test-case="*Fork*"` | 67 | 67 | 0 | 1512 | 329 |
| Clang `--test-case="*Fork*"` | 67 | 67 | 0 | 1512 | 329 |
| GCC dev `--test-case="*Fork*"` | 67 | 67 | 0 | 1512 | 329 |
| GCC dev `*Script compilation and runtime*` | 1 | 1 | 0 | 1578 | 513 |

Compared with MSVC (1564 cases, 9 skipped, 594947 assertions; see
[the unit-test baseline](/project/unit-test-baseline.md)): the six extra
cases are upstream's `[UDSServer]` cases
(`tests/core/io/test_uds_server.cpp`), compiled only with `UNIX_ENABLED`.
GCC and Clang differ by 9 assertions; the cause was not looked into.
Every `[Fork]` case passes on both compilers and with `DEV_ASSERT` compiled in, so the
[container contracts](/project/container-contracts.md) and the C# layout
reads hold with the Linux offsets (`DATA_OFFSET` 32). The GDScript fork
script under `modules/gdscript/tests/scripts/runtime/fork/` passes in the
dev build.

The logs also print `ERROR: Connection to remote host failed!` (the UDS case
"When stopped shouldn't accept new connections", which expects it) and
the same three `ERROR: Drawing is only allowed ...` lines as on MSVC. Both
are engine prints during a test, not failed checks.

# Smoke

`devtools/test/smoke_linux.sh` passed 4 of 4 (GCC and Clang editors, game
and editor on Vulkan, `PhysX 5.10.0 initialized.`) on its second run. On
the first, the GCC game check failed with `Failed to load script
"res://demo/cpu/physx_playground.gd" with error "Parse error"`: a fresh
clone of the example project has no global class cache, and the script ran
the game before the editor that writes it. Both smoke scripts now run the
editor first (fix 3).

# Fixes

1. `tests/core/templates/test_hash_set.cpp`: the `uint64_t` key lambda of
   "Keys of different sizes and alignments through growth" returned
   `unsigned long long`, which does not convert to the `uint64_t (*)(int)`
   parameter where `uint64_t` is `unsigned long` (LP64 Linux; on Windows
   both are `unsigned long long`). GCC stopped on it. The lambda now says
   `-> uint64_t`.
2. `servers/rendering/renderer_scene_cull.h`: `free_mirror_views()` loses
   its `override`, as upstream marks none in this class.
3. `devtools/test/smoke_linux.sh` and `devtools/test/smoke.ps1`: the editor
   check runs before the game check.

The measurement half of the same session (noise floors, counters and
profiles on GCC and Clang) is recorded in
[the noise floors](/project/benchmark-noise-floors.md) and
[the family shares](/project/family-shares.md).

# Covers

* `devtools/setup/setup_dev_cachyos.sh`, `devtools/test/smoke_linux.sh`,
  `devtools/test/smoke.ps1`
* `tests/core/templates/test_hash_set.cpp`,
  `servers/rendering/renderer_scene_cull.h`
* `modules/godot_physx/misc/build_physx.py` (Linux presets and patches)
