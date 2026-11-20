---
type: Mistake
title: A version name change didn't reach the exe after an incremental build
description: After GODOT_VERSION_NAME changed in methods.py, SCons regenerated core/version_generated.gen.h but did not recompile the files that reach it through core/version.h, so the editor kept printing the old name until those objects were deleted.
tags: [mistake, build, scons, versioning]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-20T00:00:00Z }
---

# What happened

- The engine name (`GODOT_VERSION_NAME`, set in `methods.get_version_info()`)
  was changed and the editor built incrementally with `build.ps1`.
- SCons regenerated `core/version_generated.gen.h` and recompiled the few
  files that include it directly (`os.cpp`, `ustring.cpp`,
  `os_windows.cpp`). The files that reach it through `core/version.h`
  (`main.cpp`, `editor_node.cpp` and others) were not recompiled.
- The smoke passed, but none of its logs showed the new name. Deleting the
  14 editor objects whose sources use `GODOT_VERSION_NAME` and building
  again fixed it. The unit-test build (own `t_` objects) kept the old name
  until its objects were rebuilt the same way.

# Rule

- After a change to the version info in `methods.py`, delete the objects
  of the files that use `core/version.h` (or build clean), for the editor
  build and the tests build alike.
- Check the result in the output, not the build log: the startup header
  of a smoke log must show the new name.
- The procedure note is in [CUSTOM_BUILD.md](../../CUSTOM_BUILD.md)
  ("Build (Windows)").
