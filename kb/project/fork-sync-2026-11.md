---
type: Finding
title: Sync to Godot 4.8-beta4 and PhysX module 1302f909ca (2026-11)
description: The one sync of the core-performance milestone; upstream 4.8-beta4 merged with two combined conflicts, PhysX module updated by a 3-way patch with one master adaptation, tests 1497/1497 and smoke pass; NVIDIA left for its own inventory.
tags: [sync, upstream, physx, testing, phase-1]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-19T23:45:00Z }
---

# What was taken

| Source | Before | Taken | How |
| --- | --- | --- | --- |
| Godot upstream | `b47a795aa5` | `b070e63b16` (4.8-beta4, godot-builds tag `4.8-beta4`, "Built from commit" of that release; 247 commits) | `--no-ff` merge `d08c7580f4`, second parent `b070e63b16` |
| PhysX module (uno1982/godot `feature/physx5-module`) | `847b2ca459` | `1302f909ca` (15 module commits, 19 files) | 3-way patch, commit `2b7e93d07e` |
| NVIDIA-RTX/godot `nvidia-pt-dlss` | `135dff3887` | nothing yet | head `49a825dc52` (force-pushed series) is inventoried separately before any port |
| diginaat (former maintainer's fork) | | nothing | `nvidia-dlss-physx` head `13e860dfbe` is two README commits past this tree; `dev` is `143b8865d6`, already in this tree |

4.8-beta4 was the newest 4.8 snapshot on godot-builds when the sync ran
(2026-11-19); `b070e63b16` is an ancestor of upstream `master`
(`73ca6be2ce` at the time). Godot does not tag beta snapshots in the
main repository, so the commit comes from the godot-builds release notes.

The procedure is in [CUSTOM_BUILD.md](../../CUSTOM_BUILD.md) (update
procedure and conflict rules); the sync state log there has the row.

# Conflicts and how they were combined

`git merge-tree --write-tree --name-only HEAD^1 HEAD^2` lists exactly
these two files:

* `editor/themes/theme_modern.cpp`, tooltip panel style: upstream added
  `set_content_margin_all(0)` (its tooltip margins moved from the
  `TooltipLabel` stylebox to the panel, "Adjust tooltip margins to avoid
  extra borders in doc tooltips"); this fork has the Modern Compact
  corner radius and transparent popup gutter there. Both kept: zero
  content margin first, then the fork's radius and gutter.
* `thirdparty/README.md`, fonts: upstream renamed the `DroidSans*.woff2`
  entry to `DroidSansFallback.woff2`; this fork added the
  `AtkinsonHyperlegibleNext*.woff2` entry above it. Both kept.

# Fork-owned core files that merged without conflict

Reviewed with `git diff HEAD^1 HEAD` on the merge:

* `core/templates/rid_owner.h`: upstream reserves validator `0x7FFFFFFF`
  (`% 0x7FFFFFFE`). The lock-free `owns()` and the release store of
  `max_alloc` (D012 in
  [decisions](../../docs/core-modernization/decisions.md)) are untouched.
* `core/templates/hashfuncs.cpp`: upstream rewrote
  `hash_murmur3_buffer`'s block loop with `memcpy` (strict aliasing); same
  algorithm and output. The fork's String-key byte hash next to it is
  unchanged.
* `core/math/dynamic_bvh.h`: upstream renamed a parameter in
  `select_by_proximity()`, which is shared by the SSE and scalar paths
  (D014 still holds).
* `core/os/memory.h`: upstream made the placement `operator new`
  overloads inline again (and removed them from `memory.cpp`); the fork
  has no change there.
* `SConstruct`: upstream adds `-WX` to MSVC link flags under `werror`;
  the fork's additions are untouched.
* `hash_set.h`, `hash_map.h`, `a_hash_map.h`, `local_vector.h`,
  `hashfuncs.h`: not changed by upstream (D013's one-block `HashSet`
  needs no port).

Other fork rules: `servers/rendering/rendering_shader_container.h` is not
changed by the merge, so `CONTAINER_VERSION` stays 6;
`SCENE_DATA_FLAGS_USE_DEPTH_FOG` is still bit 9, after upstream's flags,
in both `scene_data_inc.glsl` and `render_scene_data_rd.h`; the Vulkan
driver links `ray_query_features` into each `pNext` chain once.

# PhysX module

Module changes (`847b2ca459..1302f909ca`): Optimize Shaders editor tool
(merges shaders that differ only in parameter defaults, optional delete
of the unused ones, progress and Cancel), PXTGI Measure Materials
measures albedo and metals and tags in one undo action, off-screen albedo
averaged by surface area and glow only where the glowing surface is,
`<cfloat>` for Linux GCC, README.

The patch applied with no conflict. The local adaptations
(`get_opts()` SCons options in `config.py`, the `generator="ninja"`
preset line, `PhysXCloth3D.use_global_wind`, master API adaptations) are
the same 42 files as before; one more master adaptation was needed:
`Color::components` no longer exists, so `PXTGIEmissionMeter` uses
`Color::operator[]`. `modules/godot_physx/misc` did not change, so the
PhysX, Blast and Flow SDKs were not rebuilt.

# Build and tests

* Editor build (`build.ps1 -KeepGoing`, MSVC 14.51, `production=yes`)
  builds with no errors after the merge and after the module update.
  New upstream deprecation warnings (C4996: `RayResult::collider` and
  `ShapeResult::collider`, use `collider_id`) in the PhysX module's
  direct space state and particle fluid; not errors, left for a later
  module change.
* The first tests build hit an MSVC internal compiler error (C1001,
  compiler access violation) on one object; the unchanged rerun built it.
  A one-off, not caused by the source.
* `devtools/test/smoke.ps1` passes after the merge and again after the
  module update: PhysX game and editor (`PhysX 5.10.0 initialized.`, CPU
  only on this machine), path tracer on Vulkan (noisy path-traced room),
  DDGI interior.
* Full `--test`: 1497 cases, 1497 passed, 0 failed, 3 skipped (the same
  three skipped on purpose); 594617 assertions. The 15 new cases are
  upstream's new tests (`tests/core/templates/test_sort_array.cpp` and
  additions to `test_scenes.cpp` and `test_button.cpp`). No new failure.
  `[RID]` (5), `[HashMap]` (11), `[HashSet]` (14) and `[Hashing]` (3, the
  hash-buffer file) pass. See the
  [unit-test baseline](/project/unit-test-baseline.md).

# Covers

* The merge `d08c7580f4` (whole upstream delta), in particular
  `editor/themes/theme_modern.cpp`, `thirdparty/README.md`,
  `core/templates/rid_owner.h`, `core/templates/hashfuncs.cpp`,
  `core/math/dynamic_bvh.h`, `core/os/memory.h`, `SConstruct`
* `modules/godot_physx/` (commit `2b7e93d07e`)
* `CUSTOM_BUILD.md` sync state log
