---
type: Mistake
title: Transient MSVC internal compiler errors in variant builds
description: Three variant builds stopped on an MSVC crash (C1001 or cl.exe exit 0xC0000409) in an object the candidate does not touch; the unchanged rerun built each one. Retry once before changing any code.
tags: [mistake, build, msvc, benchmark]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-23T00:00:00Z }
sources:
  - id: build-ps1
    resource: ../../devtools/build/build.ps1
    title: The -Bench -Variant builds that bench.py compare --build-only runs
---

# What happened

- 03-09: the first `small8` build stopped at `fatal error C1001: Internal
  compiler error` (compiler access violation) in
  `modules/enet/enet_multiplayer_peer.cpp`. The 2026-11 sync had one too
  ([sync note](/project/fork-sync-2026-11.md)).
- 03-10, building the hash bench variants: `pow2` stopped at C1001 in
  `servers/audio/audio_server.cpp` (compiler file `main.cpp`, line 262),
  and `small8` template_release at `cl.exe` exit code 3221226505
  (0xC0000409, a fail-fast crash of the compiler) on
  `thirdparty/basis_universal/transcoder/basisu_transcoder.cpp`.
- None of these files depends on the candidate's macro in a way that
  matters, and each object compiled cleanly on the next run with no source
  change. The variants with more macros on compiled the same code.

# Why

The compiler itself crashed under a parallel variant build: the error
moved between objects and builds and did not come back on a rerun. Reading
it as a code problem would have sent the work after a bug that is not in
the source.

# Rule

- A C1001 or a `cl.exe` exit of 0xC0000409 (3221226505) in a variant build
  gets one rerun of the same command with no source change. Keep the failed
  log (for example `build310-<n>-attempt1.log`).
- Only a crash that repeats on the same object is investigated in the code
  (simplify the construct the message points at, or report it to the
  compiler vendor).
- A rerun after a failed variant build is a normal incremental build; the
  objects that compiled are kept.
