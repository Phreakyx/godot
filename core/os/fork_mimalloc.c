/**************************************************************************/
/*  fork_mimalloc.c                                                       */
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

// Fork-owned. Compiles mimalloc (thirdparty/mimalloc) as one translation unit when
// FORK_BACKEND_ALLOCATOR is non-zero (core/templates/fork_backends.h). With 0 this file is
// empty. With 1 mimalloc is only called directly; with 3 (Linux only) it is built with
// MI_MALLOC_OVERRIDE and exports malloc, free, operator new and the rest for the process.
//
// Why a guarded file instead of a conditional SCsub entry: once linked, mimalloc's Windows
// TLS callbacks run on every thread attach and detach even if nothing allocates through it,
// so the control build must not carry the object. Reading the macro in SCons would break
// the day the default changes in fork_backends.h.

#include "core/templates/fork_backends.h"

#if FORK_BACKEND_ALLOCATOR

#if FORK_BACKEND_ALLOCATOR == 3
// alloc-override.c (included by src/static.c) defines the exported symbols; fork_backends.h
// already refused this value outside Linux GCC and Clang.
#define MI_MALLOC_OVERRIDE 1
#else
// Every other value: only core/os/memory.cpp calls mimalloc; malloc, free, operator new and
// delete stay the C runtime's.
#ifdef MI_MALLOC_OVERRIDE
#error "MI_MALLOC_OVERRIDE must not be defined: only Memory:: calls mimalloc."
#endif
#endif

// mimalloc turns on its debug checks, statistics and guarded sampling when NDEBUG is not
// defined, which is every dev_build. Pin them off so dev and production get the same
// allocator; a -Defines value (for example MI_DEBUG=1 for an audit) still wins.
#ifndef MI_DEBUG
#define MI_DEBUG 0
#endif
#ifndef MI_SECURE
#define MI_SECURE 0
#endif
#ifndef MI_STATS
#define MI_STATS 0
#endif
#ifndef MI_PROFILE
#define MI_PROFILE 0
#endif
#ifndef MI_GUARDED
#define MI_GUARDED 0
#endif

#include "thirdparty/mimalloc/src/static.c"

#endif // FORK_BACKEND_ALLOCATOR
