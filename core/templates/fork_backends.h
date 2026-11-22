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
// The header is also included from C (core/os/fork_mimalloc.c): preprocessor checks only.

// Seam self-test: proves a define reaches every translation unit and the suffixed binary.
// No container reads it.
#ifndef FORK_BACKEND_SELFTEST
#define FORK_BACKEND_SELFTEST 0
#endif
#if !(FORK_BACKEND_SELFTEST == 0 || FORK_BACKEND_SELFTEST == 1)
#error "FORK_BACKEND_SELFTEST must be 0 or 1."
#endif

// General allocator under Memory::alloc_static and friends (ALLOC-01).
// 0: the C runtime malloc, calloc, realloc and free (upstream).
// 1: mimalloc (thirdparty/mimalloc), called directly from core/os/memory.cpp. It is
//    compiled by core/os/fork_mimalloc.c, which is empty with 0. Nothing is overridden.
// 3: Linux only, exploratory, never a default: mimalloc exports malloc, calloc, realloc,
//    free, the aligned and POSIX variants and operator new/delete for the whole process,
//    third-party code included. core/os/memory.cpp keeps its C runtime calls, which then
//    reach mimalloc.
#ifndef FORK_BACKEND_ALLOCATOR
#define FORK_BACKEND_ALLOCATOR 0
#endif
#if !(FORK_BACKEND_ALLOCATOR == 0 || FORK_BACKEND_ALLOCATOR == 1 || FORK_BACKEND_ALLOCATOR == 3)
#error "FORK_BACKEND_ALLOCATOR must be 0, 1 or 3."
#endif
// Windows builds use the static C runtime (/MT), and mimalloc's Windows override needs its
// redirect DLL and the DLL runtime; macOS and MinGW are not tried either.
#if FORK_BACKEND_ALLOCATOR == 3 && !(defined(__linux__) && (defined(__GNUC__) || defined(__clang__)))
#error "FORK_BACKEND_ALLOCATOR=3 replaces malloc for the whole process and is Linux-only (GCC or Clang)."
#endif

// HashMap node storage (HASH-01).
// 0: one Allocator::new_allocation per element (upstream).
// 1: a per-map node pool inside HashMap (core/templates/hash_map.h): geometric pages that
//    never move, an intrusive free list, released by clear() and the destructor. Only maps
//    with the default Allocator use it; a map with its own Allocator keeps that path.
#ifndef FORK_BACKEND_HASHMAP_POOL
#define FORK_BACKEND_HASHMAP_POOL 0
#endif
#if !(FORK_BACKEND_HASHMAP_POOL == 0 || FORK_BACKEND_HASHMAP_POOL == 1)
#error "FORK_BACKEND_HASHMAP_POOL must be 0 or 1."
#endif

// HashMap and HashSet capacity and slot arithmetic (HASH-03, kb/project/hash-pow2-mixer.md).
// 0: prime capacities (hash_table_size_primes) and fastmod on every probed slot (upstream).
// 1, 2: power-of-two capacities from 8 and a mask; _hash() mixes the table hash once with
//    hash_table_mix() (1: multiply then xorshift, 2: hash_fmix32). Byte-hashed String and
//    CharString keys are not mixed again. Hasher outputs do not change.
#ifndef FORK_BACKEND_HASH_POW2
#define FORK_BACKEND_HASH_POW2 0
#endif
#if !(FORK_BACKEND_HASH_POW2 == 0 || FORK_BACKEND_HASH_POW2 == 1 || FORK_BACKEND_HASH_POW2 == 2)
#error "FORK_BACKEND_HASH_POW2 must be 0, 1 or 2."
#endif

// Table-internal hash of StringName and pointer keys (HASH-05, kb/project/table-key-hash.md).
// 0: HashMapHasherDefault's output (upstream): StringName's cached hash, Thomas Wang's
//    64-to-32 mix for pointers.
// 1: hash_table_key() with the default hasher hashes a StringName by its data pointer (its
//    equality is pointer identity) and a pointer with one 64-bit multiply, high half.
//    HashMapHasherDefault and every public hash keep their outputs.
#ifndef FORK_BACKEND_TABLE_KEY_HASH
#define FORK_BACKEND_TABLE_KEY_HASH 0
#endif
#if !(FORK_BACKEND_TABLE_KEY_HASH == 0 || FORK_BACKEND_TABLE_KEY_HASH == 1)
#error "FORK_BACKEND_TABLE_KEY_HASH must be 0 or 1."
#endif

// Short-key byte hash in hash_buffer64() below HASH_XXH3_THRESHOLD (HASH-04,
// kb/project/rapidhash-byte-hash.md). Only table-internal hashes use hash_buffer*.
// 0: wyhash final v4.3 (the fork's current byte hash; upstream has no byte hash).
// 1: rapidhash V3 (thirdparty/rapidhash, rapidhash_withSeed). XXH3 above the threshold either way.
#ifndef FORK_BACKEND_BYTE_HASH
#define FORK_BACKEND_BYTE_HASH 0
#endif
#if !(FORK_BACKEND_BYTE_HASH == 0 || FORK_BACKEND_BYTE_HASH == 1)
#error "FORK_BACKEND_BYTE_HASH must be 0 or 1."
#endif
