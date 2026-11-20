---
type: Mistake
title: A function returned a one-element array and the build ran the command "p"
description: Get-SconsPython returned @("python") with a bare return; PowerShell unrolled it to a string, and build.ps1 indexed the first character.
tags: [mistake, build, tooling, powershell, windows]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-20T00:00:00Z }
---

# What happened

`Get-SconsPython` in `devtools/common.ps1` returns the Python that has
SCons as an array, `@(executable, leading args...)`, and `build.ps1` takes
`$python[0]` as the executable. A bare `return $candidate` sends the array
down the pipeline, and PowerShell unrolls it. A two-element candidate
(`@("py", "-3.12")`) still reaches the caller as an array, but the first
candidate, `@("python")`, arrives as the string `"python"`, and
`"python"[0]` is the character `p`. On a PC whose `python` on PATH has
SCons, every `build.ps1` run failed with "The term 'p' is not recognized".
It stayed hidden on the PC where it was written because there only the py
launcher's Python has SCons. A code review found it; a repro with a fake
`python.cmd` first on PATH confirmed it (`exe=p` before the fix, `exe=python`
after).

Wrapping the call as well (`@(Get-SconsPython)`) once the function returns
with the unary comma is wrong too: it nests the array, and the executable
becomes `"py -3.12"`.

# Rule

A PowerShell function that returns an array returns it with the unary
comma (`return , $array`), and its callers take the result as is. Test
such a function with a one-element and a two-element result. The same
unrolling broke splatted arguments before; see
[single-flag benchmark runs](/mistakes/powershell-single-arg-splat.md).
