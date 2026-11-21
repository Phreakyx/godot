"""Benchmark runner: same-source builds, pinned alternating rounds, paired differences and verdicts.

  python devtools/bench/bench.py check
  python devtools/bench/bench.py fetch
  python devtools/bench/bench.py aa --workloads scene:instance:10000 --builds editor,template_release
  python devtools/bench/bench.py compare --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev
      --target scene:churn:10000 --workloads scene:churn:10000,scene:instance:10000
  python devtools/bench/bench.py profile --workload scene:instance:10000 --build template_release

`check` reads the machine state (power plan, core parking, idle compilers, bench-CPU load, Game Mode,
the runner lock) and prints every value. `aa` builds a control (extra_suffix bench) and an A/A twin
(bench.aa) and measures the noise floor. `compare` runs three binaries: the previous commit (built in a
git worktree under devtools/out/bench/wt/), the candidate (same source plus --defines) and the
same-source control; the verdict compares the candidate with the control only. Every series runs one
discarded cold round, plans its rounds within --budget, then alternates the order every other round.
`profile` samples one workload on the existing bench control exe with devtools/profile/sampler.exe and
writes its TSV, analyze.py summary and family shares under devtools/out/bench/profile/.
On Linux (bench_linux.py) the same commands build with scons (--compiler gcc|clang), `--counters` adds one
perf stat pass per binary and workload after the timed rounds, and `profile` records with perf.
The protocol is kb/tooling/benchmark-protocol.md. Outputs go to devtools/out/bench/ (gitignored).
"""

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path

if sys.version_info < (3, 12):  # hashlib.file_digest, tarfile filter="data"; fail before any build or export
    raise SystemExit(f"bench.py needs Python 3.12 or newer (this is {sys.version.split()[0]})")

if os.name == "nt":
    import bench_windows as osb
else:
    import bench_linux as osb

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "devtools/out/bench"
DEFAULT_FLOORS = ROOT / "devtools/bench/floors.json"
PLATFORM = "windows" if os.name == "nt" else "linuxbsd"
COMPILERS = ("msvc",) if os.name == "nt" else ("gcc", "clang")
COMPILER = COMPILERS[0]  # --compiler; part of every floor key and of meta.json
COUNTER_COLUMNS = ("instructions", "cycles", "cache_misses", "cache_references")  # --counters (Linux), never judged
MIN_ROUNDS = 5
TIMEOUT_S = 600
CHEAP_S = 3.0  # processes under this get n = 11 rounds, the others n = 5 (D-03)
MAX_LOAD_PCT = 10.0  # mean % Processor Utility of the bench CPUs before a series (starting value, --max-load)
CLOCK_TOLERANCE = 0.05  # rounds this far off the series median clock are flagged (D-14)
GAME_WORKERS = 14  # the worker pool cap of the bench project (D-12)
UNAVAILABLE = "unavailable"
PROFILE_FRAMES = 1000000  # a "frames" workload under `profile` outlives any warm-up plus sampling
ANALYZE = ROOT / "devtools/profile/analyze.py"
MIN_BUILD_FREE_GB = 5.0  # a build that fills the disk fails at the link (LNK1106) and can't write its logs
# Metrics judged per scene sample: one from the BENCH line, one from the OS.
SCENE_METRICS = ("frame_ms_p50", "peak_ws_mb")


# --- statistics and verdicts (one copy for every OS) ---


def wins_needed(n, alpha=0.05):
    """Smallest k such that k or more wins of n by chance alone has probability below alpha (one-sided)."""
    for k in range(1, n + 1):
        if sum(math.comb(n, i) for i in range(k, n + 1)) / 2**n < alpha:
            return k
    return None


def paired_diffs(control, candidate):
    """Relative per-round differences (cand - ctrl) / ctrl; a round missing or failed on either side is dropped."""
    return [(candidate[r] - control[r]) / control[r] for r in sorted(control) if r in candidate]


def floor_from_aa(diffs):
    """The A/A noise floor: the largest absolute paired difference (D-04)."""
    return max(abs(d) for d in diffs) if diffs else None


def wins_losses(diffs):
    wins = sum(d < 0 for d in diffs)
    return wins, len(diffs) - wins  # a tie is a loss for the candidate


def verdict(diffs, floor, is_target):
    """D-05 on the target workload, D-06 on every other one. Unrounded floats throughout."""
    if len(diffs) < MIN_ROUNDS:
        return "INCOMPLETE"
    if floor is None:
        return "NO-FLOOR"
    k = wins_needed(len(diffs))
    median = statistics.median(diffs)
    wins, losses = wins_losses(diffs)
    if is_target and median < -floor:
        return "ACCEPT" if wins >= k else "REJECT"
    # A slower target is judged like every other workload (so a slowed case of a multi-case target is reported).
    # Equal values (memory equal to the control) never pass the median test, so they never regress.
    return "REGRESSION" if median > floor and losses >= k else "WITHIN-NOISE"


def count_verdict(control_counts, candidate_counts, aa_identical):
    """Allocation counts are deterministic: any increase is flagged (D-06), unless A/A showed they are not."""
    if not aa_identical:
        return "COUNT-NONDETERMINISTIC"
    if any(candidate_counts.get(key, 0) > count for key, count in control_counts.items()):
        return "ALLOC-INCREASE"
    return "NO-INCREASE"


def aa_counts(control, twin):
    """A/A counts (round -> count): identical across both binaries and all rounds, or the spread."""
    missing = [label for label, series in (("control", control), ("aa", twin)) if not series]
    if missing:
        return f"n/a ({', '.join(missing)})"
    seen = set(control.values()) | set(twin.values())
    return "identical" if len(seen) == 1 else f"COUNT-NONDETERMINISTIC ({min(seen)}..{max(seen)})"


def compare_counts(control, candidate):
    """D-06 on counts (round -> count); a binary without counts (built before the counter) is n/a."""
    if not control or not candidate:
        return "n/a"
    deterministic = len(set(control.values())) == 1 and len(set(candidate.values())) == 1
    return count_verdict({"count": max(control.values())}, {"count": max(candidate.values())}, deterministic)


def max_gain(share, speedup):
    """Best case of a family (D-07): its share of the workload times the saved fraction of its time."""
    return share * (1 - 1 / speedup)


def skip_decision(gain, floor):
    return "skip" if gain < floor else "build"


# --- floors as data, round plan, clocks, metadata (one copy for every OS) ---


def floor_key(workload, case, metric, build, compiler):
    return "|".join((workload, case, metric, build, compiler))


def _major_os(build):
    """Windows "26100.9711": the build without the monthly update revision; a Linux kernel "6.17.8-2-cachyos": 6.17."""
    parts = str(build).split(".")
    return ".".join(parts[:2]) if len(parts) > 2 else parts[0]


def lookup_floor(floors, key, meta, n):
    """The stored floor, or why it can't be used: NO-FLOOR, or RE-MEASURE-AA after a machine change (D-04)."""
    entry = floors.get(key)
    if entry is None:
        return "NO-FLOOR"
    fields = ("compiler_version", "bios", "microcode")
    current = [meta.get(f) for f in fields] + [_major_os(meta.get("os_build"))]
    stored = [entry.get(f) for f in fields] + [_major_os(entry.get("os_build"))]
    if current != stored or UNAVAILABLE in current or entry.get("n") != n:
        return "RE-MEASURE-AA"
    return entry["floor"]


def plan_rounds(cold_seconds):
    return 11 if cold_seconds < CHEAP_S else 5


def budget_minutes(plan, binaries, builds):
    """plan: (seconds per process, n) per workload; every binary and build type runs each workload n times."""
    return sum(seconds * n for seconds, n in plan) * binaries * builds / 60


def clock_flags(round_mhz):
    """1 for rounds more than CLOCK_TOLERANCE off the median clock, 0 otherwise; an unread clock stays unread."""
    known = [mhz for mhz in round_mhz.values() if mhz != UNAVAILABLE]
    median = statistics.median(known) if known else None
    return {
        rnd: UNAVAILABLE if mhz == UNAVAILABLE else int(abs(mhz - median) / median > CLOCK_TOLERANCE)
        for rnd, mhz in round_mhz.items()
    }


def series_dir(base, stamp):
    """A new directory per series: two series started in the same second get stamp-2, stamp-3."""
    base.mkdir(parents=True, exist_ok=True)
    for i in range(1, 1000):
        path = base / (stamp if i == 1 else f"{stamp}-{i}")
        try:
            path.mkdir()
            return path
        except FileExistsError:
            continue
    raise RuntimeError(f"No free series directory for {stamp}")


def probe(read):
    """A metadata value, or "unavailable" when it can't be read: never left out, never written as 0."""
    try:
        value = read()
    except Exception:
        return UNAVAILABLE
    return UNAVAILABLE if value is None or value == "" else value


def git(*args, root=ROOT):
    return subprocess.run(["git", *args], cwd=root, capture_output=True, check=True).stdout


def machine_meta(mask):
    meta = {
        "commit": probe(lambda: git("rev-parse", "HEAD").decode().strip()),
        "dirty": probe(lambda: bool(git("status", "--porcelain").strip())),
        "diff_sha256": probe(lambda: hashlib.sha256(git("diff", "HEAD")).hexdigest()),
        "compiler": COMPILER,
    }
    if os.name == "nt":
        meta.update(msvc_toolset=probe(osb.msvc_toolset), vs_version=probe(osb.vs_version))
    else:
        meta.update(compiler_version=probe(osb.compiler_version), distribution=probe(osb.distribution))
    return {
        **meta,
        "cpu": probe(osb.cpu_name),
        "bios": probe(osb.bios),
        "microcode": probe(osb.microcode),
        "os_build": probe(osb.os_build),  # Linux: the kernel release
        # Windows has no package temperature without a kernel driver (D-14); Linux reads hwmon coretemp.
        "temperature": UNAVAILABLE if os.name == "nt" else probe(osb.package_temp),
        "affinity_mask": hex(mask),
        "python": platform.python_version(),
    }


def floor_meta(meta):
    version = meta["msvc_toolset"] if COMPILER == "msvc" else meta["compiler_version"]
    return {"compiler_version": version, **{f: meta[f] for f in ("bios", "microcode", "os_build")}}


def load_floors(path):
    return json.loads(Path(path).read_text(encoding="utf-8")) if Path(path).is_file() else {}


# --- workloads ---

