---
type: Finding
title: Allocator mismatch audit for the mimalloc candidate
description: Before the mimalloc candidate (FORK_BACKEND_ALLOCATOR=1) is measured, every block that crosses the Memory:: boundary was checked statically (590 allocation call-site hits, 178 Memory:: free and realloc sites, 13 allocator hook tables, the module and SDK handoffs) and dynamically (the full unit tests and the smoke test with mimalloc's MI_DEBUG=1 checks in production, template_release and dev builds); no mismatched pair was found; a double free and an uninitialized read in the text server's script iterator, heap corruption under any allocator, were found and fixed.
tags: [core, memory, allocator, mimalloc, audit, phase-3]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-21T00:00:00Z }
sources:
  - id: memory
    resource: ../../core/os/memory.cpp
    title: The allocator seam (_fork_sys_* helpers)
  - id: wrapper
    resource: ../../core/os/fork_mimalloc.c
    title: The guarded mimalloc translation unit
  - id: free
    resource: ../../thirdparty/mimalloc/src/alloc/free.c
    title: mimalloc's invalid-pointer, double-free and padding checks
  - id: types
    resource: ../../thirdparty/mimalloc/include/mimalloc/types.h
    title: What MI_DEBUG=1 turns on (MI_PADDING, MI_ENCODE_FREELIST)
  - id: options
    resource: ../../thirdparty/mimalloc/src/util/options.c
    title: How mimalloc reports (show_errors, the "mimalloc:" prefix, abort on EFAULT)
  - id: pins
    resource: ../../tests/fork/test_fork_memory.cpp
    title: "[Fork][Memory] pins of realloc to 0, realloc and aligned round trips"
---

# Why an audit

The candidate replaces only the four C runtime calls under `Memory::alloc_static`,
`realloc_static`, `free_static` and the aligned functions with `mi_malloc`,
`mi_calloc`, `mi_realloc` and `mi_free` ([mimalloc candidate](/project/mimalloc-allocator.md)).
`malloc`, `free`, `operator new` and `operator delete` stay the C runtime's. So a
block is at risk only when it crosses the boundary: a `Memory::` block freed by
the C runtime or a library, or a C runtime or library block freed or reallocated
through `Memory::`.

In `DEBUG_ENABLED` builds (editor, template_debug, dev) every `alloc_static`
result is offset by the prepad header, so a raw `free()` on it already passes a
bad pointer to the C runtime and would crash on any path the tests run. In
`template_release` there is no prepad, so `free(memalloc(n))` and
`memfree(malloc(n))` work today because both sides are the same heap; with
mimalloc both corrupt a heap. The audit is therefore about release-only paths and
boundary crossings, and its dynamic part runs a `template_release` tests build too.

# Risk classes

1. `Memory::` blocks freed or reallocated by the C runtime or a library.
2. C runtime or library blocks (`strdup`, `abi::__cxa_demangle`,
   `backtrace_symbols`, `realpath(NULL)`, `scandir`, `FormatMessage`,
   `CommandLineToArgvW`, COM task memory, ALSA, X11, Wayland, SDL, Vulkan loader,
   ICU, HarfBuzz, FreeType results) freed with `memfree`, `memdelete` or
   `Memory::free_static`.
3. Partial hook tables: a library given Godot's allocate hook but its own free
   (or the reverse).
4. Ownership handed across a module or DLL boundary: GDExtension, the C# glue,
   PhysX, Blast and Flow, Streamline/DLSS, Aftermath, the D3D12 Agility SDK.
5. Output parameters allocated in one place and freed in another.
6. Pad flag mismatches (`memnew_arr` allocates with `p_pad_align = true`,
   `memdelete` frees with `false`).
7. `realloc` semantics: `realloc(p, 0)` and `realloc` of a block from another
   allocator.

# Static audit

Commands (from the repository root, at `1bb8627c1f` plus this change; the outputs
are in the gitignored `devtools/out/audit/`):

```bash
# Class 1 and 7: every C runtime and OS allocator call site (590 lines, hits.txt)
git grep -nE '\b(std::)?(free|realloc|calloc|malloc|strdup|_strdup|wcsdup|_wcsdup|_aligned_free|_aligned_malloc|LocalFree|HeapFree|CoTaskMemFree)\s*\(' \
  -- core main scene servers drivers platform/windows platform/linuxbsd modules editor tests
# Class 2, 5 and 7: every Memory:: free and realloc (178 lines, memfree.txt); each producer read
git grep -nE '\b(memfree|memrealloc|Memory::free_static|Memory::realloc_static|Memory::free_aligned_static|Memory::realloc_aligned_static)\s*\(' -- <same dirs>
# Class 2 producers (class2.txt)
git grep -nE '\b(_?strdup|_?wcsdup|__cxa_demangle|backtrace_symbols|_dupenv_s|_getcwd|_fullpath|realpath|CommandLineToArgvW|FormatMessage[AW]?|SDL_free|XFree|XGetAtomName|getcwd|scandir|snd_device_name_get_hint|pa_xfree|wl_array)' -- <same dirs>
# Class 3: hook tables (hooks.txt), plus the thirdparty files that call Memory::
git grep -nE 'Alloc(ate)?\s*=|set_?[aA]llocator|MemoryFunctions|pfnAllocation|pfnReallocation|pfnFree|customMem|FT_Memory|hb_malloc_impl|mbedtls_platform_set_calloc_free|SDL_SetMemoryFunctions|general_context_create|alloc_mem\s*=|free_mem\s*=|\.zalloc\s*=|\.zfree\s*=|godot_nir_(malloc|realloc|free)\s*\(' -- <same dirs>
git grep -nlE '\bmem(new|delete|alloc|free|realloc)\b|Memory::' -- thirdparty
# Class 6: for every variable assigned from memnew_arr, a memfree or memdelete on it in
# the same file; for every memdelete_arr, a memalloc producer (both searches: no hit)
```

