---
type: Mistake
title: A relative benchmark-file path went to the project folder
description: The first editor startup series passed a relative --benchmark-file path; Godot resolved it after moving into the --path project, so no JSON appeared where the runner looked and every sample failed.
tags: [mistake, measuring, benchmark, editor, paths]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: bench-py
    resource: ../../devtools/bench/bench.py
    title: The runner (editor_command, run_series)
---

# What happened

- `bench.py aa --workloads editor_startup --out devtools/out/bench/verify-04c`
  built the `--benchmark-file` path from the relative `--out`. The engine
  changes its working directory to the `--path` project before it writes
  the file, so the JSON landed under the project copy, and the runner read
  "No such file" in every round. The plan's grep still passed, because it
  only checked that a `FLOOR` line existed (it read `INCOMPLETE`).

# Rule

- Pass absolute paths to the engine for every file it reads or writes
  outside `res://` (`--benchmark-file`, `--script` arguments, `load()`
  paths); the runner resolves `--out` once (`Path(args.out).resolve()`).
- A verify grep for a `FLOOR` line must also reject `INCOMPLETE`; read
  the progress lines of a new workload's first series, not only the
  summary.
