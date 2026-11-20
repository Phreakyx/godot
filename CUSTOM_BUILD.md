# Custom Godot 4 build: Godot + NVIDIA path tracer/DLSS + PhysX 5

This repository is a Godot 4 engine build, "Godot Community Edition", that
combines official Godot, NVIDIA's DLSS/path tracer fork and a PhysX 5 module,
and must be kept up to date with all of them. It is maintained at
**https://github.com/Phreakyx/godot**. The former maintainer's fork,
https://github.com/Diginaat/godot, ended development; it is watched for useful
changes. This file is the maintenance runbook for humans and coding agents.

## Sources

| Remote | URL | Branch | What it provides | How it is taken |
| --- | --- | --- | --- | --- |
| `origin` | https://github.com/Phreakyx/godot | `dev` | This fork; the only push target | Push `dev` after the push gate |
| `upstream` | https://github.com/godotengine/godot | release tags, `master` | Official Godot 4 engine (the base everything tracks) | Merge of a release tag or beta/rc snapshot commit |
| `nvidia` | https://github.com/NVIDIA-RTX/godot | `nvidia-pt-dlss` | DLSS (Streamline), Ray Reconstruction, path tracer, Aftermath | Port of the new work as own commits (tree diff, see "Sync policy") |
| `physx` | https://github.com/uno1982/godot | `feature/physx5-module` | `modules/godot_physx/` (PhysX 5, GPU dynamics, Blast, Flow, water, vehicles) | Vendored directory, 3-way patch |
| `diginaat` | https://github.com/Diginaat/godot | `nvidia-dlss-physx`, `dev` | The former maintainer's fork | Watch only; cherry-pick what is useful |

The four source remotes are fetch only: their push URL is `DISABLED`, so a
push to them fails.

### Branches

`dev` is the only maintained line until the first release of this
milestone; which branch becomes the release line is decided then. All work
lands on `dev` and is published with `git push origin dev` after the push
gate (see "Sync policy").

| Branch | Purpose |
| --- | --- |
| `dev` (origin) | The maintained line; every change goes here |
| `port/godot-4.8` (origin) | Side branch: the PhysX port to the 4.8 namespaces; not merged this milestone |
| `radiance-cascade-gi` (origin) | Side branch: radiance cascade GI work; not merged this milestone |
| `master` (origin and local) | Stale (2026-10-26); not the release line |
| `nvidia-dlss-physx` (local) | The former maintainer's published branch, same as `diginaat/nvidia-dlss-physx` |
| `backup/pre-sync-<date>` (local) | The `dev` head before a sync; never pushed |

The old three-branch flow (`dev-pt` to `dev` to `nvidia-pt-dlss`, published
as `nvidia-dlss-physx` through a `fork` remote) is retired.

### If a remote is missing

Every line is guarded, so the block can be run again on any clone:

```bash
git remote get-url origin >/dev/null 2>&1 || git remote add origin https://github.com/Phreakyx/godot.git
git remote get-url upstream >/dev/null 2>&1 || git remote add upstream https://github.com/godotengine/godot.git
git remote get-url nvidia >/dev/null 2>&1 || git remote add nvidia https://github.com/NVIDIA-RTX/godot.git
git remote get-url physx >/dev/null 2>&1 || git remote add physx https://github.com/uno1982/godot.git
git remote get-url diginaat >/dev/null 2>&1 || git remote add diginaat https://github.com/Diginaat/godot.git
git remote set-url --push upstream DISABLED
git remote set-url --push nvidia DISABLED
git remote set-url --push physx DISABLED
git remote set-url --push diginaat DISABLED
```

`devtools/setup/setup_dev_windows.ps1` and `devtools/setup/setup_dev_cachyos.sh`
add the same four source remotes with the same `DISABLED` push URL (`origin`
stays as cloned).

## How the sources are combined

- **Upstream is merged as git history**, one release tag or snapshot commit
  per sync (see "Sync policy"), with `--no-ff`.
- **NVIDIA is ported, not merged.** NVIDIA rewrites its branch (force-push,
  2026-11: `135dff3887` replaced by `49a825dc52`). Merging a rewritten head
  re-applies rewritten copies of commits this tree already has, with mass
  conflicts and silent duplicates. Its new work is taken as a tree diff and
  committed as `NVIDIA: <feature> (nvidia-pt-dlss <sha>)` commits.
- **PhysX is vendored as a directory, NOT merged.** The `physx` branch is based on
  `4.7-stable`, not master. Merging it would pull hundreds of 4.7 release backports into
  a master-based tree. Only `modules/godot_physx/` is taken from it, as a 3-way patch.
  Its commits touch no other engine file that matters for PhysX.
- **diginaat is watched.** It is fetched at each sync; anything useful is
  cherry-picked like NVIDIA work. Never pushed.