## Allocation call sites (hits.txt)

Every one of the 590 lines is in exactly one row (a script assigned each line to a
group and reported none left over). Producer is the allocator that made the block,
consumer the one that frees it.

| Row | Lines | Where | Producer | Consumer | Verdict |
| --- | --- | --- | --- | --- | --- |
| G1 | 456 | 155 files (Appendix) | - | - | Not an allocator call: a Godot method named `free` or `realloc` (`RenderingServer::free(RID)`, `RID_Owner::free`, `PagedAllocator::free`, pool and BVH `free`, the static `free()` of the string-name tables, declarations of these) |
| G2 | 12 | 9 files (Appendix) | - | - | Comment or message text |
| G3 | 27 | 22 GDScript test files (Appendix) | - | - | GDScript `Object.free()` in test scripts and their expected output |
| G4 | 23 | 3 web JS libraries (Appendix) | Emscripten `_malloc` | Emscripten `_free` | Matched; JS side of the web platform (not a required platform) |
| R01 | 6 | `core/error/error_backtrace.cpp:165,178`, `platform/linuxbsd/crash_handler_linuxbsd.cpp:318,351,362`, `platform/windows/crash_handler_windows_signal.cpp:137` | `abi::__cxa_demangle`, `backtrace_symbols` (C runtime) | `free` | Matched, not a `Memory::` block |
| R02 | 4 | `core/os/memory.cpp:54,62,76,84` | - | - | The seam itself: the control side of `_fork_sys_malloc`, `_calloc`, `_realloc`, `_free` |
| R03 | 3 | `drivers/alsa/audio_driver_alsa.cpp:310,313`, `drivers/alsamidi/midi_driver_alsamidi.cpp:111` | `snd_device_name_get_hint` (libasound, C runtime) | `free` | Matched, as libasound documents |
| R04 | 6 | `drivers/apple_embedded/apple_embedded.mm:191,197`, `godot_app_delegate_service_apple_embedded.mm:80,83`, `os_apple_embedded.mm:85,148` | `malloc`, `realloc`, `strdup` | `free` (argv copies live for the process) | Matched; iOS and visionOS, not required platforms |
| R05 | 4 | `drivers/d3d12/rendering_shader_container_d3d12.cpp:110,124,128,132` | Mesa NIR hooks: per-thread Win32 heap (`HeapAlloc`, `HeapReAlloc`) on Windows, `malloc`/`realloc` elsewhere | `HeapFree`, `free` | Matched; the hook table is all one allocator on each platform (see hook tables) |
| R06 | 1 | `drivers/gles3/storage/config.cpp:71` | `emscripten_webgl_get_supported_extensions` (Emscripten `malloc`) | `free` | Matched; web only |
| R07 | 2 | `drivers/unix/file_access_unix.cpp:244`, `modules/mono/utils/path_utils.cpp:157` | `realpath(path, nullptr)` (C runtime) | `::free` | Matched |
| R08 | 4 | `drivers/vulkan/rendering_device_driver_vulkan.cpp:667,668,683,684` | `malloc` (Swappy extension list) | `free` | Matched; Android only |
| R09 | 13 | `drivers/wasapi/audio_driver_wasapi.cpp:260,354,374,380,399,404,412,445,466,474,477,489,494` | `IMMDevice::GetId`, `IAudioClient::GetMixFormat` (COM task allocator) | `CoTaskMemFree` | Matched |
| R10 | 2 | `modules/camera/camera_linux.cpp:76,80` | `scandir` (C runtime) | `free` | Matched |
| R11 | 6 | `modules/godot_physx/blast/godot_physx_blast_probe.cpp:54,66,68`, `blast/physx_destructible_3d.cpp:59,71,73` | `_aligned_malloc` / `posix_memalign` (Blast asset and family memory) | `_aligned_free` / `free` | Matched; Blast never frees this memory itself |
| R12 | 1 | `modules/tinyexr/image_saver_tinyexr.cpp:307` | `SaveEXRImageToMemory` (tinyexr `malloc`) | `free` | Matched; tinyexr has no Godot allocator hook |
| R13 | 1 | `modules/upnp/upnp_miniupnp.cpp:142` | `miniwget` (miniupnpc `malloc`) | `free` | Matched |
| R14 | 5 | `modules/webrtc/webrtc_data_channel_js.cpp:203,209`, `modules/webxr/webxr_interface_js.cpp:194,212,236` | JS `GodotRuntime.allocString` / `_malloc` | `free` | Matched; web only |
| R15 | 7 | `platform/linuxbsd/godot_linuxbsd.cpp:107,114,133`, `platform/linuxbsd/wayland/wayland_embedder.cpp:3116,3138`, `platform/windows/console_wrapper_windows.cpp:54,63` | `malloc` | `free` | Matched, local buffers |
| R16 | 3 | `platform/windows/display_server_windows.cpp:116`, `gl_manager_windows_native.cpp:68`, `os_windows.cpp:146` | `FormatMessageW` with `FORMAT_MESSAGE_ALLOCATE_BUFFER` | `LocalFree` | Matched |
| R17 | 3 | `platform/windows/display_server_windows.cpp:768,782`, `os_windows.cpp:2510` | `IShellItem::GetDisplayName`, `SHGetKnownFolderPath` (COM task allocator) | `CoTaskMemFree` | Matched |
| R18 | 1 | `platform/windows/godot_windows.cpp:134` | `CommandLineToArgvW` | `LocalFree` | Matched |

