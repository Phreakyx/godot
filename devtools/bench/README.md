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
python devtools/bench/bench.py fetch
python devtools/bench/bench.py aa --workloads scene:instance:10000 --builds editor,template_release --write-floors
python devtools/bench/bench.py compare --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev \
    --target scene:churn:10000 --workloads scene:churn:10000,scene:instance:10000
python devtools/bench/test_bench.py
```

- Workloads (judged metrics, class and build types in the protocol's "Workloads"):
  `scene:<scenario>:<count>` (`workloads/scene.gd`), `script:<nodes|script|instance>`
  (`workloads/script.gd`), `gdbench` (`workloads/gdbench.gd`: the godot-benchmarks subset and
  `workloads/gdbench_dictionary.gd`, one item per case `gdbench/<file stem>.<method>`) and
  `editor_startup` (editor only, on a per-binary copy of `misc/large_world_test_project`) and
  `level` (`workloads/real_level.gd` on the Godot TPS demo: load, instancing, main-thread and
  render-thread CPU per frame along a fixed camera path; windowed on Vulkan, so close GPU-heavy
  programs first).
- `fetch` fetches the pinned third-party sources (`PINS` in `bench.py`: godot-benchmarks
  `ff8e67af1e`, MIT; tps-demo `a82f15448e`, MIT code and CC-BY 3.0 assets, 470 MB) into
  `devtools/out/bench/src/` and refuses a checkout whose HEAD is not the pin. A `gdbench` or
  `level` series fetches what it needs too.
- A `level` series first imports the fetched demo with the control editor bench build and exports
  `devtools/out/bench/<series>/tps-demo.pck` with `override.cfg` next to it (about 570 MB per
  series; delete old series directories). Every binary runs that `.pck` with `--main-pack`.
- `--skip-build` reuses the binaries in `bin/` (and the previous build's worktree in
  `devtools/out/bench/wt/<sha7>/`); `--rounds` fixes n; `--budget` is the minutes of measurement
  allowed (default 30); `--floors` is the floors file (default `devtools/bench/floors.json`).
- Windows only for now; the Linux adapter comes with the Linux session.