# Third-party sources, fetched into devtools/out/bench/src/<name>/<sha7>/ at a pinned commit and refused
# unless HEAD is the pin; never vendored (T-02-10).
PINS = {
    "godot-benchmarks": {
        "url": "https://github.com/godotengine/godot-benchmarks",
        "commit": "ff8e67af1eac6054f011ee7137163b64bd0bf7fb",
        "license": "MIT",
    },
    "tps-demo": {  # the real level (D-08): 470 MB, no LFS
        "url": "https://github.com/godotengine/tps-demo",
        "commit": "a82f15448e9b015440d3bbdf5e10801b260c4e9f",
        "license": "MIT code, CC-BY 3.0 assets",
    },
    # Container micro-benchmark controls (bench.py micro), compiled into every side; include: the include root.
    "unordered_dense": {
        "url": "https://github.com/martinus/unordered_dense",
        "commit": "7c58a8b7a48e790fe72f6508eb3b46317cbba9d5",
        "version": "v5.3.3",
        "license": "MIT",
        "include": "include",
    },
    "gtl": {
        "url": "https://github.com/greg7mdp/gtl",
        "commit": "b9a2586edd9f052b6d071c751fe188b0e71bd2ee",
        "version": "v1.3.1",
        "license": "Apache-2.0",
        "include": "include",
    },
    "emhash": {
        "url": "https://github.com/ktprime/emhash",
        "commit": "e84fa51009e5040a82801290313b22976bfa56de",  # v1.3.0^{} (the tag object is 2f6301b972)
        "version": "v1.3.0",
        "license": "MIT",
        "include": "include",
    },
    "boost": {  # a release archive, not a git pin: size and sha256 checked on every use (T-02-17)
        "url": "https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.tar.gz",
        "archive": "boost-1.92.0-b2-nodocs.tar.gz",
        "size": 73484649,
        "sha256": "e2a814b3a158ab482c7a3d330f8bf5a7a8423d258a4e2fb396996e00fcee2111",  # = GitHub's asset digest
        "version": "1.92.0",
        "license": "BSL-1.0",
        "include_marker": "boost/unordered/unordered_flat_map.hpp",
    },
}
CONTROLS = ("unordered_dense", "gtl", "emhash", "boost")
# The gdbench subset (BENCH-03): file at the godot-benchmarks pin -> ITERATIONS (None keeps upstream's value).
# Copies change only `extends Benchmark` (to RefCounted) and that constant. Upstream's values took 27 s of
# timed calls (55 s per process with the warm-up calls, template_release); these keep a process near 10 s.
GDBENCH_FILES = {
    "benchmarks/gdscript/string_format.gd": 100_000,  # upstream 1_000_000 (8.9 s)
    "benchmarks/gdscript/string_manipulation.gd": 100_000,  # upstream 1_000_000 (11.9 s)
    "benchmarks/gdscript/array.gd": 200_000,  # upstream 2_000_000 (3.6 s)
    "benchmarks/gdscript/alloc.gd": 25_000,  # upstream 100_000 (1.8 s)
    "benchmarks/core/string_name.gd": None,
    "benchmarks/core/signal.gd": None,
}
GDBENCH_EXTRA = ROOT / "devtools/bench/workloads/gdbench_dictionary.gd"  # the fork's own (no upstream one)
EDITOR_PROJECT = "misc/large_world_test_project"  # only ever opened as a copy under the series directory


def fetch_git(name):
    """The pinned checkout in a directory of its own; a reused directory is checked the same way."""
    pin = PINS[name]
    path = OUT / "src" / name / pin["commit"][:7]
    if not path.is_dir():
        path.mkdir(parents=True)
        git("init", "-q", root=path)
        git("fetch", "-q", "--depth", "1", pin["url"], pin["commit"], root=path)
        git("checkout", "-q", "FETCH_HEAD", root=path)
    # Without its own .git (a partial fetch) rev-parse finds the fork's HEAD, which is not the pin either.
    head = probe(lambda: git("rev-parse", "HEAD", root=path).decode().strip())
    if head != pin["commit"]:
        raise RuntimeError(f"{path.relative_to(ROOT).as_posix()} is at {head}, not the pin {pin['commit']}:"
                           " delete it and run bench.py fetch")
    return path


def fetch_archive(name):
    """The pinned archive, downloaded into a new directory, checked (size, sha256) and extracted with the data filter."""
    pin = PINS[name]
    path = OUT / "src" / name / pin["version"]
    if not (path / ".extracted").is_file():
        shutil.rmtree(path, ignore_errors=True)
        partial = path.with_name(path.name + ".partial")
        shutil.rmtree(partial, ignore_errors=True)
        partial.mkdir(parents=True)
        print(f"FETCH {name}: downloading {pin['url']}", flush=True)
        urllib.request.urlretrieve(pin["url"], partial / pin["archive"])
        _check_archive(name, partial / pin["archive"])
        with tarfile.open(partial / pin["archive"]) as archive:
            archive.extractall(partial / "x", filter="data")  # no absolute paths, links or traversal
        (partial / ".extracted").write_text(pin["sha256"] + "\n", encoding="utf-8")
        partial.rename(path)
    _check_archive(name, path / pin["archive"])
    return path


def _check_archive(name, archive):
    pin = PINS[name]
    size = archive.stat().st_size
    if size != pin["size"]:
        raise RuntimeError(f"{rel(archive)} is {size} bytes, not {pin['size']}: delete it and run bench.py fetch")
    digest = sha_file(archive)
    if digest != pin["sha256"]:
        raise RuntimeError(f"{rel(archive)} has sha256 {digest}, not the pin {pin['sha256']}")


def fetch(name):
    return fetch_git(name) if "commit" in PINS[name] else fetch_archive(name)


def gdbench_copy(text, iterations):
    """(copy, changed line count) of one upstream file: only the extends line and the ITERATIONS constant change."""
    lines = text.splitlines(keepends=True)
    out = []
    for line in lines:
        body = line.rstrip("\r\n")
        end = line[len(body):]
        if body == "extends Benchmark":
            body = "extends RefCounted"
        elif iterations is not None and re.fullmatch(r"const ITERATIONS = [\d_]+", body):
            body = f"const ITERATIONS = {iterations}"
        out.append(body + end)
    if not any(line == "extends Benchmark" for line in text.splitlines()):
        raise RuntimeError("no `extends Benchmark` line: the upstream file changed shape")
    if iterations is not None and not any(line.startswith("const ITERATIONS = ") for line in "".join(out).splitlines()):
        raise RuntimeError("no `const ITERATIONS = <n>` line to reduce: the upstream file changed shape")
    return "".join(out), sum(a != b for a, b in zip(lines, out))


def prepare_gdbench():
    """The subset, copied from the pinned checkout into devtools/out/bench/gdbench/<sha7>/ for every series."""
    src, pin = fetch_git("godot-benchmarks"), PINS["godot-benchmarks"]
    dest = OUT / "gdbench" / pin["commit"][:7]
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    files = {}
    for rel, iterations in GDBENCH_FILES.items():
        name = Path(rel).name
        if name in files:
            raise RuntimeError(f"Two subset files named {name}: case names would collide")
        source = (src / rel).read_bytes().decode("utf-8")
        try:
            text, changed = gdbench_copy(source, iterations)
        except RuntimeError as error:
            raise RuntimeError(f"{rel}: {error}") from None
        (dest / name).write_bytes(text.encode("utf-8"))
        upstream = re.search(r"^const ITERATIONS = ([\d_]+)", source, re.M)
        files[name] = {"source": rel, "changed_lines": changed, "iterations": iterations,
                       "upstream_iterations": upstream.group(1) if upstream else None}
    info = {"repository": pin["url"], "commit": pin["commit"], "license": pin["license"], "files": files,
            "extra": GDBENCH_EXTRA.relative_to(ROOT).as_posix()}
    return dest, info


def prepare_editor_projects(out, labels):
    """A fresh mirror of the tracked test project per binary: the editor imports and writes into its own copy."""
    tracked = git("ls-files", "-z", "--", EDITOR_PROJECT).decode().split("\0")
    for label in labels:
        dest = out / "editor_project" / label
        shutil.rmtree(dest, ignore_errors=True)
        for rel in filter(None, tracked):
            target = dest / Path(rel).relative_to(EDITOR_PROJECT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, target)


LEVEL_SCRIPT = ROOT / "devtools/bench/workloads/real_level.gd"
LEVEL_ROBOTS = 32  # extra robots on top of the level's 4 (real_level.gd --robots)
LEVEL_FRAMES = 600
LEVEL_PRESET = f"""[preset.0]

name="bench"
platform="{'Windows Desktop' if PLATFORM == 'windows' else 'Linux'}"
runnable=true
export_filter="all_resources"
include_filter=""
exclude_filter=""
export_path=""
encrypt_pck=false
encrypt_directory=false

[preset.0.options]
"""
# Next to the .pck: what the project leaves to the machine, pinned without editing project.godot. The
# project asks for fullscreen, D3D12, HDR output and "Bullet" (a Godot 3 name; DEFAULT is GodotPhysics3D).
LEVEL_OVERRIDE = f"""[application]

run/max_fps=0

[display]

window/size/mode=0
window/size/viewport_width=1920
window/size/viewport_height=1080
window/size/window_width_override=1920
window/size/window_height_override=1080
window/vsync/vsync_mode=0
window/hdr/request_hdr_output=false

[physics]

3d/physics_engine="GodotPhysics3D"

[rendering]

rendering_device/driver.{PLATFORM}="vulkan"

[threading]

worker_pool/max_threads={GAME_WORKERS}
"""


def prepare_level(out, mask):
    """D-10: the control editor imports the fetched copy and exports one .pck for every binary of the series."""
    src, pin = fetch_git("tps-demo"), PINS["tps-demo"]
    editor = osb.engine_exe("editor", "bench")
    if not editor.is_file():
        raise RuntimeError(f"Missing {editor.relative_to(ROOT)}: the level is exported with the control editor")
    pck = out / "tps-demo.pck"
    pck.unlink(missing_ok=True)
    (src / "export_presets.cfg").write_text(LEVEL_PRESET, encoding="utf-8")  # gitignored by the demo
    for step in (["--import"], ["--export-pack", "bench", str(pck)]):
        result = osb.run_pinned([str(editor), "--headless", "--path", str(src), *step], ROOT, mask, 3600)
        if result["returncode"] != 0 or result["timed_out"]:
            tail = "\n".join(result["output"].splitlines()[-20:])
            raise RuntimeError(f"Level {step[0]} failed (exit {result['returncode']}):\n{tail}")
    if not pck.is_file():
        raise RuntimeError(f"--export-pack wrote no {pck.name}")
    (out / "override.cfg").write_text(LEVEL_OVERRIDE, encoding="utf-8")
    changed = git("status", "--porcelain", root=src).decode().splitlines()
    return pck, {"repository": pin["url"], "commit": pin["commit"], "license": pin["license"],
                 "pck_sha256": sha_file(pck), "pck_bytes": pck.stat().st_size,
                 "editor_changed_files": len(changed), "robots": LEVEL_ROBOTS, "frames": LEVEL_FRAMES,
                 "exported_by": editor.name}


