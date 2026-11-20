/**************************************************************************/
/*  fork_backends.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// Fork-owned. Selects the backend of each core container family at compile time.
// 0 is upstream's code: the control build and the merge anchor. Other values are fork
// candidates. One source tree gives two binaries side by side:
//   devtools/build/build.ps1 -Variant <name> -Defines "FORK_BACKEND_<FAMILY>=<n>"
// which passes SCons cppdefines and extra_suffix. A family gets its macro together with
// its first candidate; a losing candidate is deleted, an accepted one becomes the default
// with upstream's code kept under the 0 branch.

// Seam self-test: proves a define reaches every translation unit and the suffixed binary.
// No container reads it.
#ifndef FORK_BACKEND_SELFTEST
#define FORK_BACKEND_SELFTEST 0
#endif
static_assert(FORK_BACKEND_SELFTEST == 0 || FORK_BACKEND_SELFTEST == 1,
		"FORK_BACKEND_SELFTEST must be 0 or 1.");