No row is a `Memory::` block freed by the C runtime (class 1) or a C runtime
block given to `Memory::` (class 2).

## Memory:: free and realloc sites (memfree.txt)

The producer of each of the 178 sites was read:

| Group | Sites | Producer | Verdict |
| --- | --- | --- | --- |
| Containers and pools (`core/templates/*`: CowData, LocalVector, HashMap, AHashMap, HashSet, PagedAllocator, PagedArray, RID_Owner) and `core/os/memory.*` | 47 | `Memory::alloc_static`/`realloc_static` in the same class | Matched |
| Engine buffers: `core/core_bind.cpp`, `core/io/logger.cpp`, `core/os/os.cpp`, `core/math/delaunay_3d.h`, `core/object/object.cpp`, `core/crypto/crypto_core.cpp`, `core/io/stream_peer_gzip.cpp`, `drivers/gles3/*`, `drivers/vulkan/rendering_device_driver_vulkan.cpp` (device fault info), `drivers/windows/ip_windows.cpp`, `drivers/coreaudio/*`, `drivers/apple/os_log_logger.cpp`, `servers/rendering/*` (canvas command blocks, z lists, cluster elements, pipeline versions, mesh surfaces and versions), `scene/resources/mesh.cpp` and `3d/importer_mesh.cpp` (the xatlas unwrap outputs, allocated with `memalloc` in `modules/xatlas_unwrap/register_types.cpp`), `modules/text_server_adv/*` (HarfBuzz tag arrays, glyph buffers, ICU skeletons copied into `memalloc`), `modules/theora/editor/movie_writer_ogv.cpp`, `modules/websocket/websocket_multiplayer_peer.cpp`, `modules/raycast/raycast_occlusion_cull.cpp`, `modules/openxr/extensions/openxr_fb_display_refresh_rate_extension.cpp`, the ELF and PE string tables in both crash handlers and export plugins and `platform/linuxbsd/os_linuxbsd.cpp`, `platform/windows/display_server_windows.cpp`, `platform/windows/windows_terminal_logger.cpp`, `platform/linuxbsd/x11/display_server_x11.cpp` (shape points, clipboard strings, and the cursor pixels Godot puts into an `XcursorImage` and frees before `XcursorImageDestroy`), `platform/linuxbsd/wayland/wayland_embedder.cpp` (aligned control buffer) | 106 | `memalloc`, `memalloc_zeroed`, `memrealloc`, `Memory::alloc_static`, `alloc_aligned_static` | Matched |
| Objects freed with `memfree` instead of `memdelete`: `editor/editor_interface.cpp:395`, `modules/openxr/*` (4 sites), `platform/linuxbsd/wayland/wayland_thread.cpp` (3), `platform/windows/drop_target_windows.cpp:289`, `scene/main/scene_tree_fti.cpp:810`, `core/object/script_language_extension.h:1043,1046`, `tests/core/object/test_object.cpp:607` | 13 | `memnew` (`alloc_static(n, false)`) | Same allocator and pad flag; some skip the destructor (upstream behavior, not an allocator mismatch) |
| Allocator hooks: `core/extension/gdextension_interface.cpp`, `core/string/regex.cpp`, `core/io/zip_io.cpp`, `core/io/file_access_zip.cpp`, `modules/jolt_physics/jolt_globals.cpp`, `drivers/vulkan/rendering_context_driver_vulkan.cpp` | 12 | The paired allocate hook | Matched (see hook tables) |

The class 2 producers found (`class2.txt`) are all freed with the C runtime or the
library's own release call (rows R01, R03, R07, R10; `XGetAtomName` with `XFree`;
`CommandLineToArgvW` with `LocalFree`); none reaches `memfree`.

## Hook tables

