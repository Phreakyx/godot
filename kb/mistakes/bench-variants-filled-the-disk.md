---
type: Mistake
title: Benchmark build variants filled the disk
description: Four new bench tests variants (about 3 GB of objects and a 0.7 GB pdb each) took the last free space; the fourth link failed with LNK1106 and the next series could not write its log, so its result was lost.
tags: [mistake, benchmark, build, disk]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T03:00:00Z }
sources:
  - id: bench-py
    resource: ../../devtools/bench/bench.py
    title: run_checks (disk_free_gb) and the MIN_BUILD_FREE_GB refusal in run_series
---

# What happened

- Plan 02-07 built four new variants for the in-engine micro cases:
  `editor` and `template_release`, each as `bench.tests` and
  `bench.aa.tests`. Every variant has its own objects in `bin/obj/`
  (`extra_suffix` is part of the object name): about 3.0 GB for an editor
  tests variant and 2.1 GB for a template_release one, plus a 0.5-0.65 GB
  pdb in `bin/` (`debug_symbols=yes`). `bin/obj/` already held 59 GB of
  earlier variants.
- No free space was checked first. The last link stopped with `fatal error
  LNK1106: invalid file or disk full`, and the `aa` series that ran next
  measured all five rounds but died flushing its log (`OSError: [Errno 28]
  No space left on device`, exit code 120), so it had to run again.
- The SCons signature database could not be written either, so the relink
  after freeing space recompiled the whole variant (4 minutes), not just
  the link.

# Rule

- Check free space before building variants: `bench.py check` prints
  `disk_free_gb`, and a series that builds refuses below
  `MIN_BUILD_FREE_GB` (5 GB). Plan about 4 GB per new tests variant.
- Once a variant is linked and won't be rebuilt soon, its objects can go
  (`bin/obj/**/*.<target>.x86_64.<suffix>.obj`); the exe and pdb stay. The
  next build of that variant is then a full build.
- After a disk-full failure, re-run the series: its log, `raw.csv` or
  `summary.csv` may be cut short.
