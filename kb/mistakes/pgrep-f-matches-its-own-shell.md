---
type: Mistake
title: pgrep -f and pkill -f matched the watching shell
description: During the Linux benchmark session a pattern given to pgrep -f or pkill -f also appeared in the command line of the shell that ran it, so a watch loop waited on itself and a pkill killed its own shell; match the real process exactly or watch the job's output file.
tags: [mistake, linux, shell, benchmark]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T22:00:00Z }
---

# What happened

- A watch loop looked for the running benchmark series with `pgrep -f`
  and a pattern that was also in the loop's own command line, so
  `pgrep` found the loop's shell: the watch started before the series
  did and followed an old log.
- A `pkill -f` on a similar pattern matched the shell it was typed in
  and killed it.

# Rule

- Match the real process, anchored: `pgrep -f '^python3 devtools/bench/bench.py aa'`,
  or wait on the job itself (`wait <pid>`, or the series' output file
  and its exit line).
- Never `pkill -f` a pattern that is also in the current command line;
  find the PID first and kill only that.
