---
type: Mistake
title: A level driver quit right after freeing the level's audio
description: The real-level driver quit in the frame it finished measuring; the level music's playback was still listed in the AudioServer, so some runs ended with "2 resources still in use at exit", an ERROR line that fails the sample.
tags: [mistake, measuring, benchmark, audio, shutdown]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: real-level
    resource: ../../devtools/bench/workloads/real_level.gd
    title: The real-level driver (_finish)
  - id: audio-server
    resource: ../../servers/audio/audio_server.cpp
    title: AudioServer playback removal (_mix_step, _delete_stream_playback_list_node)
---

# What happened

- The first `real_level.gd` (Godot TPS demo) printed its BENCH line and
  called `quit()` at once. About two runs in three then ended with
  `WARNING: 4 ObjectDB instances were leaked at exit` and
  `ERROR: 2 resources still in use at exit`; the runner fails a sample on
  any `ERROR` line. `--verbose` named them: the level music's
  `AudioStreamOggVorbis`, its playback and packet sequences.
- Freeing the level first did not help. A freed audio player only marks
  its playback for deletion; the audio thread (also with
  `--audio-driver Dummy`) removes it after a fade-out mix step, and the
  main thread frees it later. Waiting 30 more frames (about 0.1 s) still
  failed one run in three.

# Rule

- A benchmark driver that ends a scene with audio frees the scene, then
  keeps the tree running for a fixed wall time (the driver uses 500 ms)
  before it quits, after the BENCH line, so nothing measured changes.
- Run a new workload driver several times on its own before a series,
  and read the whole output of each run: an exit-time error that comes
  and goes is a race, not a fluke.
