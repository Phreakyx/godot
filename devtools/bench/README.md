# Benchmark runner

`bench.py` builds same-source benchmark binaries through `devtools/build/build.ps1 -Bench`,
checks the machine (power plan, core parking, idle compilers, bench-CPU load, one runner at a
time), runs every workload pinned to P-cores 1-7 with one discarded cold round and alternating
reversed rounds, and judges a candidate against its same-source control with the A/A floor and a
sign test. Outputs (`raw.csv`, `summary.csv`, `meta.json`) go to `devtools/out/bench/<series>/`.

The rules, the machine checks and what each run records are in
[the benchmark protocol](../../kb/tooling/benchmark-protocol.md). Read it before measuring.

```
python devtools/bench/bench.py check
python devtools/bench/bench.py aa --workloads scene:instance:10000 --builds editor,template_release --write-floors
python devtools/bench/bench.py compare --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev \
    --target scene:churn:10000 --workloads scene:churn:10000,scene:instance:10000
python devtools/bench/test_bench.py
```

- Workloads: `scene:<scenario>:<count>` (`devtools/bench/workloads/scene.gd`, project
  `misc/scene_bench_project`).
- `--skip-build` reuses the binaries in `bin/` (and the previous build's worktree in
  `devtools/out/bench/wt/<sha7>/`); `--rounds` fixes n; `--budget` is the minutes of measurement
  allowed (default 30); `--floors` is the floors file (default `devtools/bench/floors.json`).
- Windows only for now; the Linux adapter comes with the Linux session.