def prepare_scene_project():
    # Template builds print "ERROR: Could not load global script cache." without the cache an editor import
    # writes. The bench project has no global classes: an empty list, in its gitignored .godot/ folder.
    cache = ROOT / "misc/scene_bench_project/.godot/global_script_class_cache.cfg"
    if not cache.is_file():
        cache.parent.mkdir(exist_ok=True)
        cache.write_text("list=[]\n", encoding="utf-8")


def _game(exe, script, user_args):
    return [
        str(exe), "--headless", "--path", str(ROOT / "misc/scene_bench_project"), "--fixed-fps", "60",
        "--script", str(ROOT / "devtools/bench/workloads" / script), "--", *user_args,
    ]


def scene_command(exe, params, ctx):
    scenario, count = params[0], int(params[1])
    # 100k-node runs take up to a second or more per frame (instancing): fewer frames, as the old driver.
    frames, warmup = (30, 5) if count >= 100000 else (120, 30)
    frames = ctx.get("frames", frames)  # profile: runs until the runner ends it
    return _game(exe, "scene.gd", [f"--scenario={scenario}", f"--count={count}", f"--frames={frames}",
                                   f"--warmup={warmup}"])


def script_command(exe, params, ctx):
    return _game(exe, "script.gd", [f"--mode={params[0]}", f"--frames={ctx.get('frames', 120)}", "--warmup=30"])


def gdbench_command(exe, params, ctx):
    return _game(exe, "gdbench.gd", [f"--files={ctx['gdbench'].as_posix()}", f"--extra={GDBENCH_EXTRA.as_posix()}"])


def level_command(exe, params, ctx):
    # Windowed on Vulkan (render-thread CPU needs a window); no audio device, so no audio thread or sound.
    return [str(exe), "--main-pack", str(ctx["level"]), "--script", str(LEVEL_SCRIPT), "--fixed-fps", "60",
            "--rendering-driver", "vulkan", "--resolution", "1920x1080", "--audio-driver", "Dummy", "--",
            f"--frames={ctx.get('frames', LEVEL_FRAMES)}", f"--robots={LEVEL_ROBOTS}"]


MICRO_ENGINE_PARITY = ("operations", "checksum")  # the same work in every process of a series, as in micro


def micro_engine_command(exe, params, ctx):
    # The [Bench] doctest cases of tests/fork/test_fork_bench_variant_keys.cpp; skipped in every other run.
    return [str(exe), "--test", "--test-case=*Bench*", "--no-skip"]


def engine_parity(refs, item, fields, names):
    """Stop the series unless a case does the work its first process did (every binary, every round)."""
    seen = {name: fields.get(name) for name in names}
    first = refs.setdefault(item, seen)
    if first != seen:
        raise RuntimeError(f"{item} ran different work: {first} then {seen}")


def _editor_json(ctx):
    return ctx["out"] / "editor_startup" / f"{ctx['label']}-{ctx['round']}.json"


def editor_command(exe, params, ctx):
    json_path = _editor_json(ctx)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.unlink(missing_ok=True)
    project = ctx["out"] / "editor_project" / ctx["label"]
    return [str(exe), "--headless", "--editor", "--quit", "--path", str(project), "--benchmark-file", str(json_path)]


def editor_fields(ctx):
    """Main::Start and First Scan from the editor's --benchmark-file JSON (seconds), in ms."""
    try:
        marks = json.loads(_editor_json(ctx).read_text(encoding="utf-8"))
        return {"main_start_ms": marks["[Startup] Main::Start"] * 1000,
                "first_scan_ms": marks["[Editor] First Scan"] * 1000}, None
    except (OSError, ValueError, KeyError, TypeError) as error:
        return None, f"benchmark file: {type(error).__name__} {error}"


SCENE_COUNTS = ("allocs", "reallocs")  # bench builds only (fork/alloc_count); judged by the count rule (D-06)
# name -> definition. metrics: judged per process, the time metric first; counts: allocation counts;
# cases: the process prints one BENCH line per case, each its own item <spec>/<case> with case_metrics and
# case_counts; info: printed and stored, never judged; builds: the build types it runs on (others read n/a);
# costly: n = 5 whatever the cold round takes; rounds: n whatever the cold round takes; editor_mode: starts 32
# workers whatever the project says (D-12 exception), so the count is recorded, not checked; workers: False for a
# process with no game (no worker check); tests_build: runs the bench tests build (bench[.<variant>].tests);
# parity: case fields every process must repeat; profile: how `profile` samples it, "frames" (runs PROFILE_FRAMES
# frames, sampled after --warmup for --seconds, then ended) or "launch" (sampled from launch until it exits,
# with the measured command: editor_startup is profiled with --quit, startup only).
WORKLOADS = {
    "scene": {"command": scene_command, "metrics": SCENE_METRICS, "counts": SCENE_COUNTS, "profile": "frames"},
    "script": {"command": script_command, "metrics": SCENE_METRICS, "counts": SCENE_COUNTS, "profile": "frames"},
    "gdbench": {"command": gdbench_command, "metrics": ("peak_ws_mb",), "counts": (), "cases": True,
                "case_metrics": ("time_ms",), "case_counts": SCENE_COUNTS, "costly": True, "profile": "launch"},
    "editor_startup": {"command": editor_command, "metrics": ("wall_ms", "peak_ws_mb"), "counts": (),
                       "info": ("main_start_ms", "first_scan_ms"), "read": editor_fields, "bench_line": False,
                       "builds": ("editor",), "costly": True, "editor_mode": True, "profile": "launch"},
    # BENCH-04 keys the standalone harness can't link (StringName, Variant, Dictionary), in the engine (02-07).
    "micro_engine": {"command": micro_engine_command, "metrics": ("peak_ws_mb",), "counts": (), "cases": True,
                     "case_metrics": ("ns_per_op",), "case_counts": ("allocs",), "rounds": 11, "workers": False,
                     "tests_build": True, "parity": MICRO_ENGINE_PARITY, "profile": "launch"},
    # D-09: load and CPU frame times are judged; GPU time and the wall frame time are stored, never judged.
    "level": {"command": level_command,
              "metrics": ("main_ms_p50", "instance_ms", "load_ms", "peak_ws_mb", "render_cpu_ms_p50"),
              "counts": ("load_allocs", "fly_allocs", "fly_reallocs"),
              "info": ("first_frame_ms", "main_ms_p95", "render_cpu_ms_p95", "gpu_ms_p50", "wall_ms"),
              "costly": True, "profile": "frames"},
}


# The fixed suite `--workloads all` runs; the standalone container harness is `micro --aa` (D-03).
SUITE = ("scene:instance:10000", "scene:churn:10000", "scene:reparent:10000", "scene:script:10000",
         "script:nodes", "script:script", "script:instance", "gdbench", "editor_startup", "level", "micro_engine")


def parse_workload(spec):
    name, *params = spec.split(":")
    if name not in WORKLOADS:
        raise ValueError(f"Unknown workload {name!r} (known: {', '.join(WORKLOADS)})")
    return name, params


def bench_lines(result, need_line=True):
    """(fields of every BENCH line, None) for a good process, or (None, reason) for a failed one."""
    lines = result["output"].splitlines()
    errors = [line for line in lines if line.startswith("ERROR") and "screen reader support driver" not in line]
    bench = [line for line in lines if line.startswith("BENCH: ")]
    reason = None
    if result["timed_out"]:
        reason = "timeout"
    elif any("SCRIPT ERROR" in line for line in lines):
        reason = next(line for line in lines if "SCRIPT ERROR" in line)
    elif errors:
        reason = errors[0]
    elif any(line.startswith("BENCH_FAIL") for line in lines):
        reason = next(line for line in lines if line.startswith("BENCH_FAIL"))
    elif result["returncode"] != 0:
        reason = f"exit code {result['returncode']}"
    elif need_line and not bench:
        reason = "no BENCH line"
    if reason:
        return None, reason.strip()
    return [dict(re.findall(r"(\w+)=(\S+)", line[len("BENCH: ") :])) for line in bench], None


def process_fields(spec, name, result, ctx, expected=()):
    """Item label -> (fields, reason) for one process. A multi-case workload's cases are items <spec>/<case>;
    a case in expected (the cold round's) but absent from this process is failed, never left out."""
    w = WORKLOADS[name]
    lines, reason = bench_lines(result, w.get("bench_line", True))
    extra = {}
    if reason is None and "read" in w:
        extra, reason = w["read"](ctx)
    if reason:
        return {spec: (None, reason), **{case: (None, reason) for case in sorted(expected)}}
    proc = {"peak_ws_mb": result["peak_ws_bytes"] / 1048576, "wall_ms": result["wall_s"] * 1000, **extra}
    if not w.get("cases"):
        return {spec: ({**(lines[0] if lines else {}), **proc}, None)}
    by_case = {f"{spec}/{line['case']}": line for line in lines if "case" in line}
    out = {spec: (proc, None)}
    for case in sorted(set(expected) | set(by_case)):
        out[case] = (by_case[case], None) if case in by_case else (None, "case missing (seen in the cold round)")
    return out


def metric_values(fields, reason, metrics):
    """Metric -> (value, reason); value None means failed. Never turns a failure into a number."""
    if reason:
        return {m: (None, reason) for m in metrics}
    sample = {}
    for metric in metrics:
        try:
            value = float(fields[metric])
        except (KeyError, ValueError):
            sample[metric] = (None, f"{metric} missing or not a number")
            continue
        sample[metric] = (value, None) if math.isfinite(value) and value > 0 else (None, f"{metric}={value}")
    return sample


def count_values(fields, names):
    """Count -> int, or None (n/a) when the sample failed or the binary has no counter. Zero is a count."""
    fields = fields or {}
    return {name: int(fields[name]) if re.fullmatch(r"-?\d+", str(fields.get(name, ""))) else None for name in names}


def item_def(spec, name, params, label):
    """An item's floor case and fields; time is the metric the accept rule judges (None: no time metric)."""
    w = WORKLOADS[name]
    if label == spec:
        return {"case": ":".join(params), "metrics": w["metrics"], "counts": w["counts"], "info": w.get("info", ()),
                "time": None if w.get("cases") else w["metrics"][0]}
    return {"case": label[len(spec) + 1 :], "metrics": w["case_metrics"], "counts": w["case_counts"], "info": (),
            "time": w["case_metrics"][0]}


def runs_on(name, build):
    return build in WORKLOADS.get(name, {}).get("builds", (build,))  # micro is not in WORKLOADS


def is_tests(name):
    return bool(WORKLOADS.get(name, {}).get("tests_build"))


# --- machine checks (D-13: read, refuse with the fix, never change) ---


