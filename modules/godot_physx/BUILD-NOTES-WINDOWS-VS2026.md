# Building on Windows with Visual Studio 2026 (and no VS 2022)

Supplement to [README.md](README.md)'s *Building* section, for the case where the only
Visual Studio on the machine is **2026** (VS 18, MSVC 14.51). The README's documented path
assumes VS 2022 (`vc17`); this file covers what changes when you don't have it, and what was
added to the module to make that work.

Written 2026-10-25 against `feature/physx5-module` @ `53c1939ec4`, PhysX ref
`ovphysx-0.5.11`. A full CPU **and** GPU build was completed and verified this way.

## The one constraint that drives everything

CUDA's host-compiler check, not PhysX, is what dictates the toolchain. From each toolkit's
`include/crt/host_config.h`:

| CUDA | accepted `_MSC_VER` | verdict against MSVC 14.51 (`_MSC_VER 1951`) |
| --- | --- | --- |
| 12.8 (README's pinned choice) | `[1910, 1950)` | **rejected.** `-allow-unsupported-compiler` does not help — `cudafe++` dies with `0xC0000005 ACCESS_VIOLATION` parsing the 14.51 headers |
| 13.4 | `[1920, 1960)` | accepted, no flag needed; also ships VS 2026 integration (`CudaProjectVsWizards/18`) |

So a GPU build on a VS-2026-only machine has exactly two options:

1. **CUDA 12.8 with an older MSVC.** Blast already packman-downloads **MSVC 14.16**
   (`_MSC_VER 1916`), which is inside 12.8's range — no extra toolchain to install. This is
   what was built and is the recommended route, because 12.8 is what the module is tested
   against.
2. **CUDA 13.4 with MSVC 14.51.** Fewer moving parts, but deviates from the tested CUDA and
   needs `-diag-suppress 20011` (MSVC 14.51's `<cmath>` trips PhysX's `-Werror=all-warnings`
   via diagnostic #20011 — it is not a PhysX device-code problem).

Note that **option 1 costs no GPU architectures.** The GPU preset sets
`PX_GENERATE_GPU_REDUCED_ARCHITECTURES=True`, whose SASS list (after patch `0002`) is
`75,80,86,89,90,100,120`. `sm_70` appears only in the non-reduced `ELSE` branch, which this
preset never takes — so in a **default** build the common "CUDA 13 drops sm_70" concern is moot
in both directions.

That holds for a default build only. `build_physx.py --legacy-gpu` adds `50,60,61,70` to that
same reduced list (patch `legacy/0012`), and CUDA 13's nvcc fatals on all four with
`Unsupported gpu architecture` — so option 2 and `--legacy-gpu` are mutually exclusive, and the
script refuses the combination by name. A legacy GPU SDK is an option-1 build.

MinGW is not an option for a GPU or Blast build: nvcc on Windows supports only MSVC as host
compiler, and Blast exposes a C++ ABI built with MSVC. Flow alone would be fine, as it is a
runtime-loaded C API.

## What this adds to the module

Presets (`misc/physx_presets/`):

- `vc18win64-godot{,-gpu}.xml` — VS 2026 generator, MSBuild. Good for a **CPU** SDK.
- `vc15win64-godot{,-gpu}.xml` — MSVC 14.16 via **Ninja**. CMake's "Visual Studio 15 2017"
  generator requires a *registered* VS 2017 installation, which a packman toolchain is not,
  so these set `generator="ninja"`. PhysX reads that from the preset's `generator` attribute
  and then passes `-DCMAKE_CUDA_COMPILER` instead of MSBuild's `-T cuda=`.

`misc/build_physx.py` gains `--vc 15|17|18` (default `17`, so existing usage is unchanged).

`SCsub` now globs `bin/win.x86_64.vc*.mt` instead of hardcoding `vc143`. This is required,
not cosmetic: PhysX derives that directory from `MSVC_VERSION` in
`GetCompilerAndPlatform.cmake` — `>=1930 → vc143`, `>=1920 → vc142`, `>=1910 → vc141` — so a
14.16 build emits `vc141` and the hardcoded path never resolves. (14.51 happens to also map
to `vc143`, which is why the VS 2026 CPU build worked before this change.)

Patches `0008`–`0011` in `misc/physx_patches/`, described in that directory's
[README](misc/physx_patches/README.md).

## Building the SDK

Everything below must run **inside Blast's packman MSVC 14.16 environment**. Three
non-obvious lines are required; see *Environment traps* for why. Adjust the five
machine-specific paths in the block: the packman root (twice), `CUDA_PATH`,
`PM_CUDA_PATH` — which uses forward slashes, so searching for `E:\` misses it —
and the Visual Studio Ninja directory, which depends on your VS edition
(`Professional`, `Enterprise`, `BuildTools`) and install root.

```bat
@echo off
call "E:\packman-repo\chk\msvc\2017-15.9.17-1\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
:: vcvars leaves a real env var ERRORLEVEL=1 which permanently shadows cmd's %errorlevel%.
set "ERRORLEVEL="
:: vcvars also resets the environment, hiding packman's own PM_PACKAGES_ROOT.
set "PM_PACKAGES_ROOT=E:\packman-repo"
:: ninja for Ninja Multi-Config (this VS dir holds only ninja.exe, so prepending is safe);
:: CC/CXX so CMake picks cl rather than a MinGW gcc that may also be on PATH.
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CC=cl"
set "CXX=cl"
set "CUDA_PATH=E:\cuda-12.8"
set "PM_CUDA_PATH=E:/cuda-12.8"
cd /d <godot repo>
python modules\godot_physx\misc\build_physx.py --vc 15 --gpu --src <physx checkout>
```

Blast must be built at least once first (`blast\build.bat`) so its packman MSVC 14.16 and
Windows SDK are on disk — that is where this toolchain comes from. Blast needs no
VS-version patching at all; it builds with its own bundled toolchain.

Flow builds with the system VS 2026 and needs patch `0009`.

### Getting CUDA 12.8 without installing it

The CUDA installer bundles a display driver and its silent install **fails (exit 8)** when a
newer driver is already present, which is the normal case. A portable toolkit root avoids
both the failure and the need for elevation: extract only the toolkit components from the
installer with 7-zip and merge each component's single inner directory into one root.

For 12.8 the components worth taking are `cuda_nvcc`, `cuda_cudart`, `cuda_cccl`,
`cuda_nvrtc`, `cuda_profiler_api`, `cuda_nvtx`, `cuda_cuobjdump`, `cuda_nvdisasm`,
`cuda_nvprune`, `cuda_cuxxfilt`, `cuda_nvml_dev`, `visual_studio_integration` — about 1.5 GB,
versus pulling in Nsight and the math libraries PhysX does not use. Point `CUDA_PATH` and
`PM_CUDA_PATH` at the merged root. (CUDA 13 splits things differently: `cuda_crt`, `libnvvm`,
`cccl`, `libnvjitlink`, `libnvfatbin` are separate, and CCCL headers land under
`include/cccl/`.)

## Building the editor

```bat
scons platform=windows target=editor ^
  physx_sdk=<physx checkout>/physx/install/vc15win64-godot-gpu/PhysX ^
  physx_gpu=yes ^
  blast_sdk=<physx checkout>/blast/_build/windows-x86_64/release/blast-sdk ^
  flow_sdk=<physx checkout>/flow
```

Run this from `cmd`. `^` is cmd's line continuation; PowerShell's is a backtick and
treats `^` as a literal argument, so pasting the block into PowerShell silently runs a
truncated `scons platform=windows target=editor` with none of the SDK paths.

If the AccessKit SDK is absent, scons prints a red `ERROR:` about the screen reader
driver, then disables that driver itself and carries on — the build still succeeds, it
just does not have screen reader support. Install the SDK with `python
misc\scripts\install_accesskit.py`, or pass `accesskit=no` explicitly to silence the
message.

PhysX static libraries built with MSVC 14.16 `/MT` **link cleanly into a Godot built with
14.51** — verified, and consistent with Microsoft's binary-compatibility guarantee. Godot
itself is built by the system VS 2026; SCons 4.11 detects it correctly. Don't try to build
Godot with 14.16.

Export templates need the same flags with `target=template_release` / `template_debug`; the
downloadable templates do not contain this module.

## Verifying a GPU build

```
bin\godot.windows.editor.x86_64.console.exe --headless --path <demo project> --quit-after 120
```

A working GPU build prints:

```
PhysX: CUDA context ready on device 'NVIDIA GeForce RTX 4080 SUPER' -> GPU dynamics available.
```

If CUDA is unavailable it warns and falls back to CPU simulation, so absence of that line is
the thing to look for, not a crash.

## Environment traps

These produce *silent* failures and are worth reading before debugging a build that dies with
no output.

- **`vcvars64` leaves a real environment variable named `ERRORLEVEL=1`.** It permanently
  shadows cmd's dynamic `%errorlevel%`, so afterwards `%errorlevel%` is frozen at 1 no matter
  what runs — `powershell -Command "exit 7"` still reports 1. packman's `packman.cmd` does
  `set PM_CONFIG_ERRORLEVEL=%errorlevel%`, concludes it failed, and kills
  `generate_projects.bat` with **no output whatsoever**. Fix: `set "ERRORLEVEL="` immediately
  after calling vcvars. (The underlying vcvars error is only `team_explorer.bat`, which is
  harmless in a relocated packman package.)
- **`vcvars64` resets the environment**, so `PM_PACKAGES_ROOT` becomes undefined and
  packman's `configure.ps1` takes a `setx` branch it should skip. Set it explicitly, and set
  `PATH`/`CC`/`CXX` *after* vcvars or they are wiped.
- **packman hides its own diagnostics.** `packman.cmd` redirects all of `configure.ps1`'s
  stdout to a temp file and deletes it, and that script writes its logs to stderr. Run
  `bootstrap\configure.ps1` directly to see what it is complaining about.
- **`cmd` may not resolve a bare batch-file name from the current directory** on a machine
  with `NoDefaultCurrentDirectoryInExePath` behaviour. Invoke batch files by full path.
- **PowerShell's `Set-Location` does not change the working directory child processes
  inherit.** Also set `[Environment]::CurrentDirectory`. This matters for
  `flow/generate_projects.bat`, which runs `premake5 --file=premake5.lua` relative to the cwd.
- **`%errorlevel%` inside a single chained `cmd /c "a & b & echo %errorlevel%"` expands at
  parse time** and reports a stale value. Use a batch file, where each line is parsed as it
  is reached, or test the artifact on disk instead of trusting the code.

## Status as of 2026-10-25

Built and verified on this machine: CPU and GPU PhysX SDKs (MSVC 14.16 + CUDA 12.8), Blast,
Flow, and a Godot 4.7.3 editor with all seven runtime DLLs beside it. The headless CUDA
context check above passes on an RTX 4080 SUPER.

Not done: export templates, and no on-screen check of PXTGI, Flow or Blast yet. The GI demos
live in the separate [godot-physx-example](https://github.com/uno1982/godot-physx-example)
project — `demo/editor/rendering/gi_courtyard.tscn`, `gi_courtyard_night.tscn`,
`gi_cornell_box.tscn`, and `demo/cpu/physx_playground_gi.tscn` (`G` toggles GI). PXTGI is
Forward+ only and needs an `Environment` in the scene.
