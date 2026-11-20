/**************************************************************************/
/*  fork_alloc_counters.h                                                 */
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

#include <cstdint>

// Fork-owned. Counts Memory::alloc_static, alloc_aligned_static and realloc_static calls
// for benchmark builds only (devtools/build/build.ps1 -Bench sets FORK_ALLOC_COUNTERS=1).
// With 0 nothing is counted and no monitor is registered. Scripts read the totals through
// the custom monitors fork/alloc_count and fork/realloc_count.
#ifndef FORK_ALLOC_COUNTERS
#define FORK_ALLOC_COUNTERS 0
#endif
static_assert(FORK_ALLOC_COUNTERS == 0 || FORK_ALLOC_COUNTERS == 1,
		"FORK_ALLOC_COUNTERS must be 0 or 1.");

void fork_alloc_counters_on_alloc();
void fork_alloc_counters_on_realloc();
uint64_t fork_alloc_counters_allocs();
uint64_t fork_alloc_counters_reallocs();
