---
type: Mistake
title: An option list in a zsh variable reached the command as one argument
description: zsh does not split an unquoted $args on spaces, so a loop over build option lists passed scons one argument (target='editor production=yes ...') and every build was refused; loops over option lists run under bash.
tags: [mistake, linux, shell, build]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T22:00:00Z }
---

# What happened

- After a failed test build on CachyOS, the remaining builds were resumed
  by hand in the desktop terminal's zsh, with a loop that kept each
  build's options in one string and ran `scons $args`.
- zsh does not word-split unquoted parameter expansions (bash does), so
  scons got a single argument and refused all three builds at once:
  `Invalid value for enum variable 'target': 'editor production=yes
  tests=yes object_prefix=t_ extra_suffix=tests'`. The same happened to
  an option list given to `perf`.

# Rule

- Loops that keep command options in a variable run under bash
  (`bash -c '...'` or a script with a bash shebang, like the setup
  script), or use an array (`args=(...)`, `"${args[@]}"`).
- A refusal that quotes the whole option list as one value means the
  shell did not split it.
