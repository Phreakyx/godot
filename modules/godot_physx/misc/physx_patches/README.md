# PhysX source patches

`build_physx.py` applies every `*.patch` directly in this directory (sorted by
name) to the PhysX checkout after clone/locate and before project generation.
The `legacy/` subdirectory is the exception: the patches there are opt-in and
are applied only when the matching `build_physx.py` flag is passed, and
reverse-applied on any build without it. Each patch is a `-p1` diff rooted at
the repo (`a/physx/...`), and is skipped cleanly if already applied.

These are fixes the pinned `PHYSX_REF` (`ovphysx-0.5.11`) doesn't have, either
upstream fixes that landed later or our own build fixes. Drop a patch once the
ref is bumped past it.

| Patch | Upstream | Fixes |
| --- | --- | --- |
| `0001-heightfield-gpu-boundary-crash.patch` | [PR #503](https://github.com/NVIDIA-Omniverse/PhysX/pull/503) / [issue #502](https://github.com/NVIDIA-Omniverse/PhysX/issues/502) | GPU sphere–height-field narrowphase reads ~8 GB out of bounds at a height-field rim triangle (the `BOUNDARY` adjacency sentinel is passed to `getTriangle()` unchecked) → `CUDA_ERROR_ILLEGAL_ADDRESS`, dead CUDA context. Latent until the bad address is unmapped, which a renderer sharing the GPU makes reliable. |
| `0002-gpu-turing-sm75-arch.patch` | none (build config) | The GPU arch list has no SASS for `sm_75` (Turing: RTX 20-series / GTX 16-series). Those cards JIT every PhysX CUDA kernel from PTX at load and run GPU dynamics ~8× slower. Adds `sm_75` to the SASS lists. |
| `0003-linux-clang-no-werror.patch` | none (build config) | The Linux clang build uses `-Werror -Weverything`, so every clang release that adds a warning breaks the SDK build (clang 22: `-Wmissing-include-dirs`, `-Wnrvo`, `-Wformat-signedness`). Drops `-Werror`; the warnings are still printed. |
| `0004-cuda13-toolkit.patch` | none (build config) | Builds and loads with the CUDA 13 toolkit (new `cuCtxCreate`, plus stubs for two new launch functions). Only kicks in on CUDA 13, 12.x builds don't change. |
| `0005-blast-linux-host-gcc.patch` | none (build config) | Builds Blast on Linux with a normal GCC instead of NVIDIA's GCC 9 container. No `-Werror`, three missing includes, `-fnew-inheriting-ctors` for GCC 13, `-no-pie` for the tests, and `-fno-strict-aliasing` since Blast casts between its vector types and optimized builds hang in fracture authoring without it. |
| `0006-blast-cutout-double-free.patch` | none (not reported yet) | Blast's cutout code runs a vector's destructor by hand and then deletes it, so it's freed twice. MSVC gets away with it, on Linux cutout fracturing crashes. |
| `0007-flow-linux-arch.patch` | none (build config) | Flow's `build.sh` uses the `arch` command, which some distros (Arch) don't have. `uname -m` gives the same thing everywhere. |
| `0008-vs2026-generator.patch` | none (build config) | The pinned ref maps only vc15/vc16/vc17 onto CMake's Visual Studio generators, so a `vc18win64` preset has no generator and project generation fails. Adds `vc18` -> `"Visual Studio 18 2026"`, includes `vc18` in the CUDA `-T cuda=` branch so the GPU projects generate, and lets a `vc18win64*` preset pull VsWhere. |
| `0009-flow-vs2026-toolset.patch` | none (build config) | Flow's `build.bat` maps any VS major >= 17 onto premake's `vs2022` action, and premake 5.0.0-beta2 has no `vs2026` action, so the solution asks for PlatformToolset `v143`. VS 2026 ships only `v145` -> `MSB8020`. Retargets the toolset on the MSBuild command line for VS 18+; VS 2022 builds unchanged. |
| `0010-ninja-skip-windows-resources.patch` | none (build config) | CMake's "Ninja Multi-Config" generator forwards `-DCMAKE_INTDIR=\"release\"` to `rc.exe`, which cannot parse the escaped quotes (`RC1107`). Marks the five Windows `.rc` files `HEADER_FILE_ONLY`; they are inert version resources in a static-library build. Only affects Ninja generators, which the `vc15win64-godot` presets use. |
| `0011-msvc141-nan-macro.patch` | none (not reported upstream) | MSVC 14.16 (VS 2017) cannot parse the CRT's `NAN` macro in `ExtDelaunayBoundaryInserter.cpp` (`C2143`), though it compiles on 14.3x+ and in isolation on 14.16. Uses `std::numeric_limits`, which needs no macro expansion. Newer toolsets unaffected. |
| `legacy/0012-legacy-gpu-archs.patch` | none (build config) | A Maxwell or Pascal card gets no GPU simulation at all, and a Volta one gets no native code: PhysX's `MIN_SM_MAJOR_VERSION` floor is compute 7.0, so anything below it is rejected outright, while compute 7.0 itself passes the floor and then finds no `sm_70` cubin in the reduced SASS list and JIT-compiles from the embedded `compute_52` PTX. Widens both reduced `GENERATE_ARCH_CODE_LIST` calls with `50,60,61,70` and lowers the floor to 5.0, in one coupled diff so the architecture list and the gate can never disagree. Only applied under `--legacy-gpu`, which needs CUDA 12.x (CUDA 13's nvcc fatals on all four); every build without the flag reverses it first, so the default architecture list is unchanged. |
