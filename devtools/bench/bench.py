"""Benchmark runner: same-source builds, pinned alternating rounds, paired differences and verdicts.

  python devtools/bench/bench.py check
  python devtools/bench/bench.py aa --workloads scene:instance:10000 --builds editor,template_release
  python devtools/bench/bench.py compare --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev
      --target scene:churn:10000 --workloads scene:churn:10000,scene:instance:10000

`check` reads the machine state (power plan, core parking, idle compilers, bench-CPU load, Game Mode,
the runner lock) and prints every value. `aa` builds a control (extra_suffix bench) and an A/A twin
(bench.aa) and measures the noise floor. `compare` runs three binaries: the previous commit (built in a
git worktree under devtools/out/bench/wt/), the candidate (same source plus --defines) and the
same-source control; the verdict compares the candidate with the control only. Every series runs one
discarded cold round, plans its rounds within --budget, then alternates the order every other round.
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
import time
from pathlib import Path

if os.name == "nt":
    import bench_windows as osb
else:
    osb = None  # the Linux adapter comes with the Linux plan

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "devtools/out/bench"
DEFAULT_FLOORS = ROOT / "devtools/bench/floors.json"
COMPILER = "msvc"  # the only compiler of the Windows adapter
MIN_ROUNDS = 5
TIMEOUT_S = 600
CHEAP_S = 3.0  # processes under this get n = 11 rounds, the others n = 5 (D-03)
MAX_LOAD_PCT = 10.0  # mean % Processor Utility of the bench CPUs before a series (starting value, --max-load)
CLOCK_TOLERANCE = 0.05  # rounds this far off the series median clock are flagged (D-14)
GAME_WORKERS = 14  # the worker pool cap of the bench project (D-12)
UNAVAILABLE = "unavailable"
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
    if is_target:
        if median < -floor:
            return "ACCEPT" if wins >= k else "REJECT"
        return "WITHIN-NOISE"
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
    return str(build).split(".")[0]  # "26100.9711": the build, without the monthly update revision


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
    return {
        "commit": probe(lambda: git("rev-parse", "HEAD").decode().strip()),
        "dirty": probe(lambda: bool(git("status", "--porcelain").strip())),
        "diff_sha256": probe(lambda: hashlib.sha256(git("diff", "HEAD")).hexdigest()),
        "compiler": COMPILER,
        "msvc_toolset": probe(osb.msvc_toolset),
        "vs_version": probe(osb.vs_version),
        "cpu": probe(osb.cpu_name),
        "bios": probe(osb.bios),
        "microcode": probe(osb.microcode),
        "os_build": probe(osb.os_build),
        "temperature": UNAVAILABLE,  # no package temperature on Windows without a kernel driver (D-14)
        "affinity_mask": hex(mask),
        "python": platform.python_version(),
    }


def floor_meta(meta):
    return {"compiler_version": meta["msvc_toolset"], **{f: meta[f] for f in ("bios", "microcode", "os_build")}}


def load_floors(path):
    return json.loads(Path(path).read_text(encoding="utf-8")) if Path(path).is_file() else {}


# --- workloads ---


def scene_command(exe, scenario, count):
    count = int(count)
    # 100k-node runs take up to a second or more per frame (instancing): fewer frames, as the old driver.
    frames, warmup = (30, 5) if count >= 100000 else (120, 30)
    return [
        str(exe), "--headless", "--path", str(ROOT / "misc/scene_bench_project"), "--fixed-fps", "60",
        "--script", str(ROOT / "devtools/bench/workloads/scene.gd"), "--",
        f"--scenario={scenario}", f"--count={count}", f"--frames={frames}", f"--warmup={warmup}",
    ]


def prepare_scene_project():
    # Template builds print "ERROR: Could not load global script cache." without the cache an editor import
    # writes. The bench project has no global classes: an empty list, in its gitignored .godot/ folder.
    cache = ROOT / "misc/scene_bench_project/.godot/global_script_class_cache.cfg"
    if not cache.is_file():
        cache.parent.mkdir(exist_ok=True)
        cache.write_text("list=[]\n", encoding="utf-8")


# name -> (command builder, judged metrics with the time metric first, editor mode). Editor-mode runs start
# 32 workers whatever the project says (D-12 exception): their count is recorded, not checked.
WORKLOADS = {"scene": (scene_command, SCENE_METRICS, False)}
# Allocation counts per workload (bench builds only, fork/alloc_count): judged by the count rule, never by the
# time or memory rules (D-06).
COUNT_METRICS = {"scene": ("allocs", "reallocs")}


def parse_workload(spec):
    name, *params = spec.split(":")
    if name not in WORKLOADS:
        raise ValueError(f"Unknown workload {name!r} (known: {', '.join(WORKLOADS)})")
    return name, params


def bench_fields(result):
    """(BENCH line fields, None) for a good sample, or (None, reason) for a failed one."""
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
    elif not bench:
        reason = "no BENCH line"
    if reason:
        return None, reason.strip()
    return dict(re.findall(r"(\w+)=(\S+)", bench[0][len("BENCH: ") :])), None


def parse_sample(result, metrics):
    """Metric -> (value, reason); value None means failed. Never turns a failure into a number."""
    values, reason = bench_fields(result)
    if reason:
        return {m: (None, reason) for m in metrics}
    values["peak_ws_mb"] = result["peak_ws_bytes"] / 1048576
    sample = {}
    for metric in metrics:
        try:
            value = float(values[metric])
        except (KeyError, ValueError):
            sample[metric] = (None, f"{metric} missing or not a number")
            continue
        sample[metric] = (value, None) if math.isfinite(value) and value > 0 else (None, f"{metric}={value}")
    return sample


def parse_counts(result, names):
    """Count -> int, or None (n/a) when the sample failed or the binary has no counter. Zero is a count."""
    values, _ = bench_fields(result)
    values = values or {}
    return {name: int(values[name]) if values.get(name, "").isdigit() else None for name in names}


# --- machine checks (D-13: read, refuse with the fix, never change) ---


def run_checks(mask, max_load):
    """Every pre-series check; returns (values read, problems). Reads only."""
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
                " close what is running there (Task Manager, Details, sort by CPU) and run again."
            )

    for check in (power, compilers, load):
        try:
            check()
        except (RuntimeError, OSError) as error:
            problems.append(str(error))
    values["game_mode"] = probe(osb.game_mode)
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
    checks, problems = run_checks(mask, args.max_load)
    if problems:
        raise RuntimeError("Refusing to start; nothing was changed.\n" + "\n".join(problems))
    if mode == "compare":
        sides[0]["root"] = ensure_worktree(sides[0]["commit"], not args.skip_build)
    if not args.skip_build:
        for build in builds:
            for side in sides:
                source_root = side["root"] if side["root"] != ROOT else None
                osb.build(build, side["variant"], side["defines"], source_root)
    exes = {(b, s["label"]): osb.engine_exe(b, s["suffix"], s["root"]) for b in builds for s in sides}
    missing = [str(exe.relative_to(ROOT)) for exe in exes.values() if not exe.is_file()]
    if missing:
        raise RuntimeError(f"Missing {', '.join(missing)}; run without --skip-build")
    if any(name == "scene" for _, name, _ in workloads):
        prepare_scene_project()
    checks["home_mask"] = hex(osb.set_home_affinity(mask))  # the runner and its helpers stay off the bench CPUs
    out = Path(args.out)
    meta = {
        **machine_meta(mask), **checks,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "runner_args": vars(args),
        "binaries": [  # round-1 order
            {
                "build": build, "label": s["label"], "exe": exes[build, s["label"]].name, "commit": s["commit"],
                "build_ps1_args": osb.build_args(build, s["variant"], s["defines"],
                                                 "<worktree>" if s["root"] != ROOT else None),
                "scons_args": osb.scons_args(build, s["variant"], s["defines"]),
                "cppdefines": osb.bench_defines(s["defines"]),
            }
            for build in builds
            for s in sides
        ],
    }

    raw, values, clocks, cold_s = [], {}, {}, {}  # values: (spec, metric, build, label) -> {round: value}
    counts = {}  # (spec, count, build, label) -> {round: count}; a binary without the counter has none

    def run_round(rnd, order, specs):
        osb.ensure_idle()
        sampler = probe(lambda: osb.ClockSampler(mask))
        position = 0
        for build in builds:
            for spec, name, params in specs:
                command_for, metrics, _ = WORKLOADS[name]
                for label in order:
                    position += 1
                    start = time.perf_counter()
                    result = osb.run_pinned(command_for(exes[build, label], *params), ROOT, mask, TIMEOUT_S)
                    wall = time.perf_counter() - start
                    if result["child_mask"] != mask:
                        raise RuntimeError(f"Child affinity {hex(result['child_mask'])}, expected {hex(mask)}")
                    if rnd == 0:
                        cold_s[spec] = max(cold_s.get(spec, 0.0), wall)
                    shown = []
                    for metric, (value, reason) in parse_sample(result, metrics).items():
                        raw.append({
                            "round": rnd, "position": position, "binary": label, "build": build, "workload": name,
                            "case": ":".join(params), "metric": metric, "value": "" if value is None else repr(value),
                            "failed": int(value is None), "reason": reason or "", "cpu_s": repr(result["cpu_s"]),
                            "wall_s": f"{wall:.3f}",
                        })
                        shown.append(f"{metric}={value:.3f}" if value is not None else f"{metric}=FAILED ({reason})")
                        if rnd > 0 and value is not None:
                            values.setdefault((spec, metric, build, label), {})[rnd] = value
                    for metric, count in parse_counts(result, COUNT_METRICS.get(name, ())).items():
                        raw.append({
                            "round": rnd, "position": position, "binary": label, "build": build, "workload": name,
                            "case": ":".join(params), "metric": metric, "value": "" if count is None else count,
                            "failed": 0, "reason": "n/a" if count is None else "", "cpu_s": repr(result["cpu_s"]),
                            "wall_s": f"{wall:.3f}",
                        })
                        shown.append(f"{metric}={'n/a' if count is None else count}")
                        if rnd > 0 and count is not None:
                            counts.setdefault((spec, metric, build, label), {})[rnd] = count
                    tag = "cold" if rnd == 0 else f"r{rnd}"
                    print(f"[{tag}] {build:16} {spec} {label:9} {' '.join(shown)} cpu_s={result['cpu_s']:.2f}",
                          flush=True)
        clocks[rnd] = UNAVAILABLE if sampler == UNAVAILABLE else probe(sampler.stop)
        shown = clocks[rnd] if clocks[rnd] == UNAVAILABLE else f"{clocks[rnd]:.0f} MHz"
        print(f"[{'cold' if rnd == 0 else f'r{rnd}'}] effective clock of the bench CPUs: {shown}", flush=True)

    print(f"ORDER round 1: {', '.join(labels)}; even rounds reversed", flush=True)
    run_round(0, labels, workloads)  # cold, discarded (driver and file caches per exe)

    # D-12: game runs must start GAME_WORKERS workers; editor-mode runs start 32 and are recorded only.
    spec, name, params = workloads[0]
    command_for, _, editor_mode = WORKLOADS[name]
    workers = {f"{b}/{s}": worker_count(command_for(exes[b, s], *params), mask) for b in builds for s in labels}
    meta["workers"] = workers
    if editor_mode:
        meta["editor_workers"] = workers
    print(f"WORKERS {spec}: {', '.join(f'{k}={v}' for k, v in workers.items())}")
    wrong = {k: v for k, v in workers.items() if v != GAME_WORKERS}
    if not editor_mode and wrong:
        raise RuntimeError(
            f"Game runs must start {GAME_WORKERS} worker threads, found {wrong}: check [threading]"
            " worker_pool/max_threads in misc/scene_bench_project/project.godot"
        )

    # D-03: plan the rounds from the cold round and refuse a plan over the budget.
    n_of = {spec: args.rounds or plan_rounds(cold_s[spec]) for spec, _, _ in workloads}
    print(f"\nPLAN {'workload':28} {'s/process':>9} {'class':>6} {'n':>3} {'k':>3} {'processes':>9} {'minutes':>7}")
    for spec, _, _ in workloads:
        n = n_of[spec]
        processes = n * len(sides) * len(builds)
        cls = "cheap" if cold_s[spec] < CHEAP_S else "costly"
        print(f"PLAN {spec:28} {cold_s[spec]:9.2f} {cls:>6} {n:3} {wins_needed(n):3} {processes:9}"
              f" {processes * cold_s[spec] / 60:7.1f}")
    total = budget_minutes([(cold_s[s], n_of[s]) for s, _, _ in workloads], len(sides), len(builds))
    print(f"PLAN total {total:.1f} min of measurement (budget {args.budget:g} min per compiler)", flush=True)
    meta["plan"] = {s: {"seconds": round(cold_s[s], 3), "n": n_of[s], "k": wins_needed(n_of[s])} for s in n_of}
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

    flags = clock_flags(clocks)
    for row in raw:
        mhz = clocks[row["round"]]
        row["clock_mhz"] = mhz if mhz == UNAVAILABLE else f"{mhz:.0f}"
        row["clock_flag"] = flags[row["round"]]
    flagged = [rnd for rnd, flag in flags.items() if flag == 1]
    if flagged:
        print(f"CLOCK rounds {flagged} are more than {CLOCK_TOLERANCE:.0%} off the median clock (flagged in raw.csv)")
    write_csv(out / "raw.csv", raw)
    meta["clock_mhz"] = {str(r): (m if m == UNAVAILABLE else round(m)) for r, m in clocks.items()}
    (out / "meta.json").write_text(json.dumps(meta, indent=2, default=str), encoding="utf-8")
    summary = summarize_aa(args, workloads, builds, values, n_of, meta) if mode == "aa" else \
        summarize_compare(args, workloads, builds, values, n_of, meta)
    write_csv(out / "summary.csv", summary)
    count_rows = summarize_counts(mode, workloads, builds, counts)
    if count_rows:
        write_csv(out / "counts.csv", count_rows)
    print(f"\nraw.csv, summary.csv and meta.json in {out}")


def summarize_aa(args, workloads, builds, values, n_of, meta):
    summary, floors = [], load_floors(args.floors)
    machine, date = floor_meta(meta), time.strftime("%Y-%m-%d")
    print()
    for spec, name, params in workloads:
        for metric in WORKLOADS[name][1]:
            for build in builds:  # build types are never pooled
                control = values.get((spec, metric, build, "control"), {})
                twin = values.get((spec, metric, build, "aa"), {})
                diffs = paired_diffs(control, twin)
                wins, losses = wins_losses(diffs)
                floor = floor_from_aa(diffs) if len(diffs) >= MIN_ROUNDS else None
                row = {
                    "workload": spec, "metric": metric, "build": build, "n": len(diffs),
                    "control_median": statistics.median(control.values()) if control else "",
                    "aa_median": statistics.median(twin.values()) if twin else "",
                    "median_diff": statistics.median(diffs) if diffs else "",
                    "wins": wins, "losses": losses, "floor": "" if floor is None else floor,
                }
                summary.append(row)
                if diffs:
                    print(f"{spec} {metric} {build}: control {row['control_median']:.3f}, aa {row['aa_median']:.3f},"
                          f" median diff {row['median_diff'] * 100:+.2f}%, wins {wins} losses {losses}")
                shown = "INCOMPLETE" if floor is None else f"{floor * 100:.2f}%"
                print(f"FLOOR {spec} {metric} {build}: {shown} (n={len(diffs)})")
                if floor is not None:
                    case = ":".join(params)
                    floors[floor_key(name, case, metric, build, COMPILER)] = {
                        "workload": name, "case": case, "metric": metric, "build": build, "compiler": COMPILER,
                        "floor": floor, "n": n_of[spec], "date": date, **machine,
                    }
    if args.write_floors:
        Path(args.floors).parent.mkdir(parents=True, exist_ok=True)
        Path(args.floors).write_text(json.dumps(floors, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Floors written to {args.floors}")
    return summary


def summarize_compare(args, workloads, builds, values, n_of, meta):
    floors = load_floors(args.floors)
    machine = floor_meta(meta)
    targets = set(args.target.split(","))
    summary = []
    print()

    def median(series):
        return statistics.median(series.values()) if series else float("nan")

    for spec, name, params in workloads:
        metrics = WORKLOADS[name][1]
        for metric in metrics:
            for build in builds:
                prev, cand, ctrl = (values.get((spec, metric, build, label), {})
                                    for label in ("previous", "candidate", "control"))
                diffs, prev_diffs = paired_diffs(ctrl, cand), paired_diffs(ctrl, prev)
                wins, losses = wins_losses(diffs)
                # The accept rule judges the target's time metric; memory and other workloads get the regression rule.
                is_target = spec in targets and metric == metrics[0]
                floor = lookup_floor(floors, floor_key(name, ":".join(params), metric, build, COMPILER), machine,
                                     n_of[spec])
                token = verdict(diffs, None if isinstance(floor, str) else floor, is_target)
                if token == "NO-FLOOR":
                    token = floor  # NO-FLOOR, or RE-MEASURE-AA when the machine changed since the A/A run
                med = statistics.median(diffs) if diffs else float("nan")
                prev_med = statistics.median(prev_diffs) if prev_diffs else float("nan")
                shown_floor = floor if isinstance(floor, str) else f"{floor * 100:.2f}%"
                print(f"{spec} {metric} {build}: previous {median(prev):.3f}, candidate {median(cand):.3f},"
                      f" control {median(ctrl):.3f}; candidate vs control {med * 100:+.2f}%"
                      f" (wins {wins} losses {losses} of {len(diffs)}, k={wins_needed(len(diffs))});"
                      f" previous vs control {prev_med * 100:+.2f}%; floor {shown_floor};"
                      f" {'accept' if is_target else 'regression'} rule")
                print(f"VERDICT {spec} {metric} {build}: {token}", flush=True)
                summary.append({
                    "workload": spec, "metric": metric, "build": build, "n": len(diffs),
                    "previous_median": median(prev), "candidate_median": median(cand), "control_median": median(ctrl),
                    "median_diff": med, "previous_median_diff": prev_med, "wins": wins, "losses": losses,
                    "floor": "" if isinstance(floor, str) else floor, "rule": "accept" if is_target else "regression",
                    "verdict": token,
                })
    return summary


def summarize_counts(mode, workloads, builds, counts):
    """COUNTS lines: A/A determinism in aa mode, the D-06 count rule (candidate vs control) in compare mode."""
    summary = []
    for spec, name, _ in workloads:
        for metric in COUNT_METRICS.get(name, ()):
            for build in builds:
                series = {label: counts.get((spec, metric, build, label), {})
                          for label in ("control", "aa", "previous", "candidate")}
                shown = {label: f"{min(s.values())}..{max(s.values())}" if s else "n/a" for label, s in series.items()}
                if mode == "aa":
                    token = aa_counts(series["control"], series["aa"])
                    print(f"COUNTS {spec} {metric} {build}: {token} (control {shown['control']}, aa {shown['aa']})")
                else:
                    token = compare_counts(series["control"], series["candidate"])
                    print(f"COUNTS {spec} {metric} {build}: previous {shown['previous']}, candidate"
                          f" {shown['candidate']}, control {shown['control']}")
                    print(f"VERDICT {spec} {metric} {build}: {token}", flush=True)
                summary.append({"workload": spec, "metric": metric, "build": build, "verdict": token,
                                **{f"{label}_counts": shown[label] for label in series}})
    return summary


def run_check(args):
    mask = osb.p_core_mask()
    values, problems = run_checks(mask, args.max_load)
    try:
        osb.acquire_lock(OUT / ".lock").close()
        values["runner_lock"] = "free"
    except RuntimeError as error:
        problems.append(str(error))
    meta = machine_meta(mask)
    for key, value in {**values, **{k: meta[k] for k in ("cpu", "bios", "microcode", "os_build", "msvc_toolset",
                                                          "vs_version", "python")}}.items():
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
    for parsed in (check, aa, compare):
        parsed.add_argument("--max-load", type=float, default=MAX_LOAD_PCT,
                            help="refuse above this mean %% busy of the bench CPUs; floors and shares use the default")
    for series in (aa, compare):
        series.add_argument("--workloads", required=True, help="comma list, e.g. scene:instance:10000")
        series.add_argument("--builds", default="editor,template_release")
        series.add_argument("--rounds", type=int, default=None, help="n for every workload (default: planned)")
        series.add_argument("--budget", type=float, default=30.0, help="minutes of measurement allowed (D-03)")
        series.add_argument("--floors", default=str(DEFAULT_FLOORS))
        series.add_argument("--skip-build", action="store_true")
        series.add_argument("--out", default=None)
    aa.add_argument("--write-floors", action="store_true", help="merge the measured floors into --floors")
    compare.add_argument("--name", default="cand", help="candidate label: build.ps1 -Variant, binary bench.<name>")
    compare.add_argument("--defines", default="", help="space-separated macros for the candidate (cppdefines)")
    compare.add_argument("--prev", required=True, help="git ref of the previous build, e.g. origin/dev")
    compare.add_argument("--target", required=True, help="comma list of workloads judged by the accept rule")
    args = parser.parse_args()
    if osb is None:
        parser.error("No OS adapter for this platform yet")
    if args.command == "check":
        raise SystemExit(run_check(args))
    if args.rounds is not None and args.rounds < MIN_ROUNDS:
        parser.error(f"Use at least {MIN_ROUNDS} rounds: the sign test cannot pass below that")
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
    lock = osb.acquire_lock(OUT / ".lock")  # held until the process ends
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + args.command
    if args.out is None:
        args.out = str(series_dir(OUT, stamp))
    else:
        Path(args.out).mkdir(parents=True, exist_ok=True)
    try:
        run_series(args, sides)
    finally:
        lock.close()


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:  # a refusal or a failed build: the reason, not a traceback
        print(f"REFUSE: {error}", flush=True)
        raise SystemExit(1) from None
