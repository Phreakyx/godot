# Benchmark runner

`bench.py` builds same-source benchmark binaries through `devtools/build/build.ps1 -Bench`
(on Linux `scons`, see below),
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
python devtools/bench/bench.py micro --aa --write-floors
python devtools/bench/bench.py micro --name cand --defines "FORK_BACKEND_X=1" --prev origin/dev
python devtools/bench/bench.py profile --workload scene:instance:10000 --build template_release
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
  `ff8e67af1e`, MIT; tps-demo `a82f15448e`, MIT code and CC-BY 3.0 assets, 470 MB; the micro
  controls unordered_dense v5.3.3 MIT, gtl v1.3.1 Apache-2.0, emhash v1.3.0 MIT and the Boost
  1.92.0 archive BSL-1.0) into `devtools/out/bench/src/` and refuses a checkout whose HEAD is not
  the pin, or an archive whose size or sha256 is not the pin. A `gdbench` or
  `level` series fetches what it needs too.
- A `level` series first imports the fetched demo with the control editor bench build and exports
  `devtools/out/bench/<series>/tps-demo.pck` with `override.cfg` next to it (about 570 MB per
  series; delete old series directories). Every binary runs that `.pck` with `--main-pack`.
- `micro` builds `micro/containers_bench.cpp` per side with MSVC (`--aa`: control and a second
  control build; `--prev <ref>`: previous with the container headers of `<ref>`, candidate with
  `--defines`, control) and runs the case matrix in one process per sample (about 12 s): HashMap,
  AHashMap, HashSet, RBMap, RBSet with `i64`, `ptr` and `str` keys and ops `insert`, `hit`,
  `miss`, `erase`, `churn`, `iterate`, `copy`; List, Vector and LocalVector; N = 0 to 1048576;
  plus `ctl_*` rows of eight third-party maps (unordered_dense, Boost flat and node, gtl flat and
  btree, emhash8, `std::map`, `std::unordered_map`), fetched by `fetch` into
  `devtools/out/bench/src/` and never tracked. Left out to keep one process under 15 s: `copy` at
  N = 1048576 for every container, and every RBMap and RBSet `str` case at N = 1048576; `hit` and
  `churn` at N = 0 do not apply (no key to find or erase). A comparison prints
  `LAYOUT-SENTINEL` (control rows beyond their floor) and every run prints `RATIO` to the fastest
  matching control. Micro rows never accept or reject a candidate alone.
- `micro_engine` (an `aa`/`compare` workload) runs the StringName, Variant and Dictionary cases
  the standalone harness can't link: the `[Bench]` doctest cases of
  `tests/fork/test_fork_bench_variant_keys.cpp`, skipped in every normal `--test` run, run with
  `--test --test-case=*Bench* --no-skip` on the bench tests builds (`build.ps1 -Tests -Bench`,
  binary `bin/godot.windows.<target>.x86_64.bench[.<variant>].tests.exe`), n = 11, one process
  of about 12 s per sample. Each case is the item `micro_engine/<case>` (`ns_per_op` judged,
  `allocs` counted); operations and checksum must repeat in every process. Runs on editor and template_release
  (`tests=yes` builds for both).
- `--skip-build` reuses the binaries in `bin/` (and the previous build's worktree in
  `devtools/out/bench/wt/<sha7>/`); `--rounds` fixes n; `--budget` is the minutes of measurement
  allowed (default 30); `--floors` is the floors file (default `devtools/bench/floors.json`).
- Linux (CachyOS, `bench_linux.py`): the same commands with `python3`, `--compiler gcc|clang`
  (default `gcc`; part of every floor key), builds with `scons` as in CUSTOM_BUILD.md "Build
  (Linux)" plus the bench options (Clang adds `use_llvm=yes linker=lld`; binaries
  `bin/godot.linuxbsd.<target>.x86_64[.llvm].bench[.<variant>][.tests]`). `check` also reads the
  governor, EPP, boost, sched_ext, the compiler hold and `perf_event_paranoid`, and prints the
  fix command for each wrong one (it never runs sudo). `aa` and `compare` take `--counters`: one
  `perf stat` pass per binary and workload after the timed rounds, printed and written to
  `summary.csv` next to the medians, never judged. `profile` records with `perf record` and
  converts with `devtools/profile/perf_to_tsv.py`. Needs `perf` (`sudo pacman -S --needed perf`).

  ```
  python3 devtools/bench/bench.py check --compiler gcc
  python3 devtools/bench/bench.py aa --workloads all --compiler gcc --write-floors --counters
  python3 devtools/bench/bench.py aa --workloads all --compiler clang --write-floors --counters
  python3 devtools/bench/bench.py micro --aa --compiler clang --write-floors
  python3 devtools/bench/bench.py profile --workload scene:instance:10000 --compiler gcc
  ```
- `profile` samples one workload on the existing bench control exe (never builds) and writes its
  TSV, `analyze.py` summary and family shares to `devtools/out/bench/profile/<series>/`; see
  [profiling](../profile/README.md).