| Library | Where it is set | Members | Verdict |
| --- | --- | --- | --- |
| Jolt Physics | `modules/jolt_physics/jolt_globals.cpp:95-99` | `Allocate`, `Reallocate`, `Free` -> `Memory::alloc_static`, `realloc_static`, `free_static`; `AlignedAllocate`, `AlignedFree` -> `alloc_aligned_static`, `free_aligned_static` | All Godot |
| Vulkan (`VkAllocationCallbacks`) | `drivers/vulkan/rendering_context_driver_vulkan.cpp:260-380` | `pfnAllocation`, `pfnReallocation`, `pfnFree` -> `alloc_aligned_static`, `realloc_aligned_static`, `free_aligned_static`; internal notifications only count | All Godot, and only with `VK_TRACK_DRIVER_MEMORY`; otherwise `nullptr` (all the driver's) |
| dr_libs (`dr_bridge`) | `thirdparty/dr_libs/dr_bridge.h:42-60` | alloc, realloc, free -> `memalloc`, `memrealloc`, `memfree` | All Godot |
| PCRE2 general context | `core/string/regex.cpp:47-55, 411, 415` | malloc, free -> `memalloc`, `memfree` | All Godot |
| zlib `z_stream` | `core/io/compression.cpp:102-103, 159-160, 210-211`, `core/io/stream_peer_gzip.cpp:89-90` | `zalloc`, `zfree` -> `zipio_alloc` (`memalloc_zeroed`), `zipio_free` (`memfree`) | All Godot |
| zlib `z_stream`, dynamic decompression | `core/io/compression.cpp:319-320` | `Z_NULL` | All library |
| minizip | `core/io/zip_io.cpp:187-188`, `core/io/file_access_zip.cpp:133-134` | `alloc_mem`, `free_mem` -> `memalloc(_zeroed)`, `memfree`. minizip passes them only to its zlib stream (`unzip.c:1467-1468`, `zip.c:1473-1474`); its own structures use `malloc` and `free` in pairs | All Godot for the stream, all library for the rest |
| Mesa NIR (D3D12 shader compiler) | `drivers/d3d12/rendering_shader_container_d3d12.cpp:73-132` | `godot_nir_malloc`, `realloc`, `free` -> per-thread Win32 heap on Windows, `malloc`/`realloc`/`free` elsewhere; the heap object is `memnew`/`memdelete` | All one allocator per platform |
| GDExtension | `core/extension/gdextension_interface.cpp:265-288` | `mem_alloc`, `mem_realloc`, `mem_free` and the `2` versions -> `Memory::` with the caller's pad flag | All Godot |
| mbedtls / TF-PSA-Crypto | `thirdparty/mbedtls/godot/godot_psa_config.h` | No `MBEDTLS_PLATFORM_MEMORY`: `calloc`/`free`; `mbedtls_zeroize_and_free` (`godot_mbedtls_platform.cpp`) frees with `free`; Godot's mutex and condition objects are `memnew`/`memdelete` pairs | All library |
| ENet | `thirdparty/enet/enet_godot.cpp` | No `enet_initialize_with_callbacks`: `malloc`/`free`; Godot's sockets are `memnew` and `memdelete` in `enet_socket_destroy` | All library, Godot objects paired |
| PhysX | `modules/godot_physx/godot_physx_server_3d.cpp:59, 1515` | `PxDefaultAllocator` | All library |
| Blast | `modules/godot_physx/blast/*` | Asset and family memory from the module's own aligned pair (R11); the convex hull builder of `physx_blast_authoring.cpp` uses C++ `new`/`delete` on both sides; Blast authoring objects are released with their `release()` | All one allocator per object |
| Not set (library defaults): FreeType (`FT_Memory`), HarfBuzz (`hb_malloc_impl`), ICU (`u_setMemoryFunctions`), SDL (`SDL_SetMemoryFunctions`), Brotli (`BrotliDecoderCreateInstance(nullptr, ...)`), zstd, VMA (`pAllocationCallbacks`), D3D12MA (`ALLOCATION_CALLBACKS`), xatlas, meshoptimizer, ufbx, libktx, tinyexr | - | - | All library; Godot copies their outputs |

No table mixes Godot's allocator with the library's.

## Handoffs across modules and SDKs

| Boundary | What was checked | Verdict |
| --- | --- | --- |
| GDExtension | `mem_alloc`/`realloc`/`free` (and the `2` versions) go through `Memory::`; `ScriptInstanceExtension` frees its `memnew`'d `native_info` copy with `memfree` | Consistent |
| C# (`modules/mono`) | `NativeMemory.Alloc`/`Free` pairs stay in C# (`ScriptManagerBridge.cs:1208-1450`); no `Marshal.FreeHGlobal` or `NativeMemory.Free` on Godot memory, no `memfree` on .NET memory; `runtime_interop.cpp` only mirrors the CowData layout | Consistent |
| PhysX, Blast, Flow | PhysX keeps its own allocator; Blast memory as in R11; Flow objects are `memnew`/`memdelete` pairs (`flow_context_rd.cpp`); no `memfree` in `modules/godot_physx` | Consistent |
| Streamline, DLSS, Aftermath | `drivers/streamline`, `drivers/aftermath`, `servers/rendering/renderer_rd/effects/dlss.cpp`, `main/main.cpp`: only `memnew`/`memdelete` of Godot's own wrappers; no SDK buffer is freed by Godot or the reverse | Consistent |
| D3D12 Agility SDK | No allocation callbacks passed to D3D12 or D3D12MA | All library |

## Pad flags and realloc

- Class 6: no `memnew_arr` result reaches `memfree` or `memdelete`, and no
  `memdelete_arr` takes a `memalloc` block.
- Class 7: every `memrealloc` takes a `memalloc`/`memrealloc` block or `nullptr`.
  `mi_realloc(p, 0)` returns a live zero-size block where the C runtime frees and
  returns `nullptr`; `_fork_sys_realloc` handles 0 before the call, and
  `[Fork][Memory] realloc to 0 bytes frees and returns nullptr` pins it on both
  pad paths.

## Fixes

No allocator mismatch: every pair above uses one allocator, so no `core/` or
`main/` file and no row of "Diverged core files" changed.

Reading the producers of the `memfree` sites found two bugs in one function,
both heap corruption under any allocator (upstream code, unchanged in
`upstream/master`):

| File and function | Bug | Fix |
| --- | --- | --- |
| `modules/text_server_adv/script_iterator.cpp`, `ScriptIterator::ScriptIterator()` | Double free: when one script run has more than `EMOJI_STACK_DEPTH` (32) emoji runs, the emoji stack moves from the stack array to a `memalloc` block. The block was freed at the end of the run, but `emoji_stack` and `emoji_size` were not reset, so the next script run wrote into the freed block, `memrealloc`'d it once it filled, and freed it again | Reset `emoji_stack` to the stack array and `emoji_size` to `EMOJI_STACK_DEPTH` after the free |
| Same function | Uninitialized reads: the move to the heap (emoji stack above 32 entries, bracket stack above `PAREN_STACK_DEPTH`, 128) did not copy the entries already on the stack array, so the first 32 emoji runs (or 128 brackets) were read from uninitialized heap memory. Garbage emoji run ends became script ranges past the end of the text, and shaping read the string out of bounds | `memcpy` the stack array into the new block |

`[Fork][Memory] Shaping more than 32 emoji runs before another script run`
(`tests/fork/test_fork_memory.cpp`) shapes 40 emoji runs followed by a Cyrillic
run and checks that every character is in exactly one glyph cluster. It finds
the double free every time in an `MI_DEBUG=1` build (mimalloc's message, see
below) and the missing copy only when the stale heap data happens to be out of
range: before the copy fix, the `--test-case="*Fork*"` suite crashed in this
case with `Index p_index = 88 is out of bounds (size() = 87)` in
`TextServerAdvanced::_shape_run` in 2 of 7 production control runs and 1 of 7
dev control runs; with the fix, 0 of 30 production and 0 of 30 dev control runs. With mimalloc's debug fill (`0xD0`) the garbage is negative
and clamped away, so that build does not crash without the copy.

# Dynamic runs

Builds (MSVC, `production=yes` unless dev, same source as the commit of this
audit). The audit variant passes `MI_DEBUG=1` with the candidate; only the
mimalloc translation unit reads it (`fork_mimalloc.c` keeps a `-Defines`
value), the engine has no `MI_DEBUG`:

```
devtools\build\build.ps1 -Tests                                   # control
devtools\build\build.ps1 -Tests -Variant allocdbg -Defines "FORK_BACKEND_ALLOCATOR=1 MI_DEBUG=1"
devtools\build\build.ps1 -Target template_release -Tests          # control
devtools\build\build.ps1 -Target template_release -Tests -Variant allocdbg -Defines "FORK_BACKEND_ALLOCATOR=1 MI_DEBUG=1"
devtools\build\build.ps1 -Tests -Dev                              # control
devtools\build\build.ps1 -Tests -Dev -Variant allocdbg -Defines "FORK_BACKEND_ALLOCATOR=1 MI_DEBUG=1"
```

What `MI_DEBUG=1` turns on in mimalloc 3.7.0 (`types.h`, `free.c`,
`options.c`):

- **Invalid pointer:** `mi_free` looks the pointer up in the page map and
  reports `mi_free: invalid pointer` for one that is not in a mimalloc page.
- **Double free and buffer overflow:** `MI_PADDING` and
  `MI_PADDING_CHECK_BYTES` put a padding word and check bytes after every
  block; `mi_free` reports `double free detected of heap block` or `buffer
  overflow in heap block ... write after N bytes`.
- **Corrupted free lists:** `MI_ENCODE_FREELIST` encodes the free-list links,
  so a use after free that overwrites one is reported as a corrupted list.
- **Fill:** new blocks are filled with `0xD0` and freed ones with `0xDF`, so a
  read of uninitialized or freed memory sees garbage instead of old values.
- **Assertions** (`mi_assert`).
- **Reporting:** `show_errors` defaults to on with `MI_DEBUG`; messages go to
  stderr as `mimalloc: error: ...` or `mimalloc: warning: ...`, and an
  `EFAULT` error (overflow, corrupted list) aborts. Guard pages stay off
  (`MI_GUARDED` 0 in the wrapper; sampling needs an environment variable
  anyway).
- Not covered: a C runtime `free` of a mimalloc block. Only the static audit
  guards that direction.

The binaries carry the checks: the string `invalid pointer%s` is in
`bin/godot.windows.editor.x86_64.allocdbg.tests.exe` and the
template_release one (1 each) and not in the `MI_DEBUG=0` candidate
`...alloc.tests.exe` (0) or the controls (0).

The checks work: with the emoji stack reset taken out, the allocdbg editor
tests binary ran `--test --test-case="*32 emoji*"` and printed `mimalloc:
error: ... double free detected of heap block ... with size 632` twice; with
the fix back, nothing.

Full `--test` (logs `devtools/out/audit/test-<binary>.log` and
`verify-<binary>.log`):

| Build | Binary | Cases | Passed | Failed | Skipped | Assertions | `mimalloc` lines |
| --- | --- | --- | --- | --- | --- | --- | --- |
| editor, control | `godot.windows.editor.x86_64.tests` | 1569 | 1569 | 0 | 9 | 594982 | 0 |
| editor, allocdbg | `godot.windows.editor.x86_64.allocdbg.tests` | 1569 | 1569 | 0 | 9 | 594982 | 0 |
| template_release, control | `godot.windows.template_release.x86_64.tests` | 1545 | 1545 | 0 | 9 | 486685 | 0 |
| template_release, allocdbg | `godot.windows.template_release.x86_64.allocdbg.tests` | 1545 | 1545 | 0 | 9 | 486685 | 0 |
| dev, control | `godot.windows.editor.dev.x86_64.tests` | 1569 | 1569 | 0 | 9 | 594982 | 0 |
| dev, allocdbg | `godot.windows.editor.dev.x86_64.allocdbg.tests` | 1569 | 1569 | 0 | 9 | 594982 | 0 |

1569 is the earlier 1564 plus the five `[Fork][Memory]` cases; template_release
has no editor tests and no TOOLS_ENABLED case, so the emoji case is not in it.

Smoke (`devtools/test/smoke.ps1`, which now takes `-Variant`, `-Tests` and
`-Dev`; logs `devtools/out/smoke/standard_allocdbg_tests*`):

| Binary | PhysX editor | PhysX game | Path tracer, Vulkan | DDGI interior | `mimalloc` lines |
| --- | --- | --- | --- | --- | --- |
| editor, allocdbg | PASS | PASS | PASS | PASS | 0 |
| dev, allocdbg | PASS | PASS | FAIL | FAIL | 0 |
| dev, control | PASS | PASS | FAIL | FAIL | 0 |

The dev failures are the same lines on the control and the candidate, so they
are not the allocator's: both Vulkan runs print one `ERROR` loader message
(the production runs print none) for a third-party Vulkan layer
installed on the test PC that fails to load, and the path tracer run prints 121 times `Tracker can't have more than
one type of usage in the same raytracing list` (a check in
`RenderingDeviceGraph::add_raytracing_list_usage` that only dev builds
compile). The dev smoke had never run on Windows before, because
`smoke.ps1` could not take a dev binary; both are left open, not part of this
audit.

Run on 2026-11-22 on the i9-14900K (Windows 11, MSVC), source
`1bb8627c1f` plus this change.

# Appendix: files of the grouped rows

**G1, Godot methods named `free` or `realloc` (456 lines):** `core/core_string_names.h`, `core/math/bvh_public.inc`, `core/math/bvh_split.inc`, `core/math/bvh_tree.h`, `core/math/convex_hull.cpp`, `core/math/dynamic_bvh.cpp`, `core/object/message_queue.cpp`, `core/object/worker_thread_pool.cpp`, `core/os/memory.h`, `core/profiling/profiling.cpp`, `core/register_core_types.cpp`, `core/string/string_name.cpp`, `core/templates/paged_allocator.h`, `core/templates/pooled_list.h`, `core/templates/rid_owner.h`, `core/variant/variant.cpp`, `core/variant/variant_pools.cpp`, `core/variant/variant_pools.h`, `drivers/accesskit/accessibility_server_accesskit.cpp`, `drivers/d3d12/rendering_device_driver_d3d12.cpp`, `drivers/d3d12/rendering_device_driver_d3d12.h`, `drivers/gles3/rasterizer_canvas_gles3.cpp`, `drivers/gles3/rasterizer_canvas_gles3.h`, `drivers/gles3/rasterizer_scene_gles3.cpp`, `drivers/gles3/rasterizer_scene_gles3.h`, `drivers/gles3/shader_gles3.cpp`, `drivers/gles3/storage/light_storage.cpp`, `drivers/gles3/storage/material_storage.cpp`, `drivers/gles3/storage/mesh_storage.cpp`, `drivers/gles3/storage/particles_storage.cpp`, `drivers/gles3/storage/particles_storage.h`, `drivers/gles3/storage/texture_storage.cpp`, `drivers/gles3/storage/utilities.cpp`, `drivers/gles3/storage/utilities.h`, `drivers/metal/metal_allocator.cpp`, `drivers/metal/rendering_device_driver_metal.cpp`, `drivers/vulkan/rendering_device_driver_vulkan.cpp`, `editor/editor_interface.cpp`, `editor/editor_interface.h`, `editor/editor_string_names.h`, `editor/file_system/editor_paths.cpp`, `editor/file_system/editor_paths.h`, `editor/register_editor_types.cpp`, `editor/scene/3d/node_3d_editor_gizmos.cpp`, `editor/scene/3d/node_3d_editor_gizmos.h`, `modules/godot_physics_2d/godot_physics_server_2d.cpp`, `modules/godot_physics_3d/godot_physics_server_3d.cpp`, `modules/godot_physx/flow/flow_scatter_rd.cpp`, `modules/godot_physx/flow/flow_scatter_rd.h`, `modules/godot_physx/flow/physx_flow_render_effect.cpp`, `modules/godot_physx/godot_physx_server_3d.cpp`, `modules/jolt_physics/jolt_physics_server_3d.cpp`, `modules/navigation_2d/2d/godot_navigation_server_2d.cpp`, `modules/navigation_3d/3d/godot_navigation_server_3d.cpp`, `modules/openxr/extensions/openxr_composition_layer_extension.cpp`, `modules/openxr/extensions/openxr_composition_layer_extension.h`, `modules/openxr/extensions/openxr_render_model_extension.cpp`, `modules/openxr/extensions/spatial_container/openxr_spatial_container_extension.cpp`, `modules/openxr/extensions/spatial_entities/openxr_spatial_anchor.cpp`, `modules/openxr/extensions/spatial_entities/openxr_spatial_entity_extension.cpp`, `modules/openxr/openxr_api.cpp`, `modules/openxr/openxr_api.h`, `modules/openxr/openxr_api_extension.cpp`, `modules/raycast/raycast_occlusion_cull.cpp`, `modules/raycast/raycast_occlusion_cull.h`, `modules/raycast/register_types.cpp`, `modules/raycast/static_raycaster_embree.cpp`, `modules/raycast/static_raycaster_embree.h`, `modules/text_server_adv/text_server_adv.cpp`, `modules/text_server_fb/text_server_fb.cpp`, `modules/texture_streaming/texture_streaming.cpp`, `modules/texture_streaming/texture_streaming.h`, `platform/linuxbsd/wayland/wayland_embedder.cpp`, `platform/windows/native_menu_windows.cpp`, `scene/3d/node_3d.cpp`, `scene/3d/node_3d.h`, `scene/gui/rich_text_label.h`, `scene/register_scene_types.cpp`, `scene/scene_string_names.h`, `servers/navigation_2d/navigation_server_2d.cpp`, `servers/navigation_2d/navigation_server_2d.h`, `servers/navigation_3d/navigation_server_3d.cpp`, `servers/navigation_3d/navigation_server_3d.h`, `servers/physics_2d/physics_server_2d.h`, `servers/physics_3d/physics_server_3d.h`, `servers/rendering/dummy/rasterizer_canvas_dummy.h`, `servers/rendering/dummy/rasterizer_scene_dummy.cpp`, `servers/rendering/dummy/rasterizer_scene_dummy.h`, `servers/rendering/dummy/storage/light_storage.cpp`, `servers/rendering/dummy/storage/light_storage.h`, `servers/rendering/dummy/storage/material_storage.cpp`, `servers/rendering/dummy/storage/mesh_storage.cpp`, `servers/rendering/dummy/storage/texture_storage.h`, `servers/rendering/dummy/storage/utilities.cpp`, `servers/rendering/dummy/storage/utilities.h`, `servers/rendering/instance_uniforms.cpp`, `servers/rendering/instance_uniforms.h`, `servers/rendering/renderer_canvas_cull.cpp`, `servers/rendering/renderer_canvas_cull.h`, `servers/rendering/renderer_canvas_render.h`, `servers/rendering/renderer_rd/effects/bokeh_dof.cpp`, `servers/rendering/renderer_rd/effects/copy_effects.cpp`, `servers/rendering/renderer_rd/effects/fsr.cpp`, `servers/rendering/renderer_rd/effects/metal_fx.cpp`, `servers/rendering/renderer_rd/effects/metal_fx.h`, `servers/rendering/renderer_rd/effects/ss_effects.cpp`, `servers/rendering/renderer_rd/environment/fog.cpp`, `servers/rendering/renderer_rd/environment/gi.cpp`, `servers/rendering/renderer_rd/environment/gi.h`, `servers/rendering/renderer_rd/environment/sky.cpp`, `servers/rendering/renderer_rd/environment/sky.h`, `servers/rendering/renderer_rd/forward_clustered/gpu_instance_slots.h`, `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`, `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h`, `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp`, `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp`, `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.h`, `servers/rendering/renderer_rd/framebuffer_cache_rd.cpp`, `servers/rendering/renderer_rd/pipeline_deferred_rd.h`, `servers/rendering/renderer_rd/renderer_canvas_render_rd.cpp`, `servers/rendering/renderer_rd/renderer_canvas_render_rd.h`, `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`, `servers/rendering/renderer_rd/renderer_scene_render_rd.h`, `servers/rendering/renderer_rd/shader_rd.cpp`, `servers/rendering/renderer_rd/storage_rd/light_storage.cpp`, `servers/rendering/renderer_rd/storage_rd/light_storage.h`, `servers/rendering/renderer_rd/storage_rd/material_storage.cpp`, `servers/rendering/renderer_rd/storage_rd/material_storage.h`, `servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp`, `servers/rendering/renderer_rd/storage_rd/mesh_storage.h`, `servers/rendering/renderer_rd/storage_rd/particles_storage.cpp`, `servers/rendering/renderer_rd/storage_rd/particles_storage.h`, `servers/rendering/renderer_rd/storage_rd/texture_storage.cpp`, `servers/rendering/renderer_rd/storage_rd/texture_storage.h`, `servers/rendering/renderer_rd/storage_rd/utilities.cpp`, `servers/rendering/renderer_rd/storage_rd/utilities.h`, `servers/rendering/renderer_rd/uniform_set_cache_rd.cpp`, `servers/rendering/renderer_scene_cull.cpp`, `servers/rendering/renderer_scene_cull.h`, `servers/rendering/renderer_scene_render.h`, `servers/rendering/renderer_viewport.cpp`, `servers/rendering/renderer_viewport.h`, `servers/rendering/rendering_device.cpp`, `servers/rendering/rendering_device.h`, `servers/rendering/rendering_device_driver.h`, `servers/rendering/rendering_method.h`, `servers/rendering/rendering_server.h`, `servers/rendering/rendering_server_default.cpp`, `servers/rendering/storage/camera_attributes_storage.cpp`, `servers/rendering/storage/compositor_storage.cpp`, `servers/rendering/storage/environment_storage.cpp`, `servers/rendering/storage/utilities.h`, `tests/core/templates/test_rid.cpp`, `tests/servers/rendering/test_gpu_instance_slots.cpp`, `tests/test_main.cpp`

**G2, comments and messages (12 lines):** `core/object/object.cpp`, `core/templates/fork_backends.h`, `core/templates/rid_owner.h`, `drivers/wasapi/audio_driver_wasapi.cpp`, `editor/inspector/editor_preview_plugins.cpp`, `modules/gdscript/gdscript_analyzer.cpp`, `modules/godot_physx/objects/godot_physx_soft_volume_3d.cpp`, `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h`, `servers/rendering/renderer_rd/pipeline_deferred_rd.h`

**G3, GDScript test scripts (27 lines):** `modules/gdscript/tests/scripts/analyzer/errors/assign_to_read_only_property.gd`, `modules/gdscript/tests/scripts/analyzer/errors/free_self.gd`, `modules/gdscript/tests/scripts/analyzer/errors/free_self.out`, `modules/gdscript/tests/scripts/analyzer/errors/use_value_of_void_function.gd`, `modules/gdscript/tests/scripts/analyzer/errors/use_value_of_void_function.out`, `modules/gdscript/tests/scripts/analyzer/features/auto_inferred_type_dont_error.gd`, `modules/gdscript/tests/scripts/analyzer/features/auto_inferred_type_dont_error.out`, `modules/gdscript/tests/scripts/analyzer/features/preload_script_native_type.gd`, `modules/gdscript/tests/scripts/analyzer/features/untyped_override_return_compatible_type.gd`, `modules/gdscript/tests/scripts/runtime/errors/assign_freed_instance.gd`, `modules/gdscript/tests/scripts/runtime/errors/assign_to_read_only_property.gd`, `modules/gdscript/tests/scripts/runtime/errors/callable_call_after_free_object.gd`, `modules/gdscript/tests/scripts/runtime/errors/invalid_cast.gd`, `modules/gdscript/tests/scripts/runtime/errors/use_value_of_void_function.gd`, `modules/gdscript/tests/scripts/runtime/errors/use_value_of_void_function.out`, `modules/gdscript/tests/scripts/runtime/features/conversions_from_native_members.gd`, `modules/gdscript/tests/scripts/runtime/features/getter_with_freed_object.gd`, `modules/gdscript/tests/scripts/runtime/features/object_constructor.gd`, `modules/gdscript/tests/scripts/runtime/features/onready_base_before_subclass.gd`, `modules/gdscript/tests/scripts/runtime/features/stringify.gd`, `modules/gdscript/tests/scripts/runtime/features/type_casting.gd`, `modules/gdscript/tests/scripts/runtime/features/typed_argument_is_null.gd`

**G4, web JS libraries (23 lines):** `modules/webrtc/library_godot_webrtc.js`, `modules/websocket/library_godot_websocket.js`, `modules/webxr/native/library_godot_webxr.js`
