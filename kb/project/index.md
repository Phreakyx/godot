# Project

* [Forward+ shader variants and pipelines compiled only where used](/project/shader-permutations.md) - feature groups, per-shader skipped variants, pruned transparent precompiles, full-key pipeline maps: variants per material 28 to ~4, cold stall 146 to 95 s, warm 2.4 to 0.65 s, memory 353 to 84 MB

* [Scene runtime optimization](/project/scene-runtime.md) - groups, children cache, transform propagation epoch, group storage, process prefetch: instancing and churn 10-11x faster at 100k nodes, API unchanged

* [Unique array baseline and bulk-copy candidate](/project/unique-array-evaluation.md) - Windows correctness passes; 12-size boundary sweep finds small-copy regressions and blocks performance acceptance

* [Core modernization phase-zero checkpoint](/project/core-modernization-audit.md) - baseline/branch, compatibility gates, benchmark tools, native-map direction and resumable phase plan

* [Packed core containers and compatible hashing](/project/core-template-storage.md) - dropped 2026-11-15: packed storage and robin-hood indices made numeric hash tables slower and bigger; only the String-key byte hash was kept
* [Core template migration benchmarks](/project/core-template-benchmarks.md) - 24-case controlled A/B: the dropped migration, and the kept String-key byte hash (integer tables unchanged, String hits 0.55-0.62x)
* [Old benchmark drivers against the runner](/project/benchmark-driver-migration.md) - on the same binaries `bench.py` reproduces the scene driver (within 3%, same node counts), the container driver's same-code cases (within 1% with the allocation counter accounted for) and the profile driver (shares within 0.7 points); the scene driver is deleted
* [A/A noise floors on the i9-14900K (MSVC, GCC, Clang)](/project/benchmark-noise-floors.md) - floors per workload, metric and build type and per micro case, with the conditions; frames 1-15% (cheap Linux frames up to 53% from one round), level 0.3-8%, gdbench cases median 2-4%, micro cases median 5-7%; perf counters on Linux; Linux peak memory of the small workloads still to measure
* [Container family shares and decisions (MSVC, GCC, Clang)](/project/family-shares.md) - final decisions from the largest share on any compiler: build allocation (34.0%, GCC), hash tables (15.7%, GCC), CowData (23.2%, Clang), Variant containers kept on the table against the rule's skip (two Linux workloads clear their floors); skip formatting, trees/lists
* [Where engine CPU time goes in core code](/project/core-cpu-profiles.md) - superseded (Ryzen 5 3600, hypotheses only): sampling profiles of five workloads: hash lookups 3-16%, scene instancing 21% in RID checks, locks and refcounts plus 14% heap; the callers' patterns cost more than container internals

* [Sync to Godot 4.8-beta4 and PhysX module 1302f909ca](/project/fork-sync-2026-11.md) - the milestone's one sync: two conflicts combined, fork-owned core files reviewed, one PhysX master adaptation, NVIDIA `49a825dc52` in part (N1, N2; the rest deferred); tests 1497/1497 and smoke pass
* [Project overview](/project/overview.md) - what this fork is, its sources, branches and release flow
* [Modern compact editor theme](/project/modern-compact-editor-theme.md) - neutral charcoal native theme with compact spacing and restrained interaction accents
* [Native indirect foundation](/project/native-indirect-foundation.md) - existing GPU submission seam, sentinel defect and per-view culling constraints
* [Forward+ material parameter tables](/project/material-parameter-tables.md) - one storage-buffer slot per material, shared uniform sets; unique_10k 8,776 draws to 11, render CPU 7.5-8.1 to 0.42-0.47 ms
* [Native GPU geometry statistics](/project/gpu-geometry-statistics.md) - async per-viewport culling/CLOD/HZB counters, only while polled
* [Native MultiMesh culling](/project/native-multimesh-culling.md) - opt-in GPU frustum tests, stable IDs, viewport-owned commands and fallback
* [Cluster hierarchy cooker](/project/cluster-hierarchy-cooker.md) - source-index LOD trees, boundary locks, checked resource and complete reference cuts
* [GPU cluster selection](/project/gpu-cluster-selection.md) - ancestor-veto cuts, projected-error/frustum selection and guarded native commands
* [Resident CLOD raster](/project/resident-clod-raster.md) - native storage, viewport commands, original vertex/material pipelines and conventional fallback
* [Cluster scene import](/project/cluster-scene-import.md) - final-source cook/cache, shared meshes, unsupported fallback and optimized shadows
* [Current-frame CLOD HZB](/project/current-frame-clod-hzb.md) - farthest-depth reduction, opaque-only rejection and conservative current-frame recovery
* [Residency cut protocol](/project/residency-cut-protocol.md) - complete coarse fallback, evicted ancestors and GPU request feedback
* [CLOD pages from disk](/project/clod-disk-paging.md) - checked page file, root-only memory, worker reads and dropped stale reads
* [GPU voxel levels for the far field](/project/far-field-gpu-voxel-levels.md) - per-brick level selection in compute, indirect MultiMesh discs, exact CPU mirror
* [Far directional cascades kept between frames](/project/directional-cascade-cache.md) - far cascades every 2nd/4th frame, staggered, reused while the slice fits the cached region; -1.1 to -1.5 ms in the valley
* [Coarser mesh LODs for shadow casters](/project/shadow-lod-scale.md) - shadow passes use a 4x LOD threshold; -0.4 ms in the valley; grass draw order and frame timestamps tried without effect
* [Unit-test baseline per stage](/project/unit-test-baseline.md) - full `--test` result of the tests build at each groundwork stage; pre-sync `143b8865d6`: 1482 cases, post-sync, post-NVIDIA and C++20 (MSVC): 1497, 0 failed, 3 skipped on purpose; GCC and Clang 1570, 0 failed
* [Linux builds, tests and smoke on CachyOS](/project/linux-build.md) - GCC 16.2.1 and Clang 23.1.5: five builds, `--test` 1570/1570, `[Fork]` 67/67 on GCC, Clang and dev, smoke 4/4; GCC test compile fix, Clang override warnings fixed, smoke runs the editor first
* [The engine switched to C++20](/project/cpp20-switch.md) - port of godotengine/godot#100749 (head `6619728124`), floors GCC 11, Clang 14, Visual Studio 2022; on MSVC no extra breakage, no new warning, tests 1497/1497 and smoke unchanged
* [Allocation counts in release builds](/project/allocation-counter.md) - opt-in counter in `core/os/memory.cpp` for `-Bench` builds only, read through the custom monitors `fork/alloc_count` and `fork/realloc_count`; A/A counts of scene instancing and churn identical on editor and template_release
* [Container contracts and the backend switch](/project/container-contracts.md) - `tests/fork/` pins HashMap, AHashMap, HashSet, RBMap, RBSet and List behavior (42 cases, production and dev builds); `core/templates/fork_backends.h` builds a same-source control and candidate side by side; no suspected bug found
* [World wind and weather core](/project/world-wind.md) - WindServer gusts shared by foliage, cloth and bodies; Weather node, profiles, effects, borrowed scene values; weather sky and drifting clouds; rain, occlusion map, wet surfaces; ground haze and mist motes; lightning; synthesized sound; local wind zones
