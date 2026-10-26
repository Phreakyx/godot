#!/usr/bin/env python3
"""Build the PhysX 5 SDK for linking into the godot_physx module.

The module links against a PhysX SDK built out of tree with the static CRT.
This script clones NVIDIA's PhysX repository at a pinned revision, drops in a
build preset tuned to match Godot, runs PhysX's own project generation and
CMake build, and prints the path to pass to scons as physx_sdk=.

    python modules/godot_physx/misc/build_physx.py [--gpu] [--blast] [--flow]
    scons platform=windows target=editor physx_sdk=<printed path> [physx_gpu=yes] [blast_sdk=<printed path>] [flow_sdk=<printed path>]

The GPU build also needs the CUDA Toolkit (CUDA_PATH set or nvcc on PATH).
SCsub copies the GPU library next to the Godot binary for you.

--blast additionally builds the Blast SDK (runtime mesh fracture/destruction,
NvBlast + extensions, backing PhysXDestructible3D and its in-editor fracture
dialog) from this same checkout's blast/ subdirectory -- it's version-locked
to the same NVIDIA-Omniverse/PhysX release train as PhysX itself, so no
separate clone or ref pin is needed. SCsub copies its libraries next to the
Godot binary too.

--flow additionally builds NVIDIA Flow (sparse-grid smoke / fire / dust,
backing PhysXFlow3D) from the same checkout's flow/ subdirectory, with its own
build.bat / build.sh. flow_sdk= is that flow/ directory itself. Nothing is
linked -- nvflow / nvflowext are loaded at run time, and SCsub copies both
next to the Godot binary.

--legacy-gpu additionally targets Maxwell through Volta: it widens the GPU
SASS list with sm_50/60/61/70 and lowers PhysX's compute-capability gate
from 7.0 to 5.0 together, through one opt-in patch
(misc/physx_patches/legacy/0012-legacy-gpu-archs.patch). Without it a GTX
10-series card is refused outright; with it, it runs. Needs CUDA 12.x --
CUDA 13's nvcc cannot compile any of those four architectures. It costs
build time and disk, which is why it is off by default: measured on 32
cores, the SDK build goes from 100 s to 173 s and PhysXGpu_64.dll from
331 MB to 478 MB. Any run without the flag reverse-applies the patch, so
a checkout that built with it once goes back to the default architecture
list on the next build -- including a CPU-only one.

The last thing printed is the scons command for everything built.

Run this script on the machine you're building the module for -- --platform
defaults to the host OS (windows / linuxbsd).
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys

# Pinned so a given godot_physx revision always builds against the same SDK.
PHYSX_REPO = "https://github.com/NVIDIA-Omniverse/PhysX.git"
PHYSX_REF = "ovphysx-0.5.11"  # PhysX SDK 5.10.0

HERE = os.path.dirname(os.path.abspath(__file__))
PRESET_DIR = os.path.join(HERE, "physx_presets")
PATCH_DIR = os.path.join(HERE, "physx_patches")
# Opt-in, so it lives in a subdirectory the PATCH_DIR loop's *.patch filter skips.
LEGACY_PATCH = os.path.join(PATCH_DIR, "legacy", "0012-legacy-gpu-archs.patch")


def run(cmd, cwd):
    print("+ " + " ".join(cmd) + "  (in %s)" % cwd)
    subprocess.check_call(cmd, cwd=cwd)


def _nvcc_major():
    nvcc = shutil.which("nvcc") or os.path.join(os.environ.get("CUDA_PATH", ""), "bin", "nvcc")
    out = ""
    try:
        out = subprocess.run([nvcc, "--version"], capture_output=True, text=True).stdout
    except OSError:
        pass
    # "Cuda compilation tools, release 12.8, V12.8.93"
    release = out.split("release ", 1)[1].split(",", 1)[0].strip() if "release " in out else ""
    return release, release.split(".")[0]


def apply_patch(patch, src):
    name = os.path.basename(patch)
    if subprocess.call(["git", "apply", "--reverse", "--check", patch], cwd=src, stderr=subprocess.DEVNULL) == 0:
        print("patch already applied: " + name)
        return
    if subprocess.call(["git", "apply", "--check", patch], cwd=src, stderr=subprocess.DEVNULL) != 0:
        sys.exit("bundled patch does not apply (wrong PhysX ref?): " + name)
    run(["git", "apply", patch], cwd=src)
    print("applied patch: " + name)


def reverse_patch(patch, src):
    name = os.path.basename(patch)
    if subprocess.call(["git", "apply", "--reverse", "--check", patch], cwd=src, stderr=subprocess.DEVNULL) != 0:
        return  # not applied, nothing to say
    run(["git", "apply", "--reverse", patch], cwd=src)
    print("reversed patch: " + name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpu", action="store_true", help="also build the CUDA GPU projects (needs the CUDA Toolkit)")
    ap.add_argument(
        "--legacy-gpu",
        action="store_true",
        help="also target Maxwell through Volta (sm_50/60/61/70) and let those cards past PhysX's compute 7.0 gate",
    )
    ap.add_argument(
        "--blast",
        action="store_true",
        help="also build the Blast SDK (NvBlast + extensions) from this same checkout's blast/ subdirectory",
    )
    ap.add_argument(
        "--flow",
        action="store_true",
        help="also build NVIDIA Flow (nvflow + nvflowext) from this same checkout's flow/ subdirectory",
    )
    ap.add_argument(
        "--platform",
        choices=["windows", "linuxbsd"],
        default="windows" if os.name == "nt" else "linuxbsd",
        help="target platform / scons platform= value (default: autodetected from the host)",
    )
    ap.add_argument(
        "--src",
        metavar="DIR",
        help="PhysX checkout to build in; cloned here if absent (default: a 'physx-sdk' directory beside the Godot repo)",
    )
    ap.add_argument(
        "--vc",
        choices=["15", "17", "18"],
        default="17",
        help="MSVC toolchain for the Windows presets: 15 = VS 2017 (ninja), 17 = VS 2022, 18 = VS 2026 (default: %(default)s)",
    )
    ap.add_argument("--ref", default=PHYSX_REF, help="git ref to check out when cloning (default: %(default)s)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel build jobs")
    ap.add_argument("--config", default="release", choices=["release", "checked", "profile", "debug"])
    args = ap.parse_args()

    preset_base = ("vc%swin64-godot" % args.vc) if args.platform == "windows" else "linux64-godot"
    preset = preset_base + "-gpu" if args.gpu else preset_base
    preset_file = os.path.join(PRESET_DIR, preset + ".xml")
    if not os.path.isfile(preset_file):
        sys.exit("missing preset: " + preset_file)
    if args.gpu and not (os.environ.get("CUDA_PATH") or shutil.which("nvcc")):
        sys.exit("--gpu needs the CUDA Toolkit (set CUDA_PATH or put nvcc on PATH)")
    # --legacy-gpu is permanently CUDA 12.x-only, and this refusal is the mirror
    # image of the vc18 one just below: that guard refuses CUDA *below* 13 for
    # --vc 18 --gpu, this one refuses CUDA *13 or newer* for --legacy-gpu. They
    # are not two rules in tension -- --vc 18 --gpu already needs CUDA >= 13,
    # and no CUDA >= 13 can compile sm_50/60/61/70, so --vc 18 --gpu
    # --legacy-gpu is impossible by composition. That is one conflict, which is
    # why the preset case is a message branch here rather than a second
    # version-reading guard, and why both comparisons go through the one
    # _nvcc_major(). nvcc --list-gpu-arch is the more robust probe than the
    # release string if these two are ever revisited together.
    if args.legacy_gpu:
        if not args.gpu:
            sys.exit(
                "--legacy-gpu only widens the GPU architecture list, so it does nothing without "
                "--gpu: a CPU build has no architecture list to widen and gains no legacy GPU "
                "support. Add --gpu, or drop --legacy-gpu."
            )
        if args.vc == "18" and args.platform == "windows":
            sys.exit(
                "--legacy-gpu cannot be combined with --vc 18: the vc18 presets need CUDA 13 or "
                "newer (MSVC 14.5x reports _MSC_VER 1951, outside CUDA 12.x's accepted range), and "
                "CUDA 13's nvcc cannot compile sm_50, sm_60, sm_61 or sm_70 at all. Build the "
                "legacy GPU SDK with --vc 15 and CUDA 12.x."
            )
        release, major = _nvcc_major()
        if major.isdigit() and int(major) >= 13:
            sys.exit(
                "--legacy-gpu needs CUDA 12.x and this is CUDA %s: its nvcc fatals with "
                "\"Unsupported gpu architecture\" on each of sm_50, sm_60, sm_61 and sm_70, the "
                "lowest architecture it can compile offline being sm_75. Build with --vc 15 and a "
                "CUDA 12.x toolkit (CUDA_PATH), or drop --legacy-gpu." % release
            )
        if not major.isdigit():
            print(
                "warning: could not read a CUDA version from nvcc. --legacy-gpu needs CUDA 12.x; "
                "CUDA 13 and newer cannot compile sm_50/60/61/70 at all."
            )
    # vc18 + GPU is accepted by the preset check because vc18win64-godot-gpu.xml
    # exists, and the CUDA guard above only tests that a toolkit is present, not
    # which one. CUDA 12.x's crt/host_config.h accepts _MSC_VER in [1910, 1950)
    # and MSVC 14.5x reports 1951, so nvcc refuses it -- and not cleanly:
    # cudafe++ dies with 0xC0000005 tens of minutes into the build, and
    # -allow-unsupported-compiler does not help. Only block on a version we
    # actually read, so a working CUDA 13+ host is not locked out.
    if args.gpu and args.vc == "18" and args.platform == "windows":
        release, major = _nvcc_major()
        if major.isdigit() and int(major) < 13:
            sys.exit(
                "--vc 18 --gpu needs CUDA 13 or newer: CUDA %s rejects MSVC 14.5x (_MSC_VER 1951 is "
                "outside its accepted range) and cudafe++ crashes part way through the build. Build "
                "the GPU SDK with --vc 15, or see BUILD-NOTES-WINDOWS-VS2026.md." % release
            )
        if not major.isdigit():
            print(
                "warning: could not read a CUDA version from nvcc. --vc 18 --gpu needs CUDA 13 or "
                "newer; under CUDA 12.x cudafe++ crashes part way through the build. See "
                "BUILD-NOTES-WINDOWS-VS2026.md."
            )
    # --vc 15 selects a preset with generator="ninja", and PhysX only passes
    # -DCMAKE_MAKE_PROGRAM on its Linux ninja branch, so ninja has to be on PATH
    # here. Without this the failure is a CMake error buried in
    # generate_projects.bat output.
    if args.platform == "windows" and args.vc == "15" and not shutil.which("ninja"):
        sys.exit(
            "--vc 15 builds through the Ninja generator (a packman VS 2017 toolchain is not a "
            "registered VS install), so ninja must be on PATH. Visual Studio ships one under "
            "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\Ninja -- see BUILD-NOTES-WINDOWS-VS2026.md."
        )

    # Locate / create the PhysX source tree.
    src = args.src
    if not src:
        # Beside the Godot repo, not inside it -- SDK source is not part of the tree.
        repo_root = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
        src = os.path.join(os.path.dirname(repo_root), "physx-sdk")
    src = os.path.abspath(src)

    if not os.path.isdir(os.path.join(src, ".git")):
        os.makedirs(os.path.dirname(src), exist_ok=True)
        run(["git", "clone", "--depth", "1", "--branch", args.ref, PHYSX_REPO, src], cwd=os.path.dirname(src) or ".")
    else:
        print("using existing PhysX checkout: " + src)

    physx = os.path.join(src, "physx")
    if not os.path.isdir(physx):
        sys.exit("no physx/ directory in " + src)

    # Apply bundled patches (fixes not yet in the pinned PhysX ref). Each is a
    # -p1 diff rooted at the checkout; skipped cleanly if already applied.
    #
    # The opt-in legacy patch comes off FIRST. It rewrites the same two lines
    # 0002 owns, so while it is applied 0002 neither reverse-checks nor
    # forward-checks and the loop below exits with "bundled patch does not
    # apply (wrong PhysX ref?)" -- measured, not theorised. Its reversal is
    # conditioned on nothing but the absence of the flag, and deliberately not
    # on whether this is a GPU build: the checkout is reused across runs, so an
    # earlier --gpu --legacy-gpu run leaves it patched and the next run may
    # well be a CPU-only one. Skipping the reversal there would leave the patch
    # applied, and the GPU build after that would ship sm_50/60/61/70 to
    # someone who never asked for them.
    if not args.legacy_gpu:
        reverse_patch(LEGACY_PATCH, src)

    if os.path.isdir(PATCH_DIR):
        for name in sorted(f for f in os.listdir(PATCH_DIR) if f.endswith(".patch")):
            apply_patch(os.path.join(PATCH_DIR, name), src)

    # Applied after the loop, because its context lines are the text 0002
    # leaves behind rather than pristine upstream.
    if args.legacy_gpu:
        apply_patch(LEGACY_PATCH, src)

    # Install the Godot-tuned preset.
    shutil.copy2(preset_file, os.path.join(physx, "buildtools", "presets", "public", preset + ".xml"))

    is_windows = os.name == "nt"
    gen = "generate_projects.bat" if is_windows else "generate_projects.sh"
    gen_path = os.path.join(physx, gen)
    run([gen_path, preset] if is_windows else ["bash", gen_path, preset], cwd=physx)

    # Visual Studio is a multi-config generator (one build dir, config picked at
    # build time); on Linux PhysX generates one Makefile build dir per config.
    build_dir = os.path.join(physx, "compiler", preset if is_windows else preset + "-" + args.config)
    if not os.path.isdir(build_dir):
        sys.exit("project generation did not produce " + build_dir)
    run(
        ["cmake", "--build", build_dir, "--config", args.config, "--target", "install", "--parallel", str(args.jobs)],
        cwd=physx,
    )

    sdk = os.path.join(physx, "install", preset, "PhysX")
    if not os.path.isdir(os.path.join(sdk, "include")):
        sys.exit("build finished but no SDK at " + sdk)

    # Extra scons options from each optional SDK, printed as one command at the end.
    scons_args = ["physx_sdk=" + sdk.replace("\\", "/")]
    if args.gpu:
        scons_args.append("physx_gpu=yes")

    print()
    print("PhysX SDK ready:")
    print("    " + sdk)
    if args.gpu:
        print()
        print(
            "SCsub copies %s next to the Godot binary, nothing else to do."
            % ("PhysXGpu_64.dll" if args.platform == "windows" else "libPhysXGpu_64.so")
        )

    if args.blast:
        # Blast lives in the SAME checkout as PhysX (NVIDIA-Omniverse/PhysX's
        # own blast/ subdirectory), version-locked to the same release train --
        # no separate clone or ref to manage. It has its own build system
        # (premake5 via a bundled `repo` tool), independent of PhysX's CMake
        # build above, and always builds its "release" config by default (no
        # attempt is made to map PhysX's release/checked/profile/debug config
        # enum onto Blast's own, simpler config flag).
        blast_dir = os.path.join(src, "blast")
        if not os.path.isdir(blast_dir):
            sys.exit("--blast needs a blast/ directory in the checkout at " + src)
        blast_script = os.path.join(blast_dir, "build.bat" if is_windows else "build.sh")
        if not os.path.isfile(blast_script):
            sys.exit("no Blast build script at " + blast_script)
        if is_windows:
            run([blast_script], cwd=blast_dir)
        else:
            # Use our own compiler instead of NVIDIA's build container (patch 0005).
            blast_cmd = ["bash", blast_script, "--no-docker"]
            # Parallel codegen sometimes leaves a half written header, a rerun fixes it.
            if subprocess.call(blast_cmd, cwd=blast_dir) != 0:
                print("Blast build failed, retrying once")
                run(blast_cmd, cwd=blast_dir)

        blast_platform = "windows-x86_64" if is_windows else "linux-x86_64"
        blast_sdk = os.path.join(blast_dir, "_build", blast_platform, "release", "blast-sdk")
        if not os.path.isdir(os.path.join(blast_sdk, "include")):
            sys.exit("Blast build finished but no SDK at " + blast_sdk)

        scons_args.append("blast_sdk=" + blast_sdk.replace("\\", "/"))

        print()
        print("Blast SDK ready:")
        print("    " + blast_sdk)
        print("SCsub copies Blast's libraries next to the Godot binary.")

    if args.flow:
        # Flow lives in the same checkout too (its flow/ subdirectory), with its
        # own premake5-based build. Its scripts must run from the flow/
        # directory. MSVC 14.44 turns warning C4756 (overflow in constant
        # arithmetic, in Flow's float-limit constants) into an error under
        # Flow's warnings-as-errors; CL=/wd4756 silences just that one.
        flow_dir = os.path.join(src, "flow")
        if not os.path.isdir(flow_dir):
            sys.exit("--flow needs a flow/ directory in the checkout at " + src)
        if is_windows:
            flow_script = os.path.join(flow_dir, "build.bat")
        else:
            # ARM Linux has its own script.
            flow_script = os.path.join(flow_dir, "build_aarch64.sh" if platform.machine() == "aarch64" else "build.sh")
        if not os.path.isfile(flow_script):
            sys.exit("no Flow build script at " + flow_script)
        flow_env = os.environ.copy()
        if is_windows:
            flow_env["CL"] = (flow_env.get("CL", "") + " /wd4756").strip()
        print("+ %s  (in %s)" % (flow_script, flow_dir))
        subprocess.check_call([flow_script] if is_windows else ["bash", flow_script], cwd=flow_dir, env=flow_env)

        flow_platform = "windows-x86_64" if is_windows else "linux-" + platform.machine()
        flow_bin = os.path.join(flow_dir, "_build", flow_platform, "release")
        flow_libs = ("nvflow.dll", "nvflowext.dll") if is_windows else ("libnvflow.so", "libnvflowext.so")
        missing = [name for name in flow_libs if not os.path.isfile(os.path.join(flow_bin, name))]
        if missing:
            sys.exit("Flow build finished but %s missing from %s" % (", ".join(missing), flow_bin))
        scons_args.append("flow_sdk=" + flow_dir.replace("\\", "/"))

        print()
        print("Flow ready:")
        print("    " + flow_dir)
        print("SCsub copies %s and %s next to the Godot binary automatically." % flow_libs)

    print()
    print("Build the module with:")
    print("    scons platform=%s target=editor %s" % (args.platform, " ".join(scons_args)))


if __name__ == "__main__":
    main()
