"""Benchmark runner: same-source builds, pinned alternating rounds, paired differences and verdicts.

  python devtools/bench/bench.py aa --workloads scene:instance:10000 --builds editor,template_release

`aa` builds a control (extra_suffix bench) and an A/A twin (bench.aa) of each build type through
`build.ps1 -Bench`, runs one discarded cold round, then n rounds with the order reversed every other
round, and prints per workload, metric and build type the medians, the paired differences, the sign
test and the A/A noise floor. Outputs go to devtools/out/bench/ (gitignored).
"""

import argparse
import csv
import json
import math
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

if os.name == "nt":
    import bench_windows as osb
else:
    osb = None  # the Linux adapter comes with the Linux plan

ROOT = Path(__file__).resolve().parents[2]
MIN_ROUNDS = 5
TIMEOUT_S = 600
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


def max_gain(share, speedup):
    """Best case of a family (D-07): its share of the workload times the saved fraction of its time."""
    return share * (1 - 1 / speedup)


def skip_decision(gain, floor):
    return "skip" if gain < floor else "build"


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


WORKLOADS = {"scene": (scene_command, SCENE_METRICS)}


def parse_workload(spec):
    name, *params = spec.split(":")
    if name not in WORKLOADS:
        raise ValueError(f"Unknown workload {name!r} (known: {', '.join(WORKLOADS)})")
    return name, params


def parse_sample(result, metrics):
    """Metric -> (value, reason); value None means failed. Never turns a failure into a number."""
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
        return {m: (None, reason.strip()) for m in metrics}
    values = dict(re.findall(r"(\w+)=(\S+)", bench[0][len("BENCH: ") :]))
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


# --- the aa series ---


def print_rules(n):
    k = wins_needed(n)
    print(f"RULES n={n} k={k} (one-sided sign test, chance passes below 5%)")
    print(f"  accept (target workload): median paired difference faster than the A/A floor and wins >= k={k} of {n}")
    print(f"  regression (other workloads, time and peak memory): median paired difference slower than the floor"
          f" and losses >= k={k} of {n}")
    print("  a tie counts as a loss for the candidate; a round failed on either side is dropped;"
          f" fewer than {MIN_ROUNDS} pairs is INCOMPLETE", flush=True)


def git_commit():
    return subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def run_aa(args):
    workloads = [(spec, *parse_workload(spec)) for spec in args.workloads.split(",")]
    builds = args.builds.split(",")
    binaries = (("control", "bench", None), ("aa", "bench.aa", "aa"))  # label, suffix, build.ps1 -Variant
    power = osb.check_power_plan()
    osb.ensure_idle()
    if not args.skip_build:
        for build in builds:
            for _, _, variant in binaries:
                osb.build(build, variant)
    exes = {(build, label): osb.engine_exe(build, suffix) for build in builds for label, suffix, _ in binaries}
    missing = [str(exe.relative_to(ROOT)) for exe in exes.values() if not exe.is_file()]
    if missing:
        raise RuntimeError(f"Missing {', '.join(missing)}; run without --skip-build")
    if any(name == "scene" for _, name, _ in workloads):
        prepare_scene_project()
    mask = osb.p_core_mask()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    meta = {
        "commit": git_commit(),
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "affinity_mask": hex(mask),
        "power": power,
        "runner_args": vars(args),
        "binaries": {
            f"{build}/{label}": {"exe": exes[build, label].name, "build_args": osb.build_args(build, variant)}
            for build in builds
            for label, _, variant in binaries
        },
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print_rules(args.rounds)

    raw = []
    values = {}  # (workload, metric, build, label) -> {round: value}
    for rnd in range(args.rounds + 1):  # round 0: cold, discarded (driver and file caches per exe)
        osb.ensure_idle()
        order = [label for label, _, _ in binaries]
        if rnd > 0 and rnd % 2 == 0:  # odd rounds control first, even rounds reversed
            order.reverse()
        position = 0
        for build in builds:
            for spec, name, params in workloads:
                command_for, metrics = WORKLOADS[name]
                for label in order:
                    position += 1
                    result = osb.run_pinned(command_for(exes[build, label], *params), ROOT, mask, TIMEOUT_S)
                    if result["child_mask"] != mask:
                        raise RuntimeError(f"Child affinity {hex(result['child_mask'])}, expected {hex(mask)}")
                    sample = parse_sample(result, metrics)
                    shown = []
                    for metric, (value, reason) in sample.items():
                        raw.append({
                            "round": rnd, "position": position, "binary": label, "build": build, "workload": name,
                            "case": ":".join(params), "metric": metric, "value": "" if value is None else repr(value),
                            "failed": int(value is None), "reason": reason or "", "cpu_s": repr(result["cpu_s"]),
                        })
                        shown.append(f"{metric}={value:.3f}" if value is not None else f"{metric}=FAILED ({reason})")
                        if rnd > 0 and value is not None:
                            values.setdefault((spec, metric, build, label), {})[rnd] = value
                    tag = "cold" if rnd == 0 else f"r{rnd}"
                    print(f"[{tag}] {build:16} {spec} {label:7} {' '.join(shown)} cpu_s={result['cpu_s']:.2f}",
                          flush=True)

    with (out / "raw.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(raw[0]))
        writer.writeheader()
        writer.writerows(raw)

    summary = []
    print()
    for spec, name, _ in workloads:
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
    with (out / "summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    print(f"\nraw.csv, summary.csv and meta.json in {out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    aa = sub.add_parser("aa", help="A/A noise floor: control against a same-source twin")
    aa.add_argument("--workloads", required=True, help="comma list, e.g. scene:instance:10000")
    aa.add_argument("--builds", default="editor,template_release")
    aa.add_argument("--rounds", type=int, default=11)
    aa.add_argument("--skip-build", action="store_true")
    aa.add_argument("--out", default=None)
    args = parser.parse_args()
    if args.rounds < MIN_ROUNDS:
        parser.error(f"Use at least {MIN_ROUNDS} rounds: the sign test cannot pass below that")
    for spec in args.workloads.split(","):
        try:
            parse_workload(spec)
        except ValueError as error:
            parser.error(str(error))
    if osb is None:
        parser.error("No OS adapter for this platform yet")
    if args.out is None:
        args.out = str(ROOT / "devtools/out/bench" / (time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-aa"))
    run_aa(args)


if __name__ == "__main__":
    main()