def run_checks(mask, max_load, perf=False):
    """Every pre-series check; returns (values read, problems). Reads only. perf: counters or a profile follow."""
    values, problems = {"bench_mask": hex(mask)}, []

    def power():
        found = osb.check_power_plan()
        values["power_plan"] = found.pop("scheme")
        values["parking"] = found

    def compilers():
        osb.ensure_idle()
        values["compilers"] = "idle"

    def load():
        values["bench_cpu_load_pct"] = round(osb.bench_cpu_load(mask), 1)
        if values["bench_cpu_load_pct"] > max_load:
            raise RuntimeError(
                f"The bench CPUs ({hex(mask)}) are {values['bench_cpu_load_pct']}% busy, above {max_load:g}%:"
                " close what is running there ("
                + ("Task Manager, Details, sort by CPU" if os.name == "nt" else "top, then 1 for each CPU")
                + ") and run again."
            )

    if os.name != "nt":  # governor, EPP, boost, sched_ext, compiler hold, perf_event_paranoid (D-13, D-17)
        state = osb.read_state(mask)
        values.update(state)
        problems += osb.state_problems(state, osb.mask_cpus(mask), perf)
    for check in (power, compilers, load) if os.name == "nt" else (compilers, load):
        try:
            check()
        except (RuntimeError, OSError) as error:
            problems.append(str(error))
    if os.name == "nt":
        values["game_mode"] = probe(osb.game_mode)
    values["disk_free_gb"] = round(shutil.disk_usage(ROOT).free / 1e9, 1)
    return values, problems


# --- series: aa and compare ---


def print_rules(n):
    k = wins_needed(n)
    print(f"RULES n={n} k={k} (one-sided sign test, chance passes below 5%)")
    print(f"  accept (target workload): median paired difference faster than the A/A floor and wins >= k={k} of {n}")
    print(f"  regression (other workloads, time and peak memory): median paired difference slower than the floor"
          f" and losses >= k={k} of {n}")
    print("  a tie counts as a loss for the candidate; a round failed on either side is dropped;"
          f" fewer than {MIN_ROUNDS} pairs is INCOMPLETE", flush=True)


def ensure_worktree(sha, create):
    """The previous commit in its own git worktree, so the live tree stays as it is; custom.py copied in."""
    path = OUT / "wt" / sha[:7]
    if not path.is_dir():
        if not create:
            raise RuntimeError(f"No worktree for {sha[:10]}; run without --skip-build")
        git("worktree", "add", "--detach", str(path), sha)
    head = git("rev-parse", "HEAD", root=path).decode().strip()
    if head != sha:
        raise RuntimeError(f"devtools/out/bench/wt/{sha[:7]} is at {head[:10]}, not {sha[:10]}; remove it first")
    if (ROOT / "custom.py").is_file():
        shutil.copyfile(ROOT / "custom.py", path / "custom.py")
    return path


def worker_count(command, mask):
    result = osb.run_pinned(command[:1] + ["--verbose"] + command[1:], ROOT, mask, TIMEOUT_S)
    match = re.search(r"WorkerThreadPool: (\d+) threads", result["output"])
    return int(match.group(1)) if match else None


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def run_series(args, sides):
    """sides: the binaries in round-1 order, each {label, suffix, variant, defines, root, commit}."""
    mode = args.command
    workloads = [(spec, *parse_workload(spec)) for spec in args.workloads.split(",")]
    builds = args.builds.split(",")
    labels = [side["label"] for side in sides]
    mask = osb.p_core_mask()
    checks, problems = run_checks(mask, args.max_load, args.counters)
    if problems:
        raise RuntimeError("Refusing to start; nothing was changed.\n" + "\n".join(problems))
    if mode == "compare":
        sides[0]["root"] = ensure_worktree(sides[0]["commit"], not args.skip_build)
    # (build type, tests build) pairs some workload runs on: micro_engine runs the bench tests build.
    kinds = sorted({(b, is_tests(name)) for _, name, _ in workloads for b in builds if runs_on(name, b)})
    if not args.skip_build and checks["disk_free_gb"] < MIN_BUILD_FREE_GB:
        raise RuntimeError(f"{checks['disk_free_gb']} GB free on the repository's disk, below {MIN_BUILD_FREE_GB} GB:"
                           " a bench variant writes about 3 GB of objects and a 0.7 GB pdb; delete the objects"
                           " (bin/obj/**/*.<variant>.obj) of variants you no longer need")
    if not args.skip_build:
        for build, tests in kinds:
            for side in sides:
                source_root = side["root"] if side["root"] != ROOT else None
                osb.build(build, side["variant"], side["defines"], source_root, tests)
    exes = {(b, s["label"], t): osb.engine_exe(b, s["suffix"] + (".tests" if t else ""), s["root"])
            for b, t in kinds for s in sides}
    missing = [str(exe.relative_to(ROOT)) for exe in exes.values() if not exe.is_file()]
    if missing:
        raise RuntimeError(f"Missing {', '.join(missing)}; run without --skip-build")
    checks["home_mask"] = hex(osb.set_home_affinity(mask))  # the runner and its helpers stay off the bench CPUs
    out = Path(args.out).resolve()  # absolute: the engine resolves relative paths from the --path project
    ctx = {"out": out}  # per process: + label and round
    names = {name for _, name, _ in workloads}
    prepare_scene_project()
    if "gdbench" in names:
        ctx["gdbench"], gdbench_info = prepare_gdbench()
    if "editor_startup" in names:
        prepare_editor_projects(out, labels)
    if "level" in names:
        ctx["level"], level_info = prepare_level(out, mask)
        print(f"LEVEL {level_info['pck_sha256'][:16]} ({level_info['pck_bytes'] / 1048576:.0f} MB),"
              f" {level_info['editor_changed_files']} files changed by the editor in the copy", flush=True)
    meta = {
        **machine_meta(mask), **checks,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "runner_args": vars(args),
        "binaries": [  # round-1 order
            {
                "build": build, "label": s["label"], "exe": exes[build, s["label"], tests].name, "commit": s["commit"],
                "build_ps1_args" if os.name == "nt" else "build_command":
                    osb.build_args(build, s["variant"], s["defines"], "<worktree>" if s["root"] != ROOT else None,
                                   tests),
                "scons_args": osb.scons_args(build, s["variant"], s["defines"], tests),
                "cppdefines": osb.bench_defines(s["defines"]),
            }
            for build, tests in kinds
            for s in sides
        ],
    }
    if "gdbench" in names:
        meta["gdbench"] = gdbench_info
    if "level" in names:
        meta["level"] = level_info

    raw, values, clocks, temps, cold_s = [], {}, {}, {}, {}  # values: (item, metric, build, label) -> {round: value}
    counts = {}  # (item, count, build, label) -> {round: count}; a binary without the counter has none
    # item label -> (spec, name, params, item_def): every workload's process item, plus the cases found.
    items = {spec: (spec, name, params, item_def(spec, name, params, spec)) for spec, name, params in workloads}
    expected = {}  # spec -> the case items its cold round printed (multi-case workloads)
    parity = {}  # case item -> the parity fields of its first process

    def row(rnd, position, label, build, name, case, metric, value, failed, reason, result, wall):
        return {
            "round": rnd, "position": position, "binary": label, "build": build, "workload": name, "case": case,
            "metric": metric, "value": value, "failed": failed, "reason": reason, "cpu_s": repr(result["cpu_s"]),
            "wall_s": f"{wall:.3f}",
        }

    def record(rnd, position, label, build, spec, name, params, result, wall, call):
        """Rows for one process; returns what the progress line shows."""
        fields = process_fields(spec, name, result, call, expected.get(spec, ()))
        if rnd == 0 and WORKLOADS[name].get("cases"):
            expected.setdefault(spec, set()).update(k for k, (f, _) in fields.items() if k != spec and f is not None)
        shown, failed_cases = [], []
        for item, (item_fields, reason) in fields.items():
            d = item_def(spec, name, params, item)
            items.setdefault(item, (spec, name, params, d))
            if item != spec and item_fields is not None and "parity" in WORKLOADS[name]:
                engine_parity(parity, item, item_fields, WORKLOADS[name]["parity"])
            for metric, (value, why) in metric_values(item_fields, reason, d["metrics"]).items():
                raw.append(row(rnd, position, label, build, name, d["case"], metric,
                               "" if value is None else repr(value), int(value is None), why or "", result, wall))
                if rnd > 0 and value is not None:
                    values.setdefault((item, metric, build, label), {})[rnd] = value
                if item == spec:
                    shown.append(f"{metric}={value:.3f}" if value is not None else f"{metric}=FAILED ({why})")
                elif value is None:
                    failed_cases.append(f"{item}: {why}")
            for metric, count in count_values(item_fields, d["counts"]).items():
                raw.append(row(rnd, position, label, build, name, d["case"], metric, "" if count is None else count,
                               0, "n/a" if count is None else "", result, wall))
                if rnd > 0 and count is not None:
                    counts.setdefault((item, metric, build, label), {})[rnd] = count
                if item == spec:
                    shown.append(f"{metric}={'n/a' if count is None else count}")
            for metric in d["info"]:  # printed and stored, never judged
                value = (item_fields or {}).get(metric)
                value = None if value is None else float(value)  # BENCH line fields are strings
                raw.append(row(rnd, position, label, build, name, d["case"], metric,
                               "" if value is None else repr(value), int(value is None), reason or "", result, wall))
                if rnd > 0 and value is not None:
                    values.setdefault((item, metric, build, label), {})[rnd] = value
                if item == spec and value is not None:
                    shown.append(f"{metric}={value:.1f}")
        if len(fields) > 1:
            shown.append(f"cases={len(fields) - 1} failed={len(failed_cases)}")
        return shown, failed_cases

    def run_round(rnd, order, specs):
        osb.ensure_idle()
        sampler = probe(lambda: osb.ClockSampler(mask))
        position = 0
        for build in builds:
            for spec, name, params in specs:
                if not runs_on(name, build):
                    continue
                for label in order:
                    position += 1
                    call = {**ctx, "label": label, "round": f"r{rnd}"}
                    command = WORKLOADS[name]["command"](exes[build, label, is_tests(name)], params, call)
                    start = time.perf_counter()
                    result = osb.run_pinned(command, ROOT, mask, TIMEOUT_S)
                    wall = time.perf_counter() - start
                    if result["child_mask"] != mask:
                        raise RuntimeError(f"Child affinity {hex(result['child_mask'])}, expected {hex(mask)}")
                    if rnd == 0:
                        cold_s[spec] = max(cold_s.get(spec, 0.0), wall)
                    shown, failed_cases = record(rnd, position, label, build, spec, name, params, result, wall, call)
                    tag = "cold" if rnd == 0 else f"r{rnd}"
                    print(f"[{tag}] {build:16} {spec} {label:9} {' '.join(shown)} cpu_s={result['cpu_s']:.2f}",
                          flush=True)
                    for line in failed_cases[:3]:
                        print(f"  FAILED {line}", flush=True)
        clocks[rnd] = UNAVAILABLE if sampler == UNAVAILABLE else probe(sampler.stop)
        temps[rnd] = probe(lambda: sampler.temperature_c)  # Linux hwmon; the Windows sampler has none
        shown = clocks[rnd] if clocks[rnd] == UNAVAILABLE else f"{clocks[rnd]:.0f} MHz"
        print(f"[{'cold' if rnd == 0 else f'r{rnd}'}] effective clock of the bench CPUs: {shown}", flush=True)

    print(f"ORDER round 1: {', '.join(labels)}; even rounds reversed", flush=True)
    run_round(0, labels, workloads)  # cold, discarded (driver and file caches per exe; editor_startup imports)
    for spec, name, _ in workloads:
        if WORKLOADS[name].get("cases") and not expected.get(spec):
            reasons = sorted({r["reason"] for r in raw if r["workload"] == name and r["failed"]})
            raise RuntimeError(f"{spec} printed no cases in the cold round: {'; '.join(reasons) or 'no BENCH lines'}")

    # D-12: game runs must start GAME_WORKERS workers; editor-mode runs start 32 and are recorded only.
    found = {False: {}, True: {}}
    for editor_mode in found:
        first = next((w for w in workloads if bool(WORKLOADS[w[1]].get("editor_mode")) == editor_mode
                      and WORKLOADS[w[1]].get("workers", True)), None)
        if first is None:
            continue
        spec, name, params = first
        for b in (b for b in builds if runs_on(name, b)):
            for s in labels:
                command = WORKLOADS[name]["command"](exes[b, s, is_tests(name)], params,
                                                     {**ctx, "label": s, "round": "workers"})
                found[editor_mode][f"{b}/{s}"] = worker_count(command, mask)
        print(f"WORKERS {spec}: {', '.join(f'{k}={v}' for k, v in found[editor_mode].items())}")
    meta["workers"] = found[False]
    if found[True]:
        meta["editor_workers"] = found[True]
    wrong = {k: v for k, v in found[False].items() if v != GAME_WORKERS}
    if wrong:
        raise RuntimeError(
            f"Game runs must start {GAME_WORKERS} worker threads, found {wrong}: check [threading]"
            " worker_pool/max_threads in misc/scene_bench_project/project.godot"
        )

    # D-03: plan the rounds from the cold round and refuse a plan over the budget.
    def costly(spec, name):
        return WORKLOADS[name].get("costly") or cold_s.get(spec, 0.0) >= CHEAP_S

    n_of = {spec: args.rounds or WORKLOADS[name].get("rounds")
            or (5 if costly(spec, name) else plan_rounds(cold_s.get(spec, 0.0)))
            for spec, name, _ in workloads}
    print(f"\nPLAN {'workload':28} {'s/process':>9} {'class':>6} {'n':>3} {'k':>3} {'processes':>9} {'minutes':>7}")
    total = 0.0
    for spec, name, _ in workloads:
        n, run_builds = n_of[spec], [b for b in builds if runs_on(name, b)]
        processes = n * len(sides) * len(run_builds)
        minutes = budget_minutes([(cold_s.get(spec, 0.0), n)], len(sides), len(run_builds))
        total += minutes
        print(f"PLAN {spec:28} {cold_s.get(spec, 0.0):9.2f} {'costly' if costly(spec, name) else 'cheap':>6} {n:3}"
              f" {wins_needed(n):3} {processes:9} {minutes:7.1f}")
    print(f"PLAN total {total:.1f} min of measurement (budget {args.budget:g} min per compiler)", flush=True)
    meta["plan"] = {s: {"seconds": round(cold_s.get(s, 0.0), 3), "n": n_of[s], "k": wins_needed(n_of[s])}
                    for s in n_of}
    meta["plan_minutes"] = round(total, 2)
    (out / "meta.json").write_text(json.dumps(meta, indent=2, default=str), encoding="utf-8")
    if total > args.budget:
        raise RuntimeError(f"The plan takes {total:.1f} min, above --budget {args.budget:g}: run fewer workloads"
                           " or build types, or pass --rounds 5")
    for n in sorted(set(n_of.values())):
        print_rules(n)

    for rnd in range(1, max(n_of.values()) + 1):
        order = labels if rnd % 2 else labels[::-1]  # odd rounds in round-1 order, even rounds reversed
        run_round(rnd, order, [w for w in workloads if n_of[w[0]] >= rnd])
    found_counters = counter_pass(workloads, builds, labels, exes, ctx, mask, out) if args.counters else None

    flags = clock_flags(clocks)
    for entry in raw:
        mhz = clocks[entry["round"]]
        entry["clock_mhz"] = mhz if mhz == UNAVAILABLE else f"{mhz:.0f}"
        entry["clock_flag"] = flags[entry["round"]]
    flagged = [rnd for rnd, flag in flags.items() if flag == 1]
    if flagged:
        print(f"CLOCK rounds {flagged} are more than {CLOCK_TOLERANCE:.0%} off the median clock (flagged in raw.csv)")
    write_csv(out / "raw.csv", raw)
    meta["clock_mhz"] = {str(r): (m if m == UNAVAILABLE else round(m)) for r, m in clocks.items()}
    if any(t != UNAVAILABLE for t in temps.values()):
        meta["temperature_c"] = {str(r): (t if t == UNAVAILABLE else round(t, 1)) for r, t in temps.items()}
    if found_counters is not None:
        meta["counter_events"] = osb.COUNTER_EVENTS
    (out / "meta.json").write_text(json.dumps(meta, indent=2, default=str), encoding="utf-8")
    ordered = sorted_items(items)
    summary = summarize_aa(args, ordered, builds, values, n_of, meta) if mode == "aa" else \
        summarize_compare(args, ordered, builds, values, n_of, meta)
    if found_counters is not None:
        add_counter_columns(summary, found_counters, labels)
    write_csv(out / "summary.csv", summary)
    count_rows = summarize_counts(mode, ordered, builds, counts)
    if count_rows:
        write_csv(out / "counts.csv", count_rows)
    print(f"\nraw.csv, summary.csv and meta.json in {out}")


