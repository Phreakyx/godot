---
type: Mistake
title: Linux peak memory of small workloads read the runner's own peak
description: The Linux runner read the 569 MB level pack into memory to hash it; Linux starts a child's ru_maxrss at the spawning process's peak, so every workload below about 604 MiB reported exactly the runner's peak, and their A/A peak-memory floors came out 0.00%.
tags: [mistake, benchmark, linux, memory]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T22:00:00Z }
sources:
  - id: bench-linux
    resource: ../../devtools/bench/bench_linux.py
    title: run_pinned and child_peak_bytes
  - id: bench-py
    resource: ../../devtools/bench/bench.py
    title: sha_file (hashed in chunks)
---

# What happened

- `bench.py aa` prepares the level first: it exports the 569 MB `.pck`
  and records its SHA-256, which it computed with
  `hashlib.sha256(pck.read_bytes())`. The whole file sat in the runner's
  memory once, so the runner's peak RSS was 603-604 MiB for the rest of
  the series.
- The Linux adapter reads a child's peak memory from `os.wait4`
  (`ru_maxrss`). Linux keeps the high-water mark of the address space a
  process had before `exec`, and Python starts children with `vfork`,
  whose address space before `exec` is the runner's. So no child could
  report less than the runner's peak.
- Every scene, script and gdbench run on both builds, and micro_engine
  on template_release, read exactly 603.39 MiB (GCC series) or
  604.39 MiB (Clang series) in every round. Their A/A floors came out
  0.00%, which would have turned any real difference into a "regression"
  once the readings were right. It was noticed when the editor and
  template_release builds showed the same peak to the KiB.

# Rule

- A memory reading that is identical across different binaries is the
  measurement, not the program. Check that peak memory differs between
  editor and template_release before writing its floors.
- Keep the runner small: hash large files in chunks
  (`hashlib.file_digest`). The Linux adapter now refuses a child peak
  that is not above the runner's own peak (`child_peak_bytes`), so this
  fails loudly instead of being recorded.
- Floors measured this way are left out of `floors.json` (`NO-FLOOR`)
  until they are measured again; never keep a floor known to be wrong.
