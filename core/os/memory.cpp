/**************************************************************************/
/*  memory.cpp                                                            */
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

#include "memory.h"

#include "core/os/fork_alloc_counters.h"
#include "core/profiling/profiling.h"
#include "core/templates/fork_backends.h"
#include "core/templates/safe_refcount.h"

#ifdef DEV_ENABLED
#include "core/math/math_funcs_binary.h"
#endif

#include <cstdlib>

#if FORK_BACKEND_ALLOCATOR
#include "thirdparty/mimalloc/include/mimalloc.h"
#endif

// Fork: the underlying allocator, selected by FORK_BACKEND_ALLOCATOR (fork_backends.h).
// 0 is upstream's C runtime calls. Prepad, accounting, counters and profiling stay above.
static _FORCE_INLINE_ void *_fork_sys_malloc(size_t p_bytes) {
#if FORK_BACKEND_ALLOCATOR
	return mi_malloc(p_bytes);
#else
	return malloc(p_bytes);
#endif
}

static _FORCE_INLINE_ void *_fork_sys_calloc(size_t p_bytes) {
#if FORK_BACKEND_ALLOCATOR
	return mi_calloc(1, p_bytes);
#else
	return calloc(1, p_bytes);
#endif
}

static _FORCE_INLINE_ void *_fork_sys_realloc(void *p_memory, size_t p_bytes) {
#if FORK_BACKEND_ALLOCATOR
	// mi_realloc(p, 0) returns a live zero-size block; the C runtime frees and returns
	// nullptr, which realloc_static relies on.
	if (p_bytes == 0) {
		mi_free(p_memory);
		return nullptr;
	}
	return mi_realloc(p_memory, p_bytes);
#else
	return realloc(p_memory, p_bytes);
#endif
}

static _FORCE_INLINE_ void _fork_sys_free(void *p_memory) {
#if FORK_BACKEND_ALLOCATOR
	mi_free(p_memory);
#else
	free(p_memory);
#endif
}

#ifdef DEBUG_ENABLED
static SafeNumeric<uint64_t> _current_mem_usage;
static SafeNumeric<uint64_t> _max_mem_usage;
#endif

void *Memory::alloc_aligned_static(size_t p_bytes, size_t p_alignment) {
	DEV_ASSERT(Math::is_power_of_2(p_alignment));

	void *p1, *p2;
	if ((p1 = (void *)_fork_sys_malloc(p_bytes + p_alignment - 1 + sizeof(uint32_t))) == nullptr) {
		return nullptr;
	}
#if FORK_ALLOC_COUNTERS
	fork_alloc_counters_on_alloc();
#endif
	GodotProfileAlloc(p1, p_bytes + p_alignment - 1 + sizeof(uint32_t));

	p2 = (void *)(((uintptr_t)p1 + sizeof(uint32_t) + p_alignment - 1) & ~((p_alignment)-1));
	*((uint32_t *)p2 - 1) = (uint32_t)((uintptr_t)p2 - (uintptr_t)p1);
	return p2;
}

void *Memory::realloc_aligned_static(void *p_memory, size_t p_bytes, size_t p_prev_bytes, size_t p_alignment) {
	if (p_memory == nullptr) {
		return alloc_aligned_static(p_bytes, p_alignment);
	}

	void *ret = alloc_aligned_static(p_bytes, p_alignment);
	if (ret) {
		memcpy(ret, p_memory, p_prev_bytes);
	}
	free_aligned_static(p_memory);
	return ret;
}

void Memory::free_aligned_static(void *p_memory) {
	uint32_t offset = *((uint32_t *)p_memory - 1);
	void *p = (void *)((uint8_t *)p_memory - offset);
	GodotProfileFree(p);
	_fork_sys_free(p);
}