def counter_pass(workloads, builds, labels, exes, ctx, mask, out):
    """BENCH-08: one perf stat pass per binary and workload after the timed rounds, never part of a timed or
    memory sample. Returns (spec, build, label) -> counter values (None: not counted)."""
    found = {}
    for build in builds:
        for spec, name, params in workloads:
            if not runs_on(name, build):
                continue
            for label in labels:
                call = {**ctx, "label": label, "round": "counters"}
                command = WORKLOADS[name]["command"](exes[build, label, is_tests(name)], params, call)
                path = out / "counters" / f"{profile_stem(spec, build)}.{label}.txt"
                path.parent.mkdir(parents=True, exist_ok=True)
                result = osb.run_pinned(osb.counter_command(path, command), ROOT, mask, TIMEOUT_S)
                values = dict.fromkeys(COUNTER_COLUMNS)
                if result["returncode"] == 0 and not result["timed_out"] and path.is_file():
                    values = osb.parse_perf_stat(path.read_text(encoding="utf-8"))
                else:
                    tail = "; ".join(result["output"].splitlines()[-3:])
                    print(f"COUNTERS {spec} {build} {label}: FAILED (exit {result['returncode']}): {tail}", flush=True)
                found[spec, build, label] = values
    return found


def counter_text(values):
    """instructions, cycles, IPC, cache-misses and miss rate of one binary; n/a for a missing count."""
    def ratio(a, b):
        return f"{values[a] / values[b]:.3f}" if values[a] is not None and values[b] else "n/a"

    shown = {k: "n/a" if v is None else v for k, v in values.items()}
    return (f"instructions={shown['instructions']} cycles={shown['cycles']} IPC={ratio('instructions', 'cycles')}"
            f" cache-misses={shown['cache_misses']} miss-rate={ratio('cache_misses', 'cache_references')}")


def add_counter_columns(summary, found, labels):
    """Counter columns (label=value per binary) on each workload's rows of summary.csv, and COUNTERS lines after
    the medians. Reported only: the verdicts are made before and never read them."""
    printed = set()
    for row in summary:
        per_label = {label: found.get((row["workload"], row["build"], label)) for label in labels}
        for column in COUNTER_COLUMNS:
            row[column] = " ".join(f"{label}={'n/a' if v[column] is None else v[column]}"
                                   for label, v in per_label.items() if v)
        if any(per_label.values()) and (row["workload"], row["build"]) not in printed:
            printed.add((row["workload"], row["build"]))
            for label, v in per_label.items():
                if v:
                    print(f"COUNTERS {row['workload']} {row['build']} {label}: {counter_text(v)} (not judged)")


def sorted_items(items):
    """BENCH-06 order: workload, then case; summaries then go by build type, then metric (both sorted)."""
    return sorted(items.items(), key=lambda kv: (kv[1][1], kv[1][3]["case"], kv[0]))


def only_on(name):
    return f"n/a ({' and '.join(WORKLOADS[name]['builds'])} only)"


def print_info(item, d, build, values, labels):
    for metric in sorted(d["info"]):
        medians = {label: values.get((item, metric, build, label)) for label in labels}
        shown = ", ".join(f"{label} {statistics.median(v.values()):.1f}" if v else f"{label} n/a"
                          for label, v in medians.items())
        print(f"INFO {item} {metric} {build}: {shown} (median; not judged)")


