---
type: Finding
title: The engine switched to C++20 (port of godotengine/godot#100749)
description: The fork builds as C++20 on MSVC with upstream PR #100749's hunks taken as is and the floors raised to GCC 11, Clang 14, Visual Studio 2022; no extra breakage, no new warning, tests and smoke unchanged.
tags: [build, cpp20, toolchain, phase-1]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-20T12:00:00Z }
sources:
  - id: pr100749
    title: "Core: Bump C++ Standard to C++20"
    url: https://github.com/godotengine/godot/pull/100749
---

# What changed

The engine's C++ standard went from C++17 to C++20 (`/std:c++20` on
MSVC, `-std=gnu++20` on GCC and Clang; C stays C17). The switch is a
port of upstream's open PR godotengine/godot#100749 by Repiteo[^pr100749],
so upstream's eventual merge is near-identical. The rule in
`CUSTOM_BUILD.md`: take upstream's version when it lands and drop ours.
There is no option to build as C++17.

* PR head: `661972812490ee34ba8127c58210341ecef11386`, last updated
  2026-10-26T17:47:02Z, state open (conflicted against upstream master).
* Fork commit: on `dev`, after `6f3534edba`.

# Hunks taken as is

`git apply` of the PR's `.diff` applied to the post-sync tree without a
conflict, except one context line in `thirdparty/README.md` (upstream's
link for patch 0006 changed since the PR was written); the README line for
patch 0007 was added by hand. Taken unchanged:

| File | Change |
| --- | --- |
| `SConstruct` | `gnu++20` and `/std:c++20`; old GCC < 11 and Clang < 10 branches removed; `-Wenum-conversion`, `-Wattribute-alias=2`, `-Wlogical-op` moved into the lists |
| `core/typedefs.h` | `static_assert(__cplusplus >= 202002L, "Minimum of C++20 required.")` |
| `core/os/spin_lock.h` | pre-C++20 `ATOMIC_VAR_INIT` branch removed |
| `core/math/vector2/3/4.{h,cpp}`, `core/math/rect2.{h,cpp}` | `==` / `!=` against `Vector2i/3i/4i` and `Rect2i` (C++20 rewritten candidates were ambiguous) |
| `core/object/ref_counted.h` | cross-type `Ref<T>` `==` / `!=` |
| `core/variant/variant.h` | templated `Variant` comparison operators |
| `drivers/metal/SCsub` | its local C++20 override removed |
| `modules/csg/SCsub` | `_SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING` for MSVC (manifold) |
| `modules/gridmap/editor/grid_map_editor_plugin.cpp` | `u8` prefix dropped on two literals (`char8_t`) |
| `modules/mono/mono_gc_handle.h` | `operator==` / `!=` const |
| `thirdparty/basis_universal/encoder/basisu_math.h` | enum to `int32` constant, saved as `patches/0007-enum-to-int32.patch` |

# Hunks adapted: the floors

The PR keeps Visual Studio 2019 16.11 and raises Clang to 13. The fork
uses its own floors:

| Compiler | PR | Fork |
| --- | --- | --- |
| GCC | 11 | 11 |
| Clang | 13 | 14 |
| Apple Clang | 16 | 16 |
| MSVC | Visual Studio 16.11 | Visual Studio 2022 (`cc_version_major < 17`, the vswhere product major) |

Only the newest MSVC and the CachyOS GCC and Clang are tested. C++23/26
features are used only behind feature-test macros.

# Extra breakage

None. The `-k` build of the whole editor (fork renderer, NVIDIA path
tracer and Streamline code, PhysX module and SDK headers, weather module)
compiled and linked on the first pass with only the PR's hunks. No
third-party, PhysX SDK or Streamline patch was needed.

Outside the build, three fork benchmark scripts compiled engine headers
with `/std:c++17` and would now stop at the `typedefs.h` assert; they use
`/std:c++20`: `devtools/test/core_templates_bench.py`,
`devtools/test/math_simd_bench.py`, `devtools/test/unique_array_bench.py`.

# Warnings

Full `production=yes` editor builds, MSVC, counted per code in the build
logs (pre-sync `143b8865d6` and the C++20 tree; every C++ file recompiled
for the flag change: 2391 files):

| Code | Before | C++20 | Where |
| --- | --- | --- | --- |
| C4458 (declaration hides class member) | 6 | 6 | DDGI volume and its editor plugin, `render_forward_clustered.cpp` |
| C4996 (deprecated) | 4 | 4 | `modules/godot_physx` (`collider` instead of `collider_id`) |

No new warning code and no count change: MSVC at the default warning
level reports none of the C++20 deprecations (enum arithmetic between
different enums, implicit `this` capture with `[=]`, `volatile` compound
assignment). GCC and Clang may; they are checked in the Linux session.

# Tests and smoke

The tests build (`build.ps1 -Tests`, also a full C++20 rebuild, 5.2 min,
same 10 warnings) runs the whole suite as before; the result matches the
post-NVIDIA row of [the unit-test baseline](/project/unit-test-baseline.md)
exactly:

```
[doctest] test cases:   1497 |   1497 passed | 0 failed | 3 skipped
[doctest] assertions: 594617 | 594617 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Same three skipped cases and the same three `ERROR: Drawing is only
allowed ...` prints. `devtools/test/smoke.ps1` passes: PhysX game and
editor (`PhysX 5.10.0 initialized.`, CPU only), path tracer on Vulkan
(noisy path-traced room), DDGI interior.

# Conditions

* Compiler: Visual Studio 2026 (18.11.0), MSVC 14.51.36231, Windows SDK
  10.0.26100.0; `devtools/build/build.ps1` (`production=yes`), PhysX CPU
  only (no CUDA Toolkit), Blast and Flow on.
* Machine: Intel Core i9-14900K, 64 GB RAM, RTX 4080 SUPER, Windows 11.
* Editor build: 4.8 min (full C++ rebuild after the flag change).

# Covers

* `SConstruct`
* `core/typedefs.h`, `core/os/spin_lock.h`, `core/math/vector2.h`,
  `core/math/vector2.cpp`, `core/math/vector3.h`, `core/math/vector3.cpp`,
  `core/math/vector4.h`, `core/math/vector4.cpp`, `core/math/rect2.h`,
  `core/math/rect2.cpp`, `core/object/ref_counted.h`, `core/variant/variant.h`
* `drivers/metal/SCsub`, `modules/csg/SCsub`,
  `modules/gridmap/editor/grid_map_editor_plugin.cpp`,
  `modules/mono/mono_gc_handle.h`
* `thirdparty/basis_universal/encoder/basisu_math.h`,
  `thirdparty/basis_universal/patches/0007-enum-to-int32.patch`,
  `thirdparty/README.md`
* `devtools/test/core_templates_bench.py`, `devtools/test/math_simd_bench.py`,
  `devtools/test/unique_array_bench.py`

[^pr100749]: godotengine/godot#100749, head `6619728124`, 2026-10-26.
