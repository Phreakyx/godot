---
type: Mistake
title: A profile ran a different command than the workload it described
description: editor_startup was profiled with --quit-after 3000 while the workload is --quit; most samples were the frame delay after startup, which cut every family's startup share by about half.
tags: [benchmark, profiling, editor-startup]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T03:00:00Z }
---

# What happened

`bench.py profile` had its own command for `editor_startup`
(`--headless --editor --quit-after 3000`), while the measured workload is
`--headless --editor --quit`. The profile ran 3000 editor frames after
startup, and most of its busiest-thread samples were
`OS_Windows::add_frame_delay` and its sleep calls (platform/windows 48%,
ntdll 25%). Every family's share of "editor startup" came out about half
of what startup has: hash tables 3.0% against 7.2-8.8% with the measured
command, allocation 6.1% against 14-16%. It was found in plan 02-10 because
the summary's top directory was platform/windows, which startup has no
reason to spend half its time in.

# Rule

Profile a workload with the command its numbers come from; a profile form
may change how long the sampler runs, never what the process does. Before
using a profile's shares, read the top directories of its summary and ask
whether that is the workload's work.