template <bool p_ensure_zero>
void *Memory::alloc_static(size_t p_bytes, bool p_pad_align) {
#ifdef DEBUG_ENABLED
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	void *mem;
	if constexpr (p_ensure_zero) {
		mem = _fork_sys_calloc(p_bytes + (prepad ? DATA_OFFSET : 0));
	} else {
		mem = _fork_sys_malloc(p_bytes + (prepad ? DATA_OFFSET : 0));
	}

	ERR_FAIL_NULL_V(mem, nullptr);
#if FORK_ALLOC_COUNTERS
	fork_alloc_counters_on_alloc();
#endif
	GodotProfileAlloc(mem, p_bytes + (prepad ? DATA_OFFSET : 0));

	if (prepad) {
		uint8_t *s8 = (uint8_t *)mem;

		uint64_t *s = (uint64_t *)(s8 + SIZE_OFFSET);
		*s = p_bytes;

#ifdef DEBUG_ENABLED
		uint64_t new_mem_usage = _current_mem_usage.add(p_bytes);
		_max_mem_usage.exchange_if_greater(new_mem_usage);
#endif
		return s8 + DATA_OFFSET;
	} else {
		return mem;
	}
}

template void *Memory::alloc_static<true>(size_t p_bytes, bool p_pad_align);
template void *Memory::alloc_static<false>(size_t p_bytes, bool p_pad_align);

void *Memory::realloc_static(void *p_memory, size_t p_bytes, bool p_pad_align) {
	if (p_memory == nullptr) {
		return alloc_static(p_bytes, p_pad_align);
	}
#if FORK_ALLOC_COUNTERS
	if (p_bytes > 0) {
		fork_alloc_counters_on_realloc();
	}
#endif

	uint8_t *mem = (uint8_t *)p_memory;

#ifdef DEBUG_ENABLED
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	if (prepad) {
		mem -= DATA_OFFSET;
		uint64_t *s = (uint64_t *)(mem + SIZE_OFFSET);

#ifdef DEBUG_ENABLED
		if (p_bytes > *s) {
			uint64_t new_mem_usage = _current_mem_usage.add(p_bytes - *s);
			_max_mem_usage.exchange_if_greater(new_mem_usage);
		} else {
			_current_mem_usage.sub(*s - p_bytes);
		}
#endif

		if (p_bytes == 0) {
			GodotProfileFree(mem);
			_fork_sys_free(mem);
			return nullptr;
		} else {
			*s = p_bytes;

			GodotProfileFree(mem);
			mem = (uint8_t *)_fork_sys_realloc(mem, p_bytes + DATA_OFFSET);
			ERR_FAIL_NULL_V(mem, nullptr);
			GodotProfileAlloc(mem, p_bytes + DATA_OFFSET);

			s = (uint64_t *)(mem + SIZE_OFFSET);

			*s = p_bytes;

			return mem + DATA_OFFSET;
		}
	} else {
		GodotProfileFree(mem);
		mem = (uint8_t *)_fork_sys_realloc(mem, p_bytes);

		ERR_FAIL_COND_V(mem == nullptr && p_bytes > 0, nullptr);
		GodotProfileAlloc(mem, p_bytes);

		return mem;
	}
}

void Memory::free_static(void *p_ptr, bool p_pad_align) {
	ERR_FAIL_NULL(p_ptr);

	uint8_t *mem = (uint8_t *)p_ptr;

#ifdef DEBUG_ENABLED
	bool prepad = true;
#else
	bool prepad = p_pad_align;
#endif

	if (prepad) {
		mem -= DATA_OFFSET;

#ifdef DEBUG_ENABLED
		uint64_t *s = (uint64_t *)(mem + SIZE_OFFSET);
		_current_mem_usage.sub(*s);
#endif

		GodotProfileFree(mem);
		_fork_sys_free(mem);
	} else {
		GodotProfileFree(mem);
		_fork_sys_free(mem);
	}
}

uint64_t Memory::get_mem_available() {
	return -1; // 0xFFFF...
}

uint64_t Memory::get_mem_usage() {
#ifdef DEBUG_ENABLED
	return _current_mem_usage.get();
#else
	return 0;
#endif
}

uint64_t Memory::get_mem_max_usage() {
#ifdef DEBUG_ENABLED
	return _max_mem_usage.get();
#else
	return 0;
#endif
}

_GlobalNil::_GlobalNil() {
	left = this;
	right = this;
	parent = this;
}

_GlobalNil _GlobalNilClass::_nil;
