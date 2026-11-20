/**************************************************************************/
/*  fork_alloc_counters.cpp                                               */
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

#include "fork_alloc_counters.h"

#if FORK_ALLOC_COUNTERS

#include <atomic>

// ponytail: 64 slots; more threads share slots, still correct, only slower.
static constexpr uint32_t SLOT_COUNT = 64;

struct alignas(64) ForkAllocSlot {
	std::atomic<uint64_t> allocs{ 0 };
	std::atomic<uint64_t> reallocs{ 0 };
};

// Constant-initialized, so allocations during static initialization are counted too.
static constinit ForkAllocSlot slots[SLOT_COUNT];
static constinit std::atomic<uint32_t> next_slot{ 0 };

static ForkAllocSlot &own_slot() {
	thread_local ForkAllocSlot *slot = &slots[next_slot.fetch_add(1) % SLOT_COUNT];
	return *slot;
}

void fork_alloc_counters_on_alloc() {
	own_slot().allocs.fetch_add(1, std::memory_order_relaxed);
}

void fork_alloc_counters_on_realloc() {
	own_slot().reallocs.fetch_add(1, std::memory_order_relaxed);
}

uint64_t fork_alloc_counters_allocs() {
	uint64_t total = 0;
	for (const ForkAllocSlot &slot : slots) {
		total += slot.allocs.load(std::memory_order_relaxed);
	}
	return total;
}

uint64_t fork_alloc_counters_reallocs() {
	uint64_t total = 0;
	for (const ForkAllocSlot &slot : slots) {
		total += slot.reallocs.load(std::memory_order_relaxed);
	}
	return total;
}

#endif // FORK_ALLOC_COUNTERS