def summarize_aa(args, items, builds, values, n_of, meta):
    summary, floors = [], load_floors(args.floors)
    machine, date = floor_meta(meta), time.strftime("%Y-%m-%d")
    print()
    for item, (spec, name, params, d) in items:
        for build in sorted(builds):  # build types are never pooled
            for metric in sorted(d["metrics"]):
                if not runs_on(name, build):
                    print(f"FLOOR {item} {metric} {build}: {only_on(name)}")
                    summary.append({"workload": item, "metric": metric, "build": build, "n": 0, "control_median": "",
                                    "aa_median": "", "median_diff": "", "wins": "", "losses": "",
                                    "floor": only_on(name)})
                    continue
                control = values.get((item, metric, build, "control"), {})
                twin = values.get((item, metric, build, "aa"), {})
                diffs = paired_diffs(control, twin)
                wins, losses = wins_losses(diffs)
                floor = floor_from_aa(diffs) if len(diffs) >= MIN_ROUNDS else None
                row = {
                    "workload": item, "metric": metric, "build": build, "n": len(diffs),
                    "control_median": statistics.median(control.values()) if control else "",
                    "aa_median": statistics.median(twin.values()) if twin else "",
                    "median_diff": statistics.median(diffs) if diffs else "",
                    "wins": wins, "losses": losses, "floor": "" if floor is None else floor,
                }
                summary.append(row)
                if diffs:
                    print(f"{item} {metric} {build}: control {row['control_median']:.3f}, aa {row['aa_median']:.3f},"
                          f" median diff {row['median_diff'] * 100:+.2f}%, wins {wins} losses {losses}")
                shown = "INCOMPLETE" if floor is None else f"{floor * 100:.2f}%"
                print(f"FLOOR {item} {metric} {build}: {shown} (n={len(diffs)})")
                if floor is not None:
                    floors[floor_key(name, d["case"], metric, build, COMPILER)] = {
                        "workload": name, "case": d["case"], "metric": metric, "build": build, "compiler": COMPILER,
                        "floor": floor, "n": len(diffs), "date": date, **machine,
                    }
            if runs_on(name, build):
                print_info(item, d, build, values, ("control", "aa"))
    if args.write_floors:
        Path(args.floors).parent.mkdir(parents=True, exist_ok=True)
        Path(args.floors).write_text(json.dumps(floors, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Floors written to {args.floors}")
    return summary


def summarize_compare(args, items, builds, values, n_of, meta):
    floors = load_floors(args.floors)
    machine = floor_meta(meta)
    targets = set(args.target.split(","))
    summary = []
    print()

    def median(series):
        return statistics.median(series.values()) if series else float("nan")

    for item, (spec, name, params, d) in items:
        for build in sorted(builds):
            for metric in sorted(d["metrics"]):
                # The accept rule judges the target's time metric; memory and other workloads get the regression rule.
                is_target = (spec in targets or item in targets) and metric == d["time"]
                if not runs_on(name, build):
                    print(f"VERDICT {item} {metric} {build}: {only_on(name)}")
                    summary.append({
                        "workload": item, "metric": metric, "build": build, "n": 0, "previous_median": "",
                        "candidate_median": "", "control_median": "", "median_diff": "", "previous_median_diff": "",
                        "wins": "", "losses": "", "floor": "", "rule": "accept" if is_target else "regression",
                        "verdict": only_on(name),
                    })
                    continue
                prev, cand, ctrl = (values.get((item, metric, build, label), {})
                                    for label in ("previous", "candidate", "control"))
                diffs, prev_diffs = paired_diffs(ctrl, cand), paired_diffs(ctrl, prev)
                wins, losses = wins_losses(diffs)
                floor = lookup_floor(floors, floor_key(name, d["case"], metric, build, COMPILER), machine, n_of[spec])
                token = verdict(diffs, None if isinstance(floor, str) else floor, is_target)
                if token == "NO-FLOOR":
                    token = floor  # NO-FLOOR, or RE-MEASURE-AA when the machine changed since the A/A run
                med = statistics.median(diffs) if diffs else float("nan")
                prev_med = statistics.median(prev_diffs) if prev_diffs else float("nan")
                shown_floor = floor if isinstance(floor, str) else f"{floor * 100:.2f}%"
                print(f"{item} {metric} {build}: previous {median(prev):.3f}, candidate {median(cand):.3f},"
                      f" control {median(ctrl):.3f}; candidate vs control {med * 100:+.2f}%"
                      f" (wins {wins} losses {losses} of {len(diffs)}, k={wins_needed(len(diffs))});"
                      f" previous vs control {prev_med * 100:+.2f}%; floor {shown_floor};"
                      f" {'accept' if is_target else 'regression'} rule")
                print(f"VERDICT {item} {metric} {build}: {token}", flush=True)
                summary.append({
                    "workload": item, "metric": metric, "build": build, "n": len(diffs),
                    "previous_median": median(prev), "candidate_median": median(cand), "control_median": median(ctrl),
                    "median_diff": med, "previous_median_diff": prev_med, "wins": wins, "losses": losses,
                    "floor": "" if isinstance(floor, str) else floor, "rule": "accept" if is_target else "regression",
                    "verdict": token,
                })
            if runs_on(name, build):
                print_info(item, d, build, values, ("previous", "candidate", "control"))
    return summary


def summarize_counts(mode, items, builds, counts):
    """COUNTS lines: A/A determinism in aa mode, the D-06 count rule (candidate vs control) in compare mode."""
    summary = []
    for item, (_, name, _, d) in items:
        for build in sorted(b for b in builds if runs_on(name, b)):
            for metric in sorted(d["counts"]):
                series = {label: counts.get((item, metric, build, label), {})
                          for label in ("control", "aa", "previous", "candidate")}
                shown = {label: f"{min(s.values())}..{max(s.values())}" if s else "n/a" for label, s in series.items()}
                if mode == "aa":
                    token = aa_counts(series["control"], series["aa"])
                    print(f"COUNTS {item} {metric} {build}: {token} (control {shown['control']}, aa {shown['aa']})")
                else:
                    token = compare_counts(series["control"], series["candidate"])
                    print(f"COUNTS {item} {metric} {build}: previous {shown['previous']}, candidate"
                          f" {shown['candidate']}, control {shown['control']}")
                    print(f"VERDICT {item} {metric} {build}: {token}", flush=True)
                summary.append({"workload": item, "metric": metric, "build": build, "verdict": token,
                                **{f"{label}_counts": shown[label] for label in series}})
    return summary


# --- container micro-benchmark (BENCH-04): one standalone process prints every case ---

MICRO_SOURCE = ROOT / "devtools/bench/micro/containers_bench.cpp"
# The previous side compiles these from --prev (an include directory before the tree); the rest comes from the tree.
MICRO_HEADERS = (
    "core/templates/vector.h", "core/templates/cowdata.h", "core/templates/a_hash_map.h", "core/templates/hash_map.h",
    "core/templates/hash_set.h", "core/templates/hashfuncs.h", "core/templates/hashfuncs.cpp",
    "core/templates/rb_map.h", "core/templates/rb_set.h", "core/templates/list.h", "core/templates/local_vector.h",
    "core/string/ustring.h", "core/string/ustring.cpp",
)
# Not the whole String TU (it needs Variant, translation and OS): only these definitions, verbatim.
STRING_METHODS = (
    "bool String::operator==(const String &p_str) const",
    "uint32_t String::hash() const",
    "bool String::operator<(const char32_t *p_str) const",
    "bool String::operator<(const String &p_str) const",
)
# core/os/fork_mimalloc.c is C and empty unless the side's defines set FORK_BACKEND_ALLOCATOR=1.
MICRO_TREE_INPUTS = ("core/os/memory.cpp", "core/os/fork_alloc_counters.cpp", "core/os/fork_mimalloc.c",
                     "core/profiling/profiling.gen.h")
MICRO_FIELDS = ("ns_per_op", "operations", "live_bytes", "allocs", "checksum")
MICRO_PARITY = ("operations", "checksum", "live_bytes", "allocs")
MICRO_BUILD = "standalone"  # the build field of a micro floor key
MICRO_KEYS = ("i64", "ptr", "str")
MICRO_OPS = ("insert", "hit", "miss", "erase", "churn", "iterate", "copy", "push")
CONTROL_OPS = ("insert", "hit", "miss", "iterate")
ORDERED_CONTAINERS = ("rbmap", "rbset")
ORDERED_CONTROLS = ("gtl_btree", "std_map")
HASH_CASES = "hash_bytes_"  # a candidate may change the byte hash, so their checksums are not compared across sides


def parse_case(name):
    """(container, key, op, N) of a matrix case <container>_<key>_<op>_<N>, or None (string_copy_*, hash_bytes_*)."""
    parts = name.rsplit("_", 3)
    if len(parts) != 4 or parts[1] not in MICRO_KEYS or parts[2] not in MICRO_OPS or not parts[3].isdigit():
        return None
    return parts[0], parts[1], parts[2], int(parts[3])


def micro_mismatch(ref, rows, fields, skip_checksum=False):
    """The first difference between two harness outputs on fields, or None."""
    if [r["case"] for r in ref] != [r["case"] for r in rows]:
        return "the case list differs"
    for a, b in zip(ref, rows):
        for field in fields:
            if field == "checksum" and skip_checksum and a["case"].startswith(HASH_CASES):
                continue
            if a[field] != b[field]:
                return f"{a['case']} {field} {a[field]} != {b[field]}"
    return None


def micro_check(refs, label, rows, first, cross):
    """Stop the series unless rows repeat the label's first process exactly and do the first side's work."""
    refs.setdefault(label, rows)
    problem = micro_mismatch(refs[label], rows, MICRO_PARITY)
    if problem:
        raise RuntimeError(f"Non-repeatable workload in {label}: {problem}")
    problem = micro_mismatch(refs[first], rows, cross, skip_checksum=True)
    if problem:
        raise RuntimeError(f"{label} and {first} ran different workloads: {problem}")


def micro_rows(result):
    """The harness CSV as row dicts; a failed process raises (micro processes are deterministic)."""
    lines = result["output"].splitlines()
    fail = next((line for line in lines if line.startswith("BENCH_FAIL")), None)
    if result["timed_out"] or result["returncode"] or fail:
        reason = fail or ("timeout" if result["timed_out"] else f"exit code {result['returncode']}")
        raise RuntimeError(f"Micro harness failed: {reason}")
    header = "case," + ",".join(MICRO_FIELDS)
    if header not in lines:
        raise RuntimeError(f"No harness header {header!r} in the output")
    rows = []
    for line in lines[lines.index(header) + 1 :]:
        values = line.split(",")
        if len(values) != len(MICRO_FIELDS) + 1:
            raise RuntimeError(f"Bad harness line {line!r}")
        row = dict(zip(("case", *MICRO_FIELDS), values))
        if not (math.isfinite(float(row["ns_per_op"])) and float(row["ns_per_op"]) > 0 and int(row["operations"]) > 0):
            raise RuntimeError(f"Invalid timing: {line}")
        rows.append(row)
    return rows


def fastest_controls(medians):
    """case -> (ratio of its median to the fastest matching ctl_ row, that row) for Godot cases with controls.
    Ordered containers (RBMap, RBSet) are matched with the ordered controls only, hash containers with the hash ones."""
    out = {}
    for case, ns in medians.items():
        parsed = parse_case(case)
        if not parsed or parsed[0].startswith("ctl_") or parsed[2] not in CONTROL_OPS:
            continue
        container, key, op, n = parsed
        ordered = container in ORDERED_CONTAINERS
        ctl = {c: v for c, v in medians.items() if c.startswith("ctl_") and parse_case(c)[1:] == (key, op, n)
               and (parse_case(c)[0][len("ctl_"):] in ORDERED_CONTROLS) == ordered}
        if ctl:
            best = min(ctl, key=ctl.get)
            out[case] = (ns / ctl[best], best)
    return out


def layout_sentinel(summary):
    """(control rows beyond their floor, control rows with a floor) in a compare summary (Pitfall 2)."""
    rows = [r for r in summary if r["workload"].startswith("micro/ctl_") and r["floor"] != "" and r["n"]]
    return sum(abs(r["median_diff"]) > r["floor"] for r in rows), len(rows)


def extract_string_methods(ustring_cpp, dest):
    text = ustring_cpp.read_text(encoding="utf-8")
    methods = []
    for signature in STRING_METHODS:
        found = re.findall(r"^" + re.escape(signature) + r" \{\n.*?^\}", text, re.MULTILINE | re.DOTALL)
        if len(found) != 1:
            raise RuntimeError(f"Cannot extract the String method {signature} from {ustring_cpp}")
        methods.append(found[0])
    dest.write_text('#include "core/string/ustring.h"\n\n' + "\n\n".join(methods) + "\n", encoding="utf-8")


def sha_file(path):
    # In chunks: a whole-file read of the 569 MB level pack made the runner's peak RSS what Linux reports as every
    # smaller child's (bench_linux.child_peak_bytes).
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def rel(path):
    path = Path(path)
    return path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else path.as_posix()


def control_includes():
    """(include directories, pin info) of the third-party controls, fetched and checked against their pins."""
    dirs, info = [], {}
    for name in CONTROLS:
        pin, path = PINS[name], fetch(name)
        if "include_marker" in pin:
            found = sorted((path / "x").glob(f"*/{pin['include_marker']}"))
            if len(found) != 1:
                raise RuntimeError(f"No single {pin['include_marker']} in {rel(path)}: the archive changed shape")
            include = found[0].parents[len(Path(pin["include_marker"]).parts) - 1]
        else:
            include = path / pin["include"]
        dirs.append(include)
        info[name] = {"version": pin["version"], "license": pin["license"], "url": pin["url"],
                      **{k: pin[k] for k in ("commit", "sha256") if k in pin}, "include": rel(include)}
    return dirs, info


def build_micro(side, build_dir, controls):
    """Compile one side; returns its meta entry (sources and their sha256)."""
    shutil.rmtree(build_dir, ignore_errors=True)
    build_dir.mkdir(parents=True)
    src = ROOT
    if side["prev"]:
        src = build_dir / "src"  # the container headers at --prev, shadowing the tree
        for name in MICRO_HEADERS:
            try:
                content = git("show", f"{side['commit']}:{name}")
            except subprocess.CalledProcessError:
                continue  # absent at that commit: the tree's copy is used (recorded below)
            (src / name).parent.mkdir(parents=True, exist_ok=True)
            (src / name).write_bytes(content)
    string_methods = build_dir / "string_methods.cpp"
    extract_string_methods(src / "core/string/ustring.cpp", string_methods)
    modern = int("uint64_t hash_buffer64(" in (src / "core/templates/hashfuncs.h").read_text(encoding="utf-8"))
    inputs = [MICRO_SOURCE, ROOT / "core/os/memory.cpp", ROOT / "core/os/fork_alloc_counters.cpp", string_methods,
              src / "core/templates/hashfuncs.cpp", ROOT / "core/os/fork_mimalloc.c"]
    includes = ([src] if src != ROOT else []) + [ROOT, ROOT / "platform" / PLATFORM,
                                                 ROOT / "thirdparty/mimalloc/include"] + controls
    defines = [f"BENCH_MODERN_HASH={modern}"] + (["BENCH_CONTROLS"] if controls else []) + side["defines"]
    version = osb.compile_micro(build_dir, inputs, includes, defines)
    sources = {rel(ROOT / name): sha_file(ROOT / name) for name in (rel(MICRO_SOURCE), *MICRO_TREE_INPUTS)}
    for name in MICRO_HEADERS:
        path = src / name if (src / name).is_file() else ROOT / name
        sources[name] = sha_file(path) + ("" if path.is_relative_to(build_dir) or not side["prev"] else " (tree)")
    return {"label": side["label"], "commit": side["commit"], "exe": rel(build_dir / osb.MICRO_EXE),
            "defines": defines, "flags": osb.MICRO_FLAGS + osb.MICRO_LINK,
            "c_flags": osb.MICRO_C_FLAGS,
            "cl_version" if COMPILER == "msvc" else "compiler_version": version, "sources": sources}


def run_micro(args, sides):
    mask = osb.p_core_mask()
    checks, problems = run_checks(mask, args.max_load)
    if problems:
        raise RuntimeError("Refusing to start; nothing was changed.\n" + "\n".join(problems))
    out = Path(args.out).resolve()
    controls, control_info = control_includes()
    print(f"CONTROLS {', '.join(control_info) or 'none (run bench.py fetch)'}", flush=True)
    built = {}
    for side in sides:
        print(f"BUILD micro {side['label']}", flush=True)
        built[side["label"]] = build_micro(side, out / "build" / side.get("dir", side["label"]), controls)
    labels = [side["label"] for side in sides]
    checks["home_mask"] = hex(osb.set_home_affinity(mask))
    meta = {**machine_meta(mask), **checks, "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "runner_args": vars(args), "binaries": list(built.values()), "controls": control_info}
    (out / "meta.json").write_text(json.dumps(meta, indent=2, default=str), encoding="utf-8")
    # The tree sides must still match what was built ("changed after build").
    for side in sides:
        if side["prev"]:
            continue
        for name, digest in built[side["label"]]["sources"].items():
            if sha_file(ROOT / name) != digest:
                raise RuntimeError(f"{name} changed after the {side['label']} build")

    raw, values, clocks, refs = [], {}, {}, {}
    cross = MICRO_PARITY if args.aa else ("operations", "checksum")  # compare: memory is a result, not a failure

    def run_round(rnd, order):
        osb.ensure_idle()
        sampler = probe(lambda: osb.ClockSampler(mask))
        for position, label in enumerate(order, 1):
            result = osb.run_pinned([str(ROOT / built[label]["exe"])], ROOT, mask, TIMEOUT_S)
            if result["child_mask"] != mask:
                raise RuntimeError(f"Child affinity {hex(result['child_mask'])}, expected {hex(mask)}")
            rows = micro_rows(result)
            micro_check(refs, label, rows, labels[0], cross)
            tag = "cold" if rnd == 0 else f"r{rnd}"
            print(f"[{tag}] micro {label:9} cases={len(rows)} wall_s={result['wall_s']:.2f}"
                  f" peak_ws_mb={result['peak_ws_bytes'] / 1048576:.0f}", flush=True)
            for row in rows:
                raw.append({"round": rnd, "position": position, "binary": label, **row,
                            "wall_s": f"{result['wall_s']:.3f}"})
                if rnd > 0:
                    values.setdefault((f"micro/{row['case']}", "ns_per_op", MICRO_BUILD, label), {})[rnd] = \
                        float(row["ns_per_op"])
        clocks[rnd] = UNAVAILABLE if sampler == UNAVAILABLE else probe(sampler.stop)

    print(f"ORDER round 1: {', '.join(labels)}; even rounds reversed", flush=True)
    run_round(0, labels)  # the warm-up pair (cold), discarded
    print_rules(args.samples)
    for rnd in range(1, args.samples + 1):
        run_round(rnd, labels if rnd % 2 else labels[::-1])
    flags = clock_flags(clocks)
    for entry in raw:
        mhz = clocks[entry["round"]]
        entry["clock_mhz"] = mhz if mhz == UNAVAILABLE else f"{mhz:.0f}"
        entry["clock_flag"] = flags[entry["round"]]
    write_csv(out / "raw.csv", raw)
    meta["clock_mhz"] = {str(r): (m if m == UNAVAILABLE else round(m)) for r, m in clocks.items()}
    (out / "meta.json").write_text(json.dumps(meta, indent=2, default=str), encoding="utf-8")

    cases = [row["case"] for row in refs[labels[0]]]
    items = [(f"micro/{case}", ("micro", "micro", [], {"case": case, "metrics": ("ns_per_op",), "counts": (),
                                                        "info": (), "time": "ns_per_op"})) for case in cases]
    n_of, builds = {"micro": args.samples}, [MICRO_BUILD]
    summary = summarize_aa(args, items, builds, values, n_of, meta) if args.aa else \
        summarize_compare(args, items, builds, values, n_of, meta)
    medians = {case: statistics.median(values[(f"micro/{case}", "ns_per_op", MICRO_BUILD, "control")].values())
               for case in cases}
    ratios = fastest_controls(medians)
    for row in summary:
        ratio, best = ratios.get(row["workload"][len("micro/"):], ("", ""))
        row["fastest_control"], row["ratio_to_fastest_control"] = best, ratio
        if best:
            print(f"RATIO {row['workload']}: {ratio:.2f}x the fastest control ({best}, control binary medians)")
    if not args.aa:
        moved, total = layout_sentinel(summary)
        print(f"LAYOUT-SENTINEL: {moved} of {total} control rows moved" + (
            " (the run measured code layout, Pitfall 2)" if moved else "") + ("" if total else " (no control floors)"))
        count_rows = []
        for case in cases:
            ctrl, cand = (next(r for r in refs[label] if r["case"] == case) for label in ("control", "candidate"))
            for field in ("live_bytes", "allocs"):
                token = "INCREASE" if int(cand[field]) > int(ctrl[field]) else "NO-INCREASE"
                count_rows.append({"workload": f"micro/{case}", "metric": field, "control": ctrl[field],
                                   "candidate": cand[field], "verdict": token})
                if cand[field] != ctrl[field]:
                    print(f"COUNTS micro/{case} {field}: control {ctrl[field]}, candidate {cand[field]}: {token}")
        write_csv(out / "counts.csv", count_rows)
    write_csv(out / "summary.csv", summary)
    print(f"\nraw.csv, summary.csv and meta.json in {out}")


# --- profile: a sampling profile of one workload (BENCH-07), with its family shares (BENCH-10) ---


def profile_stem(spec, build):
    return spec.replace(":", "_").replace("/", "_") + "." + build


def run_profile(args, _sides):
    """Start the bench control exe pinned, attach sampler.exe to its PID, end it, analyze the TSV."""
    name, params = parse_workload(args.workload)
    w = WORKLOADS[name]
    if not runs_on(name, args.build):
        raise RuntimeError(f"{name} runs only on {', '.join(w['builds'])}")
    exe = osb.engine_exe(args.build, "bench.tests" if is_tests(name) else "bench")
    if not exe.is_file():
        how = (f"devtools\\build\\build.ps1 -Bench{' -Tests' if is_tests(name) else ''} -Target {args.build}"
               if os.name == "nt" else " ".join(osb.build_args(args.build, None, None, None, is_tests(name))))
        raise RuntimeError(f"Missing {exe.relative_to(ROOT)}: build it with {how} (profile never builds)")
    mask = osb.p_core_mask()
    checks, problems = run_checks(mask, args.max_load, perf=True)
    if problems:
        raise RuntimeError("Refusing to start; nothing was changed.\n" + "\n".join(problems))
    sampler = osb.sampler_exe()
    checks["home_mask"] = hex(osb.set_home_affinity(mask))  # the runner and the sampler stay off the bench CPUs
    out = Path(args.out).resolve()
    ctx = {"out": out, "label": "control", "round": 0}
    prepare_scene_project()
    if name == "gdbench":
        ctx["gdbench"], _ = prepare_gdbench()
    if name == "editor_startup":
        prepare_editor_projects(out, ["control"])
    if name == "level":
        ctx["level"], _ = prepare_level(out, mask)
    form = w["profile"]
    if form == "frames":
        ctx["frames"] = PROFILE_FRAMES
    command = w["command"](exe, params, ctx)
    stem = out / profile_stem(args.workload, args.build)
    tsv = stem.with_name(stem.name + ".tsv")
    print(f"PROFILE {args.workload} {exe.name} ({form})", flush=True)
    with open(stem.with_name(stem.name + ".log"), "w", encoding="utf-8") as log:
        if os.name != "nt":  # perf record, then perf script into the same TSV (bench_linux.perf_profile)
            proc, sampled = osb.perf_profile(sampler, command, mask, form, args.warmup, args.seconds, stem, log,
                                             TIMEOUT_S)
        else:
            proc = osb.start_pinned(command, ROOT, mask, stdout=log)
            try:
                if form == "frames":
                    time.sleep(args.warmup)
                    if proc.poll() is not None:
                        raise RuntimeError(f"{exe.name} ended during the warm-up (exit {proc.returncode});"
                                           f" see {log.name}")
                seconds = args.seconds if form == "frames" else TIMEOUT_S  # launch: the sampler stops when it exits
                sampled = subprocess.run([str(sampler), str(proc.pid), str(seconds), str(args.interval_us), str(tsv),
                                          str(exe.parent)], capture_output=True, text=True)
            finally:
                if proc.poll() is None:
                    proc.kill()  # the runner's own child only
                proc.wait()
    if sampled.returncode != 0 or not tsv.is_file():
        raise RuntimeError(f"{Path(sampler).name} failed (exit {sampled.returncode}): {sampled.stderr.strip()}")
    meta_path = out / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else {"profiles": {}}
    meta.update({**machine_meta(mask), **checks})
    meta["profiles"][stem.name] = {
        "workload": args.workload, "build": args.build, "exe": exe.name, "form": form, "command": command[1:],
        "pid": proc.pid, "returncode": proc.returncode, "sampler": sampled.stdout.strip(),
        "seconds": args.seconds if form == "frames" else None, "warmup_s": args.warmup if form == "frames" else 0,
        "interval_us": args.interval_us, "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "runner_args": vars(args),
    }
    meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    summary = subprocess.run([sys.executable, str(ANALYZE), str(tsv), "summary"], capture_output=True, text=True,
                             check=True).stdout
    stem.with_name(stem.name + ".summary.txt").write_text(summary, encoding="utf-8")
    families = subprocess.run([sys.executable, str(ANALYZE), str(tsv), "families", "--json",
                               str(stem.with_name(stem.name + ".families.json"))], capture_output=True, text=True,
                              check=True).stdout
    print(sampled.stdout.strip())
    print(families, end="", flush=True)


def run_check(args):
    mask = osb.p_core_mask()
    values, problems = run_checks(mask, args.max_load, perf=True)  # check covers counters and profiles too
    try:
        osb.acquire_lock(OUT / ".lock").close()
        values["runner_lock"] = "free"
    except RuntimeError as error:
        problems.append(str(error))
    meta = machine_meta(mask)
    shown = ("msvc_toolset", "vs_version") if os.name == "nt" else ("compiler_version", "distribution", "temperature")
    for key, value in {**values, **{k: meta[k] for k in ("cpu", "bios", "microcode", "os_build", *shown,
                                                          "python")}}.items():
        print(f"{key}: {value}")
    for problem in problems:
        print(f"REFUSE: {problem}")
    print("CHECK " + ("FAILED" if problems else "OK"))
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="read and print the machine state the series need; change nothing")
    aa = sub.add_parser("aa", help="A/A noise floor: control against a same-source twin")
    compare = sub.add_parser("compare", help="previous, candidate and same-source control; verdict against the control")
    sub.add_parser("fetch", help="fetch every pinned third-party source into devtools/out/bench/src/ and check it")
    micro = sub.add_parser("micro", help="container micro-benchmark: --aa, or previous, candidate and control")
    prof = sub.add_parser("profile", help="sampling profile of one workload with its family shares (no build)")
    prof.add_argument("--workload", required=True, help="e.g. scene:instance:10000")
    prof.add_argument("--build", default="editor", choices=("editor", "template_release"))
    prof.add_argument("--seconds", type=float, default=15.0, help="sampling time of a frames workload")
    prof.add_argument("--warmup", type=float, default=3.0, help="seconds before sampling a frames workload")
    prof.add_argument("--interval-us", type=int, default=1000)
    prof.add_argument("--out", default=None)
    micro.add_argument("--aa", action="store_true", help="A/A floors: the control against a second control build")
    micro.add_argument("--name", default="cand", help="candidate label (its build directory)")
    micro.add_argument("--defines", default="", help="space-separated macros for the candidate (/D)")
    micro.add_argument("--prev", default=None, help="git ref of the previous side's container headers")
    micro.add_argument("--target", default="", help="comma list of micro/<case> items judged by the accept rule")
    micro.add_argument("--samples", type=int, default=11, help="rounds after the warm-up pair (at least 5)")
    micro.add_argument("--floors", default=str(DEFAULT_FLOORS))
    micro.add_argument("--write-floors", action="store_true", help="merge the measured floors into --floors")
    micro.add_argument("--out", default=None)
    for parsed in (check, aa, compare, micro, prof):
        parsed.add_argument("--max-load", type=float, default=MAX_LOAD_PCT,
                            help="refuse above this mean %% busy of the bench CPUs; floors and shares use the default")
        parsed.add_argument("--compiler", default=COMPILERS[0], choices=("msvc", "gcc", "clang"),
                            help=f"the build and floor key (default {COMPILERS[0]}; this OS: {', '.join(COMPILERS)})")
    for series in (aa, compare):
        series.add_argument("--workloads", required=True, help="comma list, e.g. scene:instance:10000, or all (SUITE)")
        series.add_argument("--builds", default="editor,template_release")
        series.add_argument("--rounds", type=int, default=None, help="n for every workload (default: planned)")
        series.add_argument("--budget", type=float, default=30.0, help="minutes of measurement allowed (D-03)")
        series.add_argument("--floors", default=str(DEFAULT_FLOORS))
        series.add_argument("--skip-build", action="store_true")
        series.add_argument("--out", default=None)
        series.add_argument("--counters", action="store_true",
                            help="Linux: one perf stat pass per binary and workload after the rounds (not judged)")
    aa.add_argument("--write-floors", action="store_true", help="merge the measured floors into --floors")
    compare.add_argument("--name", default="cand", help="candidate label: build.ps1 -Variant, binary bench.<name>")
    compare.add_argument("--defines", default="", help="space-separated macros for the candidate (cppdefines)")
    compare.add_argument("--prev", required=True, help="git ref of the previous build, e.g. origin/dev")
    compare.add_argument("--target", required=True, help="comma list of workloads judged by the accept rule")
    args = parser.parse_args()
    if getattr(args, "compiler", COMPILER) not in COMPILERS:
        parser.error(f"--compiler {args.compiler} does not build on this OS (use {' or '.join(COMPILERS)})")
    if getattr(args, "counters", False) and os.name == "nt":
        parser.error("--counters reads perf stat (Linux only)")
    set_compiler(getattr(args, "compiler", COMPILER))
    if args.command == "check":
        raise SystemExit(run_check(args))
    if args.command == "fetch":
        for name, pin in PINS.items():
            shown = rel(fetch(name))
            what = "HEAD is the pin" if "commit" in pin else "size and sha256 are the pin"
            print(f"FETCH {name} {pin.get('commit', pin.get('sha256'))} ({pin['license']}) in {shown}: {what}")
        return
    if args.command == "micro":
        return main_micro(parser, args)
    if args.command == "profile":
        try:
            parse_workload(args.workload)
        except ValueError as error:
            parser.error(str(error))
        return locked(args, run_profile, None)
    if args.rounds is not None and args.rounds < MIN_ROUNDS:
        parser.error(f"Use at least {MIN_ROUNDS} rounds: the sign test cannot pass below that")
    if args.workloads == "all":
        args.workloads = ",".join(SUITE)
    specs = args.workloads.split(",")
    for spec in specs:
        try:
            parse_workload(spec)
        except ValueError as error:
            parser.error(str(error))
    head = git("rev-parse", "HEAD").decode().strip()
    control = {"label": "control", "suffix": "bench", "variant": None, "defines": None, "root": ROOT, "commit": head}
    if args.command == "aa":
        sides = [control, {**control, "label": "aa", "suffix": "bench.aa", "variant": "aa"}]
    else:
        unknown = set(args.target.split(",")) - set(specs)
        if unknown:
            parser.error(f"--target {', '.join(sorted(unknown))} is not in --workloads")
        try:
            prev = git("rev-parse", "--verify", f"{args.prev}^{{commit}}").decode().strip()  # resolved once (T-02-07)
        except subprocess.CalledProcessError:
            parser.error(f"--prev {args.prev} is not a commit")
        candidate = {**control, "label": "candidate", "suffix": f"bench.{args.name}", "variant": args.name,
                     "defines": args.defines or None}
        sides = [{**control, "label": "previous", "commit": prev}, candidate, control]
    locked(args, run_series, sides)


def set_compiler(name):
    global COMPILER
    COMPILER = osb.COMPILER = name  # bench_linux picks gcc or clang builds and binaries by it


def locked(args, run, sides):
    """One runner at a time; a series directory per run. An interrupted series writes no summary.csv."""
    lock = osb.acquire_lock(OUT / ".lock")  # held until the process ends
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + args.command
    if args.out is None:
        args.out = str(series_dir(OUT / "profile" if args.command == "profile" else OUT, stamp))
    else:
        Path(args.out).mkdir(parents=True, exist_ok=True)
    try:
        run(args, sides)
    finally:
        lock.close()


def main_micro(parser, args):
    if args.samples < MIN_ROUNDS:
        parser.error(f"Use at least {MIN_ROUNDS} samples: the sign test cannot pass below that")
    if args.aa == bool(args.prev):
        parser.error("Pass either --aa or --prev")
    if not (ROOT / "core/profiling/profiling.gen.h").is_file():
        parser.error("Missing core/profiling/profiling.gen.h (SCons-generated): build Godot once first")
    head = git("rev-parse", "HEAD").decode().strip()
    control = {"label": "control", "commit": head, "prev": False, "defines": []}
    if args.aa:
        sides = [control, {**control, "label": "aa"}]
    else:
        try:
            prev = git("rev-parse", "--verify", f"{args.prev}^{{commit}}").decode().strip()
        except subprocess.CalledProcessError:
            parser.error(f"--prev {args.prev} is not a commit")
        sides = [{**control, "label": "previous", "commit": prev, "prev": True},
                 {**control, "label": "candidate", "dir": args.name, "defines": args.defines.split()}, control]
    locked(args, run_micro, sides)


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:  # a refusal or a failed build: the reason, not a traceback
        print(f"REFUSE: {error}", flush=True)
        raise SystemExit(1) from None