- `modules/godot_physx/` carries local changes on top of the vendored copy:
  adaptations to upstream master API changes, the `generator="ninja"` line in
  `misc/physx_presets/vc17win64-godot-gpu.xml`, SCons options for
  `physx_sdk`, `physx_gpu`, `blast_sdk` and `flow_sdk` in `config.py`
  (`get_opts()`, so `custom.py` can set them), and
  `PhysXCloth3D.use_global_wind` (the weather module's world wind). Never
  overwrite the directory wholesale; merge 3-way (see below).

## Sync policy

- **What is merged from upstream:** Godot release tags. While the current
  minor has no tag, the newest beta or rc snapshot commit published in
  [godotengine/godot-builds](https://github.com/godotengine/godot-builds/releases)
  ("Built from commit" in the release notes; Godot does not tag snapshots in
  the main repository), recorded in the sync log with its godot-builds tag
  name (for example `4.8-beta4`).
- **When:** one sync per milestone, at a milestone boundary. After the
  Phase 2 performance baselines there is no upstream merge until
  4.8-stable is tagged, taken at a milestone boundary: a merge shifts code
  layout by about 1.5% and invalidates every baseline.
- **Order, fixed:** upstream merge, PhysX module, NVIDIA port, diginaat check.
  Each is its own commit (or merge), so each can be reverted alone.
- **NVIDIA rule.** Take the last NVIDIA head taken from the sync log (`OLD`)
  and the new head (`NEW`) and pick the case by merge base:

  ```bash
  git fetch nvidia
  OLD=<last taken NVIDIA head from the sync log>; NEW=$(git rev-parse nvidia/nvidia-pt-dlss)
  if git merge-base --is-ancestor $OLD $NEW; then
    git diff $OLD $NEW > nvidia.patch          # fast-forward: the plain delta
  elif [ "$(git merge-base $OLD upstream/master)" = "$(git merge-base $NEW upstream/master)" ]; then
    git diff $OLD $NEW > nvidia.patch          # force-push on the same base: tree diff of old and new head
  else
    git range-diff $(git merge-base $OLD upstream/master)..$OLD $(git merge-base $NEW upstream/master)..$NEW
    # base moved: port commit by commit; record it in the sync log
  fi
  git apply --3way nvidia.patch                # then resolve with zdiff3, never one side wholesale
  ```

  A large delta is first grouped by feature (new files, changed files,
  conflicts on `dev`), then ported group by group, each group its own
  commit (`git apply --3way --include=<paths>`). A group that duplicates
  fork work (for example NVIDIA's lock-free `RID_Alloc::owns()`, which this
  fork already has as D012) is skipped and listed in the sync log, as is a
  group deferred to a later sync.
- **diginaat:** fetch at each sync, `git log --oneline HEAD..diginaat/nvidia-dlss-physx`
  and `HEAD..diginaat/dev`, cherry-pick (`git cherry-pick -x`) what is
  useful, note the checked head under the sync log. Never push.
- **Push gate:** before every `git push origin dev`, an MSVC editor build and
  `devtools/test/smoke.ps1` pass. At each sync and at each phase end, the
  Linux build, the unit tests and `devtools/test/smoke_linux.sh` pass too
  (see "Build (Linux)").

The 2026-11 sync and how it went:
[kb/project/fork-sync-2026-11.md](kb/project/fork-sync-2026-11.md).

### Sync state log

Update this table after every sync. A source without new commits is written
"(no change)" with its unchanged SHA, never left empty.

| Date | upstream | NVIDIA (`nvidia`) | physx module commit | Merge commit |
| --- | --- | --- | --- | --- |
| 2026-10-07 | `e7b12e7492` | `135dff3887` | `2de7ea521f` (Flow builds pipelines on first use) | `4624c40df5` |
| 2026-10-08 | `e7b12e7492` (no change) | `135dff3887` (no change) | `5681d7519a` (Flow on Linux, MIT license) | `be69ff39db` (physx update commit, no merge; on `dev`) |
| 2026-10-08 | `65e8d16951` | `135dff3887` (no change) | `5681d7519a` (no change) | `4495b586dd` |
| 2026-10-31 | `95c0fa5fe8` (Godot 4.8-beta) | `135dff3887` (no change) | `97bd583c28` (separate render thread, PXTGI reflections, VS 2026 SDK build) | `7c12c34bed` (on `dev`) |
| 2026-11-07 | `b47a795aa5` | `135dff3887` (kept; NVIDIA force-pushed `49a825dc52`, not taken) | `92da57bda8` (cloth wind, ragdoll drives, vehicle engine drives, water and PXTGI fixes) | `09fbb8dbfa` (on `dev`) |
| 2026-11-16 | `b47a795aa5` (no change) | `135dff3887` (kept; NVIDIA still `49a825dc52`, not taken) | `847b2ca459` (groom hair simulation, optional Alembic import with `alembic_sdk`, Blast fracture in the background, XR lens effects, PXTGI fixes, editor icons) | `ed6cd4a3e2` (on `dev`) |
| 2026-11-19 | `b070e63b16` (Godot 4.8-beta4, godot-builds tag `4.8-beta4`) | `135dff3887` (kept; head `49a825dc52` inventoried, port pending) | `1302f909ca` (Optimize Shaders editor tool, PXTGI Measure Materials measures albedo and metals, area-weighted off-screen albedo and glow) | `d08c7580f4` (on `dev`) |

diginaat `13e860dfbe` checked 2026-11-19: nothing taken (README commits only).

### History layout

The history was rebuilt on 2026-10-09 so that each source stays visible
(`git log --first-parent` reads top to bottom as):

| Commits | Source |
| --- | --- |
| Upstream Godot `master` | Official history, never rewritten; joined by merges `Merge upstream Godot master (<sha>)` |
| `NVIDIA: Miscellaneous`, `NVIDIA: Dependencies`, `NVIDIA: Pathtracer + DLSS` | NVIDIA-RTX/godot, original commits and hashes |
| `Merge upstream Godot master (b6a3291ea8) into NVIDIA RTX nvidia-pt-dlss` | Only the conflict resolutions between NVIDIA and upstream |
| `godot_physx: import the PhysX 5 module by Wild Ox Studios` | uno1982/godot `feature/physx5-module` at `2de7ea521f`, verbatim, authored by uno1982 |
| `godot_physx: adapt the module to Godot 4.8 master` | The local module changes |
| Everything after | This fork's own commits, in one line, with upstream merges where they were synced |

Before the rebuild, the first merge (`4f6baae206`) also carried the whole
PhysX module and its adaptations, and four internal merges between `dev`
and the published branch added nothing. Every rebuilt commit has the same
files as the commit it replaces; the old history is in the release tags
(v0.1.0 to v0.5.1 point to it) and in the `backup/history-clean-2026-10-09/*`
branches of the former maintainer's machine.

Keep it this way: merge upstream with `--no-ff` (one merge per sync), take
PhysX module updates as their own commit
(`godot_physx: update to physx module <sha>`), port NVIDIA work as
`NVIDIA: <feature> (nvidia-pt-dlss <sha>)` commits, and keep `dev` a
straight line otherwise.

## Update procedure

1. **Start clean.** `git status` must be empty. Commit first, and save a patch
   outside the repository: `git diff --binary HEAD > <scratch>/wip.patch`.
2. **Fetch all five remotes:**
   `git fetch origin; git fetch upstream --tags; git fetch nvidia; git fetch physx; git fetch diginaat`.
3. **See what's new:**
   - Upstream: the newest release tag of the current minor
     (`git tag -l '4.8*-stable' --sort=-v:refname | head -1`) or, while the
     minor has none, the newest snapshot on godot-builds and its
     "Built from commit". Check it: `git merge-base --is-ancestor <sha> upstream/master`
     and `git rev-list --count HEAD..<sha>`.
   - NVIDIA: `git rev-parse nvidia/nvidia-pt-dlss` against the last taken
     head in the sync log; pick the case with the NVIDIA rule above.
   - PhysX: `git log --oneline <last physx commit>..physx/feature/physx5-module -- modules/godot_physx`
   - diginaat: `git log --oneline HEAD..diginaat/nvidia-dlss-physx` and
     `git log --oneline HEAD..diginaat/dev`
4. **Back up:** `git branch backup/pre-sync-<date> HEAD`.
5. **Merge upstream:** `git merge --no-ff -m "Merge Godot <name> (<sha>) into dev" <tag or sha>`.
   Resolve by the conflict rules below, then review the fork-owned files in
   "Diverged core files" that merged without a conflict
   (`git diff HEAD^1 HEAD -- <file>`).
6. **Update PhysX** if it has new module commits, 3-way, keeping local adaptations,
   as its own commit `godot_physx: update to physx module <sha>`:
   ```
   git diff <last physx commit> physx/feature/physx5-module -- modules/godot_physx > physx.patch
   git apply --3way physx.patch
   ```
7. **Port NVIDIA** if its head moved, by the NVIDIA rule in "Sync policy".
8. **Check diginaat** and cherry-pick what is useful.
9. **Build with `-k`** and fix all errors in one pass (see Build). Build the
   tests too (`devtools\build\build.ps1 -Tests`) and compare `--test` with the
   last recorded run.
10. **Smoke test** (see Test). Build and smoke must pass before committing.
11. **Commit,** then update the sync state log above and the `kb/` sync note.
12. **Publish:** check that no private paths or project names are in tracked
    files (`git grep`; the repository is public), then `git push origin dev`
    after the push gate.

### If a sync is interrupted

- A merge or cherry-pick that stops halfway: `git merge --abort` (or
  `git cherry-pick --abort`). A `git apply --3way` that left conflicts:
  `git checkout -- <the patched paths>` (only those paths).
- Then reset `dev` to the backup and start again:
  `git reset --hard backup/pre-sync-<date>`.
- Only one sync runs at a time on `dev`.
- Never interrupt a variant build (editor, tests, mono): the variants share
  generated headers (`*.gen.h`), and an interrupted build leaves them out of
  step with the objects. See
  [kb/mistakes/interrupted-mono-build-shared-objects.md](kb/mistakes/interrupted-mono-build-shared-objects.md).

### Conflict rules (learned the hard way)

- **Never resolve a conflicted file by taking one side wholesale.** The first merge on
  2026-10-07 did that and silently dropped upstream changes in 13 files and fork changes
  in 5. Use `git checkout --conflict=zdiff3 -- <files>` to see the base, and combine
  hunks.
- After any merge, verify the resolutions with
  `git merge-tree --write-tree --name-only HEAD^1 HEAD^2`, which lists the files that
  conflicted.
- **Look for silent duplicates:** when both sides added the same code in different
  places, git auto-merges both copies. This happened with Vulkan `ray_query_features`;
  linking it twice into the `pNext` chain creates a loop.
- **Shader container format:** if `ReflectionData` / `ReflectionBindingData` in
  `servers/rendering/rendering_shader_container.h` changes on either side, bump
  `CONTAINER_VERSION` above both sides' values (currently 6, since the
  upstream merge `7c12c34bed` of 2026-10-31). A stale shader cache with
  the same version but a different layout crashes D3D12 startup (0xC0000005, garbage
  in `ReflectionBindingData` allocation).
- **Fork-specific code to preserve:**
  - `RenderForwardClusteredPT` is the Forward+ renderer, including the fallback in
    `renderer_compositor_rd.cpp`.
  - `SCENE_DATA_FLAGS_USE_DEPTH_FOG` goes after upstream's flags (currently bit 9) in
    both `scene_data_inc.glsl` and `render_scene_data_rd.h`.
  - `ShaderPreprocessor::add_define()` custom defines are applied in `_prepare_state()`.
  - Streamline/Aftermath setup, markers and cleanup in `main/main.cpp`, plus the
    `--gpu-markers`, `--debug-shaders` and `--raytracing-validation` flags.
  - The engine name "Godot Community Edition" (`GODOT_VERSION_NAME`), set in
    `methods.get_version_info()` so `version.py` stays as upstream has it.
  - The shader container version, `CONTAINER_VERSION` (currently 6), above
    both sides' values (see the rule above).
  - New projects default to the Vulkan driver (`EditorNode::get_initial_settings()`),
    because path tracing needs Vulkan.
  - The project manager warns that a project can break the first time this
    build opens it (`_fork_warning_*` in
    `editor/project_manager/project_manager.cpp`, marker
    `.godot/nvidia_dlss_physx_backup.cfg`). No backup is required.
  - CI `static_checks.yml` runs `prek` via pip instead of the third-party action.
  - The split workspace (`DOCK_SLOT_MAIN_SCREEN_SPLIT`, `EditorMainScreenSplit`,
    `DockSplitWorkspace` in `EditorNode`) and the Live Runtime view
    (`scene/debugger/runtime_state_observer.cpp`,
    `editor/run/live_runtime_plugin.cpp`). See
    [SPLIT_WORKSPACE.md](SPLIT_WORKSPACE.md).
  - Forward+ shader variant feature groups and per-shader skipped variants
    (`SceneShaderForwardClustered::ShaderFeature`, `enable_shader_features()`,
    `ShaderRD::version_set_skipped_variants()`, the mask in
    `ShaderData::set_code()`), and full-key `PipelineHashMapRD`. When
    upstream adds a scene shader variant or changes which passes draw which
    surfaces, give the variant its feature group and keep the skip rules in
    step with the pass routing. See
    [docs/shader-permutations/memory.md](docs/shader-permutations/memory.md).
  - The GPU geometry paths are on by default (`rendering/geometry/gpu_instances/*`,
    `gpu_instance_table`, `multimesh_gpu_culling/*`, `cluster_lod/*` and its
    streaming; `GLOBAL_DEF`s in `render_forward_clustered.cpp` and
    `mesh_storage.cpp`, defaults in `ProjectSettings.xml`). See D035 in
    [knowledge/TECHNICAL_DECISIONS.md](knowledge/TECHNICAL_DECISIONS.md).
  - Far directional cascades kept between frames
    (`directional_shadow/far_cascade_update_interval`): the cache in
    `RendererSceneCull::_light_instance_setup_directional_shadow()`,
    `RendererSceneRender::directional_shadow_cache_generation()`, and
    directional shadow passes that clear only their own atlas region in
    Forward+ and Mobile (no whole-atlas clear). See
    [kb/project/directional-cascade-cache.md](kb/project/directional-cascade-cache.md).
  - Forward+ material parameter tables (D036 in
    [knowledge/TECHNICAL_DECISIONS.md](knowledge/TECHNICAL_DECISIONS.md)):
    `MaterialUniforms` as a storage-buffer struct array in
    `scene_forward_clustered.glsl`, `SceneShaderForwardClustered::MaterialTable`
    and its `MaterialData::update_parameters()`, the group id in the sort
    key, the slot in `compressed_aabb_position_pad.w`. Upstream changes to
    the material UBO path must be ported there.
  - Shadow passes multiply the mesh LOD threshold
    (`mesh_lod/lod_change/shadow_threshold_scale`, in `_render_shadow_append()`
    of Forward+ and Mobile). See
    [kb/project/shadow-lod-scale.md](kb/project/shadow-lod-scale.md).
  - Thread-safe `RID_Alloc::owns()` in `core/templates/rid_owner.h` is
    lock-free on x86-64 (D012 in
    [docs/core-modernization/decisions.md](docs/core-modernization/decisions.md)),
    with the release store of `max_alloc` in `_allocate_rid()`. Upstream
    reverted its own version (godotengine/godot#114900); if upstream lands
    a fixed lock-free `owns()`, take theirs and drop this.
  - `HashSet` (`core/templates/hash_set.h`) allocates one block per table
    (`_allocate_block()`, `_free_block()`; D013). Upstream changes to its
    allocation, resize or copy code must be ported to the block layout.
  - `DynamicBVH::Volume` (`core/math/dynamic_bvh.h`) has an SSE layout and
    kernels on x86-64 single precision (`DYNAMIC_BVH_SSE`; D014). Upstream
    changes to `Volume` must be made in both the SSE and the scalar code.
  - `RenderingServerDefault::finish()` makes the main RenderingDevice
    current on the main thread again after the separate render thread
    ends, so its `finalize()` runs at exit (without it the guard skipped
    the shutdown and objects leaked). Drop it if upstream fixes the same.
  - The weather module (`modules/weather/`: wind, `Weather`, profiles, effects),
    the wind in `GrassField3D`'s default material and
    `PhysXCloth3D.use_global_wind`. See [WEATHER.md](WEATHER.md).

## Diverged core files

Every file under `core/` and `main/` that differs from the upstream base this
tree was last merged with. An upstream merge that touches one of them needs
a review even when git merges it cleanly. Regenerate the list with:

```bash
git diff --name-only $(git merge-base HEAD upstream/master) HEAD -- core main
```

Every plan or sync that adds a fork hunk to `core/` or `main/` adds its row
here in the same commit. Rules: **keep** (upstream has no equivalent), **port**
(port upstream changes into the fork's version), **take upstream's** (drop the
fork's change when upstream lands the same).

| File | Owner | Change | Rule |
| --- | --- | --- | --- |
| `core/config/engine.cpp` | NVIDIA | `is_raytracing_validation_enabled()`, `is_gpu_markers_enabled()` | Keep |
| `core/config/engine.h` | NVIDIA | `use_raytracing_validation`, `use_gpu_markers` and their getters | Keep |
| `core/error/error_backtrace.cpp` | NVIDIA | New file: native backtrace dump on errors (`ERROR_BACKTRACE_ENABLED`); `backtrace_capture()`/`backtrace_format()` and the DbgHelp lock from `49a825dc52` | Keep; take NVIDIA's updates by the NVIDIA rule |
| `core/error/error_backtrace.h` | NVIDIA | New file: `backtrace_dump()`, `backtrace_capture()`, `backtrace_format()` | Keep; take NVIDIA's updates by the NVIDIA rule |
| `core/error/error_macros.cpp` | NVIDIA | Prints a backtrace to stderr for errors and shader errors | Keep; take upstream's if it lands error backtraces |
| `core/input/input.cpp` | NVIDIA | F13 emits the Streamline PC-latency ping marker | Keep |
| `core/io/dir_access.cpp` | NVIDIA | Non-virtual `get_next()` records the entry; `current_modified_time()` (faster editor filesystem scan, `49a825dc52`) | Keep; port upstream `DirAccess` changes, every subclass implements `_get_next_entry()` |
| `core/io/dir_access.h` | NVIDIA | `get_next()` non-virtual, `_get_next_entry()` pure virtual, virtual `current_modified_time()` | Keep; a new upstream `DirAccess` subclass must implement `_get_next_entry()` instead of `get_next()` |
| `core/io/file_access_pack.cpp` | NVIDIA | `DirAccessPack::get_next()` renamed to `_get_next_entry()` | Keep |
| `core/io/file_access_pack.h` | NVIDIA | Same rename in `DirAccessPack` | Keep |
| `core/math/dynamic_bvh.h` | Fork | SSE `Volume` layout and kernels on x86-64 single precision (`DYNAMIC_BVH_SSE`, D014) | Port upstream `Volume` changes into both the SSE and the scalar code |
| `core/templates/a_hash_map.h` | Fork | String keys hashed with `hash_table_key()` (byte hash) | Port |
| `core/templates/hash_map.h` | Fork | String keys hashed with `hash_table_key()` (byte hash) | Port |
| `core/templates/hash_set.h` | Fork | One allocation per table (`_allocate_block()`, `_free_block()`, D013); byte hash for String keys | Port upstream allocation, resize and copy changes to the block layout |
| `core/templates/hashfuncs.cpp` | Fork | `hash_buffer()` byte hash (wyhash/XXH3) for String and CharString keys | Port; `HashMapHasherDefault` and `String::hash()` outputs never change |
| `core/templates/hashfuncs.h` | Fork | `hash_buffer()`, `hash_table_key()` | Port |
| `core/templates/local_vector.h` | Fork | Bulk copy of trivially copyable spans from 256 elements | Keep; take upstream's if it lands a bulk copy |
| `core/templates/rid_owner.h` | Fork | Lock-free `RID_Alloc::owns()` on x86-64 with the release store of `max_alloc` (D012) | Take upstream's when a fixed lock-free `owns()` lands (godotengine/godot#114900 was reverted) |
| `main/main.cpp` | NVIDIA and fork | Streamline/Aftermath setup, markers and cleanup; `--gpu-markers`, `--debug-shaders`, `--raytracing-validation` (NVIDIA); layered windows for the editor's rounded popups (fork) | Keep |
| `main/performance.cpp` | Fork | GPU geometry monitors (GPU-culled instances, cluster LOD triangles, HZB-occluded triangles, cluster LOD streaming memory) | Keep |
| `main/performance.h` | Fork | The monitor enum entries and `_gpu_geometry_statistic()` | Keep |

## Build (Windows)

A new PC is set up with `devtools\setup\setup_dev_windows.ps1` (see
[devtools/README.md](devtools/README.md)): it checks the tools, installs SCons
and the D3D12 dependencies, sets the remotes, builds the PhysX, Blast and
Flow SDKs once (`modules/godot_physx/misc/build_physx.py --gpu --blast --flow`,
pinned to `ovphysx-0.5.11`, in a `physx-sdk` folder next to the repository)
and writes their paths into `custom.py` (not tracked). SCons reads
`custom.py` on every run, so a build needs no SDK arguments:

```
devtools\build\build.ps1 -KeepGoing
devtools\build\build.ps1 -Tests            # unit-test build
:: or directly, from PowerShell or cmd, with a Python that has SCons
python -m SCons -k platform=windows target=editor production=yes
```

`scons` is not on PATH. `devtools\build\build.ps1` finds the Python that has
SCons and clears `MSYSTEM`, so it also works when started from Git Bash. A raw
`python -m SCons` needs a Python that has SCons, and PowerShell or cmd: from
Git Bash it can't find the D3D12 and AccessKit dependencies and stops at
configure. Rebuild the SDKs (delete `physx-sdk` and run the setup again) only
when the module's `build_physx.py` pin, patches or presets change.

- Output: `bin\godot.windows.editor.x86_64.exe` and `.console.exe`. `PhysXGpu_64.dll`,
  the four `NvBlast*.dll` and `nvflow.dll`/`nvflowext.dll` are copied next to it
  automatically.
- `-Tests` builds the unit-test editor with its own objects
  (`tests=yes object_prefix=t_ extra_suffix=tests`):
  `bin\godot.windows.editor.x86_64.tests.console.exe`.
- Close any running editor from `bin\` first, or the link fails with `Access is denied`.
- After a change to the version info in `methods.py` (for example
  `GODOT_VERSION_NAME`), SCons regenerates `core/version_generated.gen.h` but
  does not recompile the files that reach it through `core/version.h`
  (`main.cpp`, `editor_node.cpp` and others). Delete their objects (or build
  clean), or the exe keeps the old name.
- .NET/C# (`module_mono_enabled=yes` + `modules/mono/build_scripts/build_assemblies.py`,
  `build.ps1 -Mono`): built and smoke-tested (`smoke.ps1 -Mono`) for every
  release since 0.5.1; not part of the push gate.

## Build (Linux)

Linux builds run on CachyOS, dual-booted on the same PC as the Windows
builds, so Windows and Linux numbers come from the same hardware. Clone the
repository onto the Linux file system, not an NTFS mount (slow, unreliable
builds; the setup script warns). Then:

```bash
devtools/setup/setup_dev_cachyos.sh --install-deps --build
```

It checks or installs the tools (Godot's Arch packages plus `clang lld llvm
cmake`), sets the four source remotes, builds CPU PhysX, Blast and Flow (no CUDA),
writes `custom.py`, and with `--build` runs these builds:

```bash
scons platform=linuxbsd target=editor production=yes                                   # GCC
scons platform=linuxbsd target=editor production=yes use_llvm=yes linker=lld           # Clang
scons platform=linuxbsd target=editor production=yes tests=yes object_prefix=t_ extra_suffix=tests
scons platform=linuxbsd target=editor production=yes use_llvm=yes linker=lld tests=yes object_prefix=t_ extra_suffix=tests
scons platform=linuxbsd target=editor dev_build=yes tests=yes                          # GCC, DEV_ASSERT compiled in
```

Run the tests with `bin/godot.linuxbsd.editor.x86_64.tests --test` (and the
`.llvm.tests` and `.dev` binaries), and the smoke test with
`devtools/test/smoke_linux.sh`.

CachyOS is rolling. While a measurement series runs, hold the compilers in
`/etc/pacman.conf` (`IgnorePkg = gcc gcc-libs clang llvm llvm-libs lld`), so
control and candidate builds use the same compiler, and record the compiler
versions (`gcc --version`, `clang --version`) with every run.

## Versioning

This build has its own version, separate from Godot's. It lives in
`CUSTOM_VERSION` at the repository root (`MAJOR.MINOR.PATCH`). Godot's own
`version.py` is never edited, so upstream merges stay clean.

- **MAJOR:** a change that breaks existing projects or removes a feature of
  this build.
- **MINOR:** a new feature of this build (for example, the in-editor DLSS
  installer), or moving to a new Godot minor version (4.8 to 4.9).
- **PATCH:** fixes, and syncs with upstream, NVIDIA or PhysX that add no
  feature of this build.

Bump `CUSTOM_VERSION` in the commit that prepares a release, not before. A
version number is never reused.

Releases on GitHub name the Godot base and the own version. The engine calls
itself "Godot Community Edition". Releases from 0.11.0 on are published on
https://github.com/Phreakyx/godot; older tags keep their names (0.9.0 to
0.10.3: `diginaat-physx`, title "Godot Diginaat + PhysX"; before 0.9.0:
`nvidia-rt-dlss-physx`, title "Godot NVIDIA + PhysX"; all on Diginaat/godot).
DLSS, DDGI and weather are named in the release notes, not in the slug.

| What | Format | Example |
| --- | --- | --- |
| Git tag | `godot{major}.{minor}-{status}-physx-pt-v{own}` | `godot4.8-beta-physx-pt-v0.11.0` |
| Release title | `Godot Community Edition + PhysX + PT (Godot {major}.{minor}-{status}) v{own}` | `Godot Community Edition + PhysX + PT (Godot 4.8-beta) v0.11.0` |
| Zip | `godot_{major}.{minor}-physx-pt_v{own}-editor_windows_amd64[_mono].zip` | `godot_4.8-physx-pt_v0.11.0-editor_windows_amd64.zip` (.NET: `..._amd64_mono.zip`) |
| Export templates | `godot_{major}.{minor}-physx-pt_v{own}-export_templates_windows_amd64[_mono].tpz` | `godot_4.8-physx-pt_v0.11.0-export_templates_windows_amd64.tpz` |

`package_editor_win64.ps1` builds the zip name from `CUSTOM_VERSION` and
`version.py`, and prints the tag and title to use.

| Own version | Godot base | Tag | Notes |
| --- | --- | --- | --- |
| 0.1.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.1.0` | First release (originally tagged `v4.8-dev-2026.10.08`) |
| 0.2.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.2.0` | In-editor DLSS installer |
| 0.3.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.3.0` | Blast and Flow in releases; path tracer: volumetric fog, glass |
| 0.4.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.4.0` | DDGI Forward+ global illumination and `DDGIVolume` node |
| 0.5.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.5.0` | DDGI baking, realtime updates, smooth light changes, bounce energy, probe count |
| 0.5.1 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.5.1` | Windows export templates (standard and .NET); exported games get the PhysX, Blast, Flow and DLSS DLLs |
| 0.5.2 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.5.2` | Fixes: exported games with an embedded PCK crashed at startup; viewport jitter when DLSS is selected but not installed |
| 0.5.3 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.5.3` | Same build as 0.5.2; the editor zips no longer put the files in a `.` folder |
| 0.6.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.6.0` | DDGI: dark caves and tunnels (no sky light or sky reflections), DDGI-lit volumetric fog, guided probe rays, faster response to moving objects, up to 30% faster; screen reader support (AccessKit) |
| 0.7.0 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.7.0` | DDGI: near-field rays (bounce light from nearby objects in place, following them), moving objects lit without smears or noise, emissive surfaces as direct lights, low resolution apply (faster), least-squares baking, visible probes updated first |
| 0.7.1 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.7.1` | DDGI: moving objects of any shape (boxes, poles, planks, capsules), not only spheres; fix: ray tracing output binding format in DDGI dispatches |
| 0.7.3 | 4.8-dev | `godot4.8-dev-nvidia-rt-dlss-physx-v0.7.3` | DDGI fixes: no flicker or blotches in dark places, no white dots near bright openings, near-field rays shorter (0.5 spacings); tunnel test view |
| 0.8.0 | 4.8-beta | `godot4.8-beta-nvidia-rt-dlss-physx-v0.8.0` | Godot 4.8-beta base, PhysX module `97bd583c28`; DDGI: near-field rays removed (dark spots that moved with the camera), DDGI rays through glass fixed |
| 0.8.1 | 4.8-beta | `godot4.8-beta-nvidia-rt-dlss-physx-v0.8.1` | Reflections: `PlanarReflector3D` with a mirror pass, automatic reflections from `DDGIVolume`, budgeted DDGI-normalized reflection probes; DDGI adaptive update budget; fix: sky flicker in mirrors with volumetric fog |
| 0.9.0 | 4.8-beta | `godot4.8-beta-diginaat-physx-v0.9.0` | New release names (`diginaat-physx`); Modern Compact editor theme with Atkinson Hyperlegible Next; split workspace and Live Runtime view; large-world geometry (GPU culling, cluster LOD, far-field voxels, HZB occlusion); project backup before first open; Godot master `b47a795aa5`, PhysX module `92da57bda8` |
| 0.9.1 | 4.8-beta | `godot4.8-beta-diginaat-physx-v0.9.1` | The engine calls itself "Godot Diginaat"; native weather and wind (`modules/weather`: `WindServer`, `WindZone3D`, `Weather` with profiles, sky, rain and wet surfaces, haze, lightning, sound); shadow speed-ups (far directional cascades kept between frames, coarser shadow LODs); no per-field GPU timestamps in `VoxelField3D` and `GrassField3D`; fixes: RenderingDevice shutdown with the render thread, transparent popup corners |
| 0.10.1 | 4.8-beta | `godot4.8-beta-diginaat-physx-v0.10.1` | GPU-driven instances on by default (persistent instance table, GPU culling into indirect draws, shadow casters, HZB occlusion, material parameter tables); scene runtime (groups, children cache, transform propagation, process loop: scene instancing 10.7x and node churn 11.4x at 100k nodes); core containers (lock-free RID ownership checks, one-allocation HashSet, String-key byte hash, SSE DynamicBVH); PhysX module `847b2ca459` (groom hair, Blast background fracture, XR lens effects); new export templates required |
| 0.10.2 | 4.8-beta | `godot4.8-beta-diginaat-physx-v0.10.2` | Fewer shader and pipeline permutations (Forward+): scene shader variants compiled per feature group when the feature is first used, each material skips the variants it can't draw with, no precompiled transparent pipelines that no pass draws, pipeline maps keyed by the whole key; compiled shader bytes and the loaded Vulkan pipeline cache freed after use (120 materials: variants 3,556 to 472, cold stall 146 to 95 s on Vulkan and 239 to 96 s on D3D12, warm 2.4 to 0.65 s, memory 353 to 84 MB); fix: SDFGI pass bindings for GPU instances |
| 0.10.3 | 4.8-beta | `godot4.8-beta-diginaat-physx-v0.10.3` | The project manager no longer forces a backup before this build first opens a project; it warns that the project can break, with "Open Anyway" and "Cancel" |

## Release packages (Windows)

All builds use `production=yes`. With MSVC that means the static CRT and no
debug symbols; LTO stays off on purpose (the platform script says it doesn't
help with MSVC), so an incremental build is quick. The release steps are in
[devtools/README.md](devtools/README.md#release):

```
devtools\build\build.ps1 -Target release      # both editors, all four templates, C# assemblies
devtools\test\smoke.ps1
devtools\test\smoke.ps1 -Mono
powershell -File devtools/package/package_editor_win64.ps1
powershell -File devtools/package/package_editor_win64.ps1 -Mono
```

The zips land in `dist/` (gitignored). The script bundles the editor exes,
`PhysXGpu_64.dll`, the Blast DLLs (`NvBlast.dll`, `NvBlastGlobals.dll`,
`NvBlastExtAuthoring.dll`, `NvBlastExtShaders.dll`), the Flow DLLs
(`nvflow.dll`, `nvflowext.dll`), the D3D12 Agility SDK DLLs, the license
files and, for mono, `bin/GodotSharp/`. Blast and Flow are always included:
the script fails if any of their DLLs is missing from `bin/`. NVIDIA
Streamline DLLs are bundled only with `-WithNvidiaRuntime` (private use; never
`bin/development/`, which holds the debug variants).

### Export templates

Four builds (`devtools\build\build.ps1 -Target templates`, and again with
`-Mono`), packaged as `.tpz` in `dist/` and installed with Editor > Manage
Export Templates > Install from File:

```
powershell -File devtools/package/package_templates_win64.ps1
powershell -File devtools/package/package_templates_win64.ps1 -Mono
```

The `.tpz` holds the templates (release, debug and their console wrappers),
the D3D12 Agility SDK DLLs (`D3D12Core.x86_64.dll`, as the Windows export
expects) and the licenses; `version.txt` comes from the template binary
(`4.8.dev`, `4.8.dev.mono`). PhysX GPU, Blast and Flow DLLs are not in the
templates: `NativeRuntimeExportPlugin`
(`editor/export/native_runtime_export_plugin.cpp`) copies them from the
editor's folder next to every exported Windows game, and the NVIDIA
Streamline / DLSS files too when they are installed there (the editor's
DLSS installer). Export options: `native_runtime/include_physx_libraries`,
`native_runtime/include_nvidia_dlss`. So games made with this editor ship
DLSS, but the public editor and template downloads never contain it.

## Test

`devtools\test\smoke.ps1` (and `-Mono` for the .NET editor) runs all of them
with the console exe. A pass means exit code 0, no `ERROR:` lines, and the
expected log line.

| Check | Command | Expect |
| --- | --- | --- |
| PhysX GPU, D3D12 | `--verbose --path <godot-physx-example> --quit-after 600` (and again with `--editor`) | `PhysX 5.10.0 initialized [GPU]` |
| Path tracer, Vulkan | The DDGI test project's room with `--pt=1`, `--rendering-driver vulkan` | Noisy path-traced image; no "Raytracing not supported" warning |
| DDGI | The DDGI test project's interior with `--measure` | Renders and measures without errors |

Unit tests: `devtools\build\build.ps1 -Tests`, then
`bin\godot.windows.editor.x86_64.tests.console.exe --test`; compare the summary
(passed, failed, skipped) with the last recorded run in
[kb/project/unit-test-baseline.md](kb/project/unit-test-baseline.md).

Linux: `devtools/test/smoke_linux.sh` runs the same pass rule against every
`bin/godot.linuxbsd.editor.x86_64*` editor (GCC and Clang): the editor and a
game start on Vulkan and PhysX (CPU) initializes. See "Build (Linux)".

## Known limitations

- **Path tracing is Vulkan only** (on Windows and Linux). The D3D12 driver has no ray tracing implementation
  (all stubs, in upstream too). Projects set to `driver.windows="d3d12"` show a warning
  and render with rasterization.
- **PhysX GPU dynamics are Windows-only.** Linux builds CPU PhysX, Blast and
  Flow (`build_physx.py` with the `linux64-godot.xml` preset, no CUDA).
