---
type: Mistake
title: The Windows Compatibility Appraiser busied the bench CPUs and a measurement window was lost
description: On-demand CompatTelRunner.exe scans started by the Inventory and Compatibility Appraisal service (InventorySvc), not its scheduled task, held one bench core at 84-92% in back-to-back instances right after the micro A/A built and ran new binaries, so every ALLOC-02 series refused at start three times on 2026-11-22.
tags: [mistake, benchmark, windows, machine-state]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-22T07:10:00Z }
---

# What happened

- The ALLOC-02 series (see [alloc-vs-pool](/project/alloc-vs-pool.md))
  were started in an idle window the user had set aside, three times on
  2026-11-22.
- Each time the micro A/A (`bench.py micro --aa`) ran with the bench CPUs
  at about 3% busy. A few minutes in, `CompatTelRunner.exe` started and
  held one bench core at 84-92%. When an instance ended, the next one
  started, about five minutes apart, for more than 20 minutes.
- `bench.py check` read the bench CPUs at 11-21% busy, above the 10%
  limit, and every series refused at start. The windows were lost.
- The scheduled tasks under Task Scheduler > Microsoft > Windows >
  Application Experience ("Microsoft Compatibility Appraiser" and the
  others) were all **disabled**. The instances were children of the
  svchost that hosts `InventorySvc` (Inventory and Compatibility
  Appraisal service), which runs the appraiser on demand. The first
  instance started while the micro A/A was building and running its fresh
  binaries, so new executables are the likely trigger.
- The last minutes of that A/A ran with the scan on a bench core, so its
  floors were not used.
- Commands run by the agent at the moment the chain started (a
  `git diff`, a floors check) added load on top and pushed a borderline
  check over the limit.

# Rule

- Disabling the scheduled appraiser task is not enough. Before a long
  series, ask the user to stop `InventorySvc` for the session (an admin
  step), or build every binary first and wait until the scans that follow
  have ended.
- Look for `CompatTelRunner.exe` (Task Manager, Details) before a series
  and after any step that builds new binaries.
- Floors measured while it ran are not used; measure them again.
- Never raise `--max-load` to get past it; the limit is what keeps the
  numbers comparable ([protocol](/tooling/benchmark-protocol.md)).
- Run nothing else on the machine while a runner starts or measures:
  start it in the background and wait for it to finish.

# Outcome

- With `InventorySvc` stopped and disabled by the user, the fourth
  window (2026-11-22, 07:26-08:49 UTC) ran the micro A/A and all six
  ALLOC-02 series without a refusal: bench CPUs 2.3-3.5% busy at every
  start and no `CompatTelRunner.exe` afterwards. Stopping the service
  fixed it.
