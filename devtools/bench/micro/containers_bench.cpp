/**************************************************************************/
/*  containers_bench.cpp                                                  */
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

// Standalone micro-benchmark of the real Godot containers, built and run by
// devtools/bench/bench.py micro, one binary per side. It links the real memory.cpp,
// fork_alloc_counters.cpp, hashfuncs.cpp and the String methods it needs, copied
// verbatim by the runner; only diagnostics are stubbed (any diagnostic fails the run).
//
// Cases are <container>_<key>_<op>_<N>. Keys: i64 (shuffled 64-bit integers), ptr
// (addresses into one arena) and str (16 ASCII decimal digits, compared with String ==).
// Every case prints ns_per_op, operations, live_bytes (Godot bytes held by one container
// of the case at its end, read untimed), allocs (Godot allocations inside the timed
// region) and an order-independent checksum. With BENCH_CONTROLS, ctl_<lib>_* rows run
// third-party maps fetched by the runner, compiled the same way into every side.

#ifdef BENCH_CONTROLS
#include <ankerl/unordered_dense.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_node_map.hpp>
#include <emhash/hash_table8.hpp>
#include <gtl/btree.hpp>
#include <gtl/phmap.hpp>
#endif

#include "core/os/fork_alloc_counters.h"
#include "core/string/ustring.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "core/templates/rb_map.h"
#include "core/templates/rb_set.h"
#include "core/templates/vector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

// These specializations are normally explicitly instantiated by a_hash_map.cpp.
template class AHashMap<int, int>;
template class AHashMap<String, int>;

// Any diagnostic means the benchmark is invalid. No engine/OS initialization is
// needed for these workloads, and no warning/error is silently swallowed.
static void bench_fail(const char *p_message) {
	std::fprintf(stderr, "BENCH_FAIL: %s\n", p_message);
	std::exit(2);
}
static void check(bool p_condition) {
	if (!p_condition) {
		bench_fail("correctness check");
	}
}
bool is_print_verbose_enabled() {
	return false;
}
void _err_flush_stdout() {
	std::fflush(stdout);
}
void _err_print_error(const char *, const char *, int, const char *p_error, bool, ErrorHandlerType) {
	bench_fail(p_error);
}
void _err_print_error(const char *, const char *, int, const char *p_error, const char *, bool, ErrorHandlerType) {
	bench_fail(p_error);
}
void _err_print_error(const char *, const char *, int, const String &, bool, ErrorHandlerType) {
	bench_fail("String diagnostic");
}
void _err_print_error(const char *, const char *, int, const String &, const char *, bool, ErrorHandlerType) {
	bench_fail("String diagnostic");
}
void _err_print_error(const char *, const char *, int, const char *, const String &, bool, ErrorHandlerType) {
	bench_fail("String diagnostic");
}
void _err_print_error(const char *, const char *, int, const String &, const String &, bool, ErrorHandlerType) {
	bench_fail("String diagnostic");
}
void _err_print_index_error(const char *, const char *, int, int64_t, int64_t, const char *, const char *, const char *, bool, bool) {
	bench_fail("index diagnostic");
}
void _err_print_index_error(const char *, const char *, int, int64_t, int64_t, const char *, const char *, const String &, bool, bool) {
	bench_fail("index diagnostic");
}

namespace {
using Clock = std::chrono::steady_clock;
volatile uint64_t sink = 0;

// Element operations per case (about 1-10 ms): see passes(), builds() and touched().
// N = 0 runs this many lookups, iterations or calls on an empty container.
constexpr uint64_t TARGET = 1 << 17;
constexpr int SIZES[] = { 0, 1, 16, 256, 4096, 32768, 1048576 };
constexpr int CONTROL_SIZES[] = { 256, 4096, 32768, 1048576 };
constexpr int MAX_N = 1048576;
constexpr int COPY_MAX_N = 32768; // copy at 1M is left out (one process under 15 s)
constexpr int RB_STR_MAX_N = 32768; // so are RBMap and RBSet with str keys at 1M
constexpr int MISS_MIN = 1024; // miss keys for N = 0

enum Op : uint32_t {
	OP_INSERT = 1 << 0,
	OP_HIT = 1 << 1,
	OP_MISS = 1 << 2,
	OP_ITERATE = 1 << 3,
	OP_COPY = 1 << 4,
	OP_CHURN = 1 << 5,
	OP_ERASE = 1 << 6,
};
constexpr uint32_t OPS_ALL = 0x7f;
constexpr uint32_t OPS_CONTROL = OP_INSERT | OP_HIT | OP_MISS | OP_ITERATE;

struct Sample {
	std::string name;
	uint64_t operations;
	uint64_t checksum = 0;
	uint64_t live_bytes = 0;
	uint64_t allocs = UINT64_MAX; // set: a count taken untimed, printed instead of the timed region's
	uint64_t allocs_start;
	Clock::time_point start;
	Sample(std::string p_name, uint64_t p_operations) :
			name(std::move(p_name)), operations(p_operations), allocs_start(fork_alloc_counters_allocs()), start(Clock::now()) {}
	void finish() {
		std::atomic_signal_fence(std::memory_order_seq_cst);
		const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
		const uint64_t timed_allocs = fork_alloc_counters_allocs() - allocs_start;
		check(ns > 0.0 && operations > 0);
		sink = checksum;
		std::printf("%s,%.6f,%llu,%llu,%llu,%llu\n", name.c_str(), ns / operations, static_cast<unsigned long long>(operations),
				static_cast<unsigned long long>(live_bytes), static_cast<unsigned long long>(allocs == UINT64_MAX ? timed_allocs : allocs),
				static_cast<unsigned long long>(checksum));
	}
};

std::string case_name(const char *p_container, const char *p_key, const char *p_op, int p_n) {
	char buffer[128];
	std::snprintf(buffer, sizeof(buffer), "%s_%s_%s_%d", p_container, p_key, p_op, p_n);
	return buffer;
}

uint64_t mem() {
	return Memory::get_mem_usage();
}

std::vector<int> shuffled(int p_size) {
	std::vector<int> result(p_size);
	std::iota(result.begin(), result.end(), 0);
	uint32_t state = 0x7137;
	for (int i = p_size - 1; i > 0; i--) {
		state = state * 1664525U + 1013904223U;
		std::swap(result[i], result[state % (i + 1)]);
	}
	return result;
}

// The query order of each N (a permutation of 0..N-1), shared by every container.
const std::vector<int> &query_order(int p_n) {
	static std::map<int, std::vector<int>> orders;
	auto found = orders.find(p_n);
	if (found == orders.end()) {
		found = orders.emplace(p_n, shuffled(p_n)).first;
	}
	return found->second;
}

uint64_t key_value(uint64_t p_index) {
	return p_index * 0x9E3779B97F4A7C15ULL; // odd multiplier: distinct for distinct indices
}

// Inserted keys (hit) and keys never inserted (miss); a case of size N uses the first N.
struct I64Keys {
	static constexpr const char *NAME = "i64";
	using K = int64_t;
	std::vector<K> hit, miss;
	explicit I64Keys(int p_count) {
		for (int i = 0; i < p_count; i++) {
			hit.push_back(int64_t(key_value(2 * uint64_t(i))));
			miss.push_back(int64_t(key_value(2 * uint64_t(i) + 1)));
		}
	}
	uint64_t fold(K p_key) const { return uint64_t(p_key); }
};

struct PtrKeys {
	static constexpr const char *NAME = "ptr";
	using K = const uint64_t *;
	std::vector<uint64_t> arena; // not Godot memory, so not in live_bytes
	std::vector<K> hit, miss;
	explicit PtrKeys(int p_count) :
			arena(2 * size_t(p_count)) {
		const std::vector<int> order = shuffled(p_count);
		for (int i = 0; i < p_count; i++) {
			hit.push_back(&arena[2 * size_t(order[i])]);
			miss.push_back(&arena[2 * size_t(order[i]) + 1]);
		}
	}
	uint64_t fold(K p_key) const { return uint64_t(p_key - arena.data()); } // addresses move between runs
};

String decimal16(uint64_t p_value) {
	String result;
	check(result.resize_uninitialized(17) == OK);
	char32_t *dst = result.ptrw();
	for (int i = 15; i >= 0; i--) {
		dst[i] = U'0' + char32_t(p_value % 10);
		p_value /= 10;
	}
	dst[16] = 0;
	return result;
}

struct StrKeys {
	static constexpr const char *NAME = "str";
	using K = String;
	std::vector<K> hit, miss;
	explicit StrKeys(int p_count) {
		// Below 2^50 < 10^16, and the odd multiplier is a bijection modulo 2^50: distinct keys.
		for (int i = 0; i < p_count; i++) {
			hit.push_back(decimal16(key_value(2 * uint64_t(i)) & ((1ULL << 50) - 1)));
			miss.push_back(decimal16(key_value(2 * uint64_t(i) + 1) & ((1ULL << 50) - 1)));
		}
	}
	uint64_t fold(const K &p_key) const { return uint64_t(p_key.ptr()[15]); }
};

// One interface over Godot's maps and sets and the std-like control maps.
template <bool SET, typename C, typename K>
void put(C &p_c, const K &p_key, int p_value) {
	if constexpr (SET) {
		p_c.insert(p_key);
	} else if constexpr (requires { p_c.insert(p_key, p_value); }) {
		p_c.insert(p_key, p_value);
	} else {
		p_c.emplace(p_key, p_value);
	}
}

template <bool SET, typename C, typename K>
int64_t find(const C &p_c, const K &p_key) {
	if constexpr (SET) {
		return p_c.has(p_key);
	} else if constexpr (requires { p_c.getptr(p_key); }) {
		const auto *value = p_c.getptr(p_key);
		return value ? int64_t(*value) : -1;
	} else if constexpr (requires { p_c.find(p_key)->value(); }) {
		const auto *element = p_c.find(p_key);
		return element ? element->value() : -1;
	} else {
		const auto found = p_c.find(p_key);
		return found == p_c.end() ? -1 : found->second;
	}
}

template <typename C, typename K>
bool has(const C &p_c, const K &p_key) {
	if constexpr (requires { p_c.has(p_key); }) {
		return p_c.has(p_key);
	} else {
		return p_c.find(p_key) != p_c.end();
	}
}

template <bool SET, typename C, typename Keys>
uint64_t sum(const C &p_c, const Keys &p_keys) {
	uint64_t total = 0;
	for (const auto &entry : p_c) {
		if constexpr (SET) {
			total += p_keys.fold(entry);
		} else if constexpr (requires { entry.value; }) {
			total += uint64_t(entry.value);
		} else {
			total += uint64_t(entry.second);
		}
	}
	return total;
}

// Passes so that a case runs about TARGET element operations (N = 0: TARGET calls).
uint64_t passes(int p_n) {
	return p_n ? std::max<uint64_t>(1, TARGET / p_n) : TARGET;
}

// Containers built per case: fewer for N from 1 to 15, where building one costs far more than an element operation.
uint64_t builds(int p_n) {
	return p_n ? std::max<uint64_t>(1, TARGET / std::max(p_n, 16)) : TARGET;
}

// Lookups and erases touch at most TARGET keys of a larger container (a random subset, in query order).
uint64_t touched(int p_n) {
	return std::min<uint64_t>(p_n, TARGET);
}

// Every op of one map or set at size N, on one container built by the insert case.
template <typename C, bool SET, typename Keys>
void bench_map_n(const char *p_container, const Keys &p_keys, int p_n, uint32_t p_ops) {
	const char *key = Keys::NAME;
	const std::vector<int> &order = query_order(p_n);
	const uint64_t mask = p_n ? uint64_t(p_n - 1) : 0; // every N is 0 or a power of two
	const uint64_t base = mem();
	const uint64_t n = p_n;
	const uint64_t m = touched(p_n);
	auto value_of = [](int p_index) { return SET ? uint64_t(1) : uint64_t(p_index); };
	C *c = nullptr;
	{
		const uint64_t repeats = builds(p_n);
		Sample sample(case_name(p_container, key, "insert", p_n), p_n ? repeats * n : repeats);
		for (uint64_t r = 0; r < repeats; r++) {
			C *filled = new C;
			for (int i = 0; i < p_n; i++) {
				put<SET>(*filled, p_keys.hit[i], i);
			}
			sample.checksum += filled->size();
			if (r + 1 < repeats) {
				delete filled; // the last one stays for the other cases
			} else {
				c = filled;
			}
		}
		sample.live_bytes = mem() - base;
		if (p_ops & OP_INSERT) {
			sample.finish();
		}
		check(sample.checksum == repeats * n);
	}
	if ((p_ops & OP_HIT) && p_n) {
		const uint64_t count = passes(p_n);
		uint64_t expected = 0;
		for (uint64_t pass = 0; pass < count; pass++) {
			for (uint64_t i = 0; i < m; i++) {
				expected += value_of(order[(i + pass * 97) & mask]);
			}
		}
		Sample sample(case_name(p_container, key, "hit", p_n), count * m);
		sample.live_bytes = mem() - base;
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			for (uint64_t i = 0; i < m; i++) {
				sample.checksum += uint64_t(find<SET>(*c, p_keys.hit[order[(i + pass * 97) & mask]]));
			}
		}
		sample.finish();
		check(sample.checksum == expected);
	}
	if (p_ops & OP_MISS) {
		const uint64_t count = passes(p_n);
		const uint64_t lookups = p_n ? m : 1;
		Sample sample(case_name(p_container, key, "miss", p_n), count * lookups);
		sample.live_bytes = mem() - base;
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			for (uint64_t i = 0; i < lookups; i++) {
				const uint64_t index = p_n ? uint64_t(order[(i + pass * 97) & mask]) : pass % MISS_MIN;
				sample.checksum += has(*c, p_keys.miss[index]);
			}
		}
		sample.finish();
		check(sample.checksum == 0);
	}
	if (p_ops & OP_ITERATE) {
		uint64_t expected = 0;
		for (int i = 0; i < p_n; i++) {
			expected += SET ? p_keys.fold(p_keys.hit[i]) : uint64_t(i);
		}
		const uint64_t count = passes(p_n);
		Sample sample(case_name(p_container, key, "iterate", p_n), p_n ? count * n : count);
		sample.live_bytes = mem() - base;
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			sample.checksum += sum<SET>(*c, p_keys);
		}
		sample.finish();
		check(sample.checksum == count * expected);
	}
	if ((p_ops & OP_COPY) && p_n <= COPY_MAX_N) {
		const uint64_t count = builds(p_n);
		Sample sample(case_name(p_container, key, "copy", p_n), p_n ? count * n : count);
		for (uint64_t r = 0; r < count; r++) {
			const uint64_t before = mem();
			C copy(*c);
			if (r == 0) {
				sample.live_bytes = mem() - before; // one copy, without its source
			}
			sample.checksum += copy.size();
		}
		sample.finish();
		check(sample.checksum == count * n);
	}
	if ((p_ops & OP_CHURN) && p_n) {
		// Even passes erase inserted keys and insert miss keys in their place, odd passes swap back.
		const uint64_t churned = std::min<uint64_t>(n, TARGET / 4);
		const uint64_t count = std::max<uint64_t>(2, TARGET / (2 * churned)) & ~uint64_t(1);
		Sample sample(case_name(p_container, key, "churn", p_n), count * churned * 2);
		for (uint64_t pass = 0; pass < count; pass++) {
			const auto &from = pass % 2 ? p_keys.miss : p_keys.hit;
			const auto &to = pass % 2 ? p_keys.hit : p_keys.miss;
			for (uint64_t i = 0; i < churned; i++) {
				const int index = order[i];
				check(c->erase(from[index]));
				put<SET>(*c, to[index], index);
			}
			sample.checksum += c->size();
		}
		sample.live_bytes = mem() - base;
		sample.finish();
		check(sample.checksum == count * n && find<SET>(*c, p_keys.hit[order[0]]) == int64_t(value_of(order[0])));
	}
	if (p_ops & OP_ERASE) {
		// Small N erases from copies made untimed, so each case runs about TARGET erases.
		const uint64_t count = p_n ? builds(p_n) : TARGET;
		std::vector<C *> targets;
		const uint64_t before = mem();
		if (p_n && count > 1) {
			for (uint64_t r = 0; r < count; r++) {
				targets.push_back(new C(*c));
			}
		} else {
			targets.push_back(c);
		}
		Sample sample(case_name(p_container, key, "erase", p_n), p_n ? targets.size() * m : count);
		if (p_n) {
			for (C *target : targets) {
				for (uint64_t i = 0; i < m; i++) {
					sample.checksum += target->erase(p_keys.hit[order[i]]) ? 1 : 0;
				}
			}
		} else {
			for (uint64_t i = 0; i < count; i++) {
				sample.checksum += c->erase(p_keys.miss[i % MISS_MIN]) ? 1 : 0;
			}
		}
		// Bytes one container holds after its erases.
		sample.live_bytes = (mem() - (targets[0] == c ? base : before)) / targets.size();
		sample.finish();
		check(sample.checksum == (p_n ? targets.size() * m : 0));
		for (C *target : targets) {
			if (target != c) {
				delete target;
			}
		}
	}
	delete c;
	check(mem() == base);
}

template <typename C, bool SET, typename Keys, size_t S>
void bench_map(const char *p_container, const Keys &p_keys, const int (&p_sizes)[S], uint32_t p_ops = OPS_ALL, int p_max_n = MAX_N) {
	for (int n : p_sizes) {
		if (n <= p_max_n) {
			bench_map_n<C, SET>(p_container, p_keys, n, p_ops);
		}
	}
}

// Tiny maps (D-05, the DependencyTracker pattern: HashMap<DependencyTracker *, uint32_t> and
// HashSet<Dependency *> in servers/rendering/storage/utilities.h): TINY_MAPS maps of N pointer keys.
// build creates, fills and destroys them (ns per inserted key); hit looks keys up in the live maps.
// Both rows report live_bytes and allocs of all TINY_MAPS maps alive, from an untimed build pass
// (requested bytes: per-block allocator overhead is not in live_bytes). The map objects themselves
// are not Godot memory.
constexpr int TINY_MAPS = 10000;
constexpr int TINY_SIZES[] = { 1, 2, 3, 8 };

template <typename C, bool SET>
void bench_tiny_n(const char *p_container, const PtrKeys &p_keys, int p_n) {
	const uint64_t base = mem();
	const uint64_t keys = uint64_t(TINY_MAPS) * p_n;
	auto fill = [&](C *p_maps) {
		for (int m = 0; m < TINY_MAPS; m++) {
			for (int i = 0; i < p_n; i++) {
				put<SET>(p_maps[m], p_keys.hit[m * p_n + i], i);
			}
		}
	};
	uint64_t live_bytes = 0;
	uint64_t allocs = 0;
	{
		const uint64_t start = fork_alloc_counters_allocs();
		C *maps = new C[TINY_MAPS];
		fill(maps);
		allocs = fork_alloc_counters_allocs() - start;
		live_bytes = mem() - base;
		delete[] maps;
	}
	{
		const uint64_t repeats = std::max<uint64_t>(1, TARGET / keys);
		Sample sample(case_name(p_container, "ptr", "build", p_n), repeats * keys);
		for (uint64_t r = 0; r < repeats; r++) {
			C *maps = new C[TINY_MAPS];
			fill(maps);
			for (int m = 0; m < TINY_MAPS; m++) {
				sample.checksum += maps[m].size();
			}
			delete[] maps;
		}
		sample.live_bytes = live_bytes;
		sample.allocs = allocs;
		sample.finish();
		check(sample.checksum == repeats * keys);
	}
	{
		C *maps = new C[TINY_MAPS];
		fill(maps);
		const uint64_t count = std::max<uint64_t>(1, TARGET / keys);
		uint64_t expected = 0;
		for (int i = 0; i < p_n; i++) {
			expected += SET ? 1 : uint64_t(i);
		}
		Sample sample(case_name(p_container, "ptr", "hit", p_n), count * keys);
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			for (int m = 0; m < TINY_MAPS; m++) {
				for (int i = 0; i < p_n; i++) {
					sample.checksum += uint64_t(find<SET>(maps[m], p_keys.hit[m * p_n + i]));
				}
			}
		}
		sample.live_bytes = live_bytes;
		sample.allocs = allocs;
		sample.finish();
		check(sample.checksum == count * TINY_MAPS * expected);
		delete[] maps;
	}
	check(mem() == base);
}

template <typename C, bool SET>
void bench_tiny(const char *p_container, const PtrKeys &p_keys) {
	for (int n : TINY_SIZES) {
		bench_tiny_n<C, SET>(p_container, p_keys, n);
	}
}

// List, Vector and LocalVector: push, iterate, copy (Vector: copy and detach) and, for List, erase by element.
template <typename C, typename Keys>
void bench_sequence_n(const char *p_container, const Keys &p_keys, int p_n) {
	constexpr bool IS_LIST = requires(C c, typename Keys::K k) { c.push_back(k)->next(); };
	constexpr bool IS_VECTOR = requires(C c) { c.write; };
	const char *key = Keys::NAME;
	const uint64_t base = mem();
	const uint64_t n = p_n;
	C *c = nullptr;
	{
		const uint64_t repeats = builds(p_n);
		Sample sample(case_name(p_container, key, "push", p_n), p_n ? repeats * n : repeats);
		for (uint64_t r = 0; r < repeats; r++) {
			C *filled = new C;
			for (int i = 0; i < p_n; i++) {
				filled->push_back(p_keys.hit[i]);
			}
			sample.checksum += filled->size();
			if (r + 1 < repeats) {
				delete filled;
			} else {
				c = filled;
			}
		}
		sample.live_bytes = mem() - base;
		sample.finish();
		check(sample.checksum == repeats * n);
	}
	{
		uint64_t expected = 0;
		for (int i = 0; i < p_n; i++) {
			expected += p_keys.fold(p_keys.hit[i]);
		}
		const uint64_t count = passes(p_n);
		Sample sample(case_name(p_container, key, "iterate", p_n), p_n ? count * n : count);
		sample.live_bytes = mem() - base;
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			for (const auto &value : *c) {
				sample.checksum += p_keys.fold(value);
			}
		}
		sample.finish();
		check(sample.checksum == count * expected);
	}
	if (p_n <= COPY_MAX_N) {
		const uint64_t count = builds(p_n);
		Sample sample(case_name(p_container, key, "copy", p_n), p_n ? count * n : count);
		for (uint64_t r = 0; r < count; r++) {
			const uint64_t before = mem();
			C copy(*c);
			if constexpr (IS_VECTOR) {
				copy.ptrw(); // detach: Vector copies share the buffer until written
			}
			if (r == 0) {
				sample.live_bytes = mem() - before;
			}
			sample.checksum += copy.size();
		}
		sample.finish();
		check(sample.checksum == count * n);
	}
	if constexpr (IS_LIST) {
		// Erase by element, in the shared query order, from copies made untimed.
		const std::vector<int> &order = query_order(p_n);
		const uint64_t m = touched(p_n);
		const uint64_t count = p_n ? builds(p_n) : TARGET;
		std::vector<C *> targets;
		std::vector<std::vector<typename C::Element *>> elements;
		const uint64_t before = mem();
		for (uint64_t r = 0; r < (p_n ? count : 1); r++) {
			C *target = (p_n && count > 1) ? new C(*c) : c;
			targets.push_back(target);
			elements.emplace_back();
			for (typename C::Element *e = target->front(); e; e = e->next()) {
				elements.back().push_back(e);
			}
		}
		Sample sample(case_name(p_container, key, "erase", p_n), p_n ? targets.size() * m : count);
		if (p_n) {
			for (size_t t = 0; t < targets.size(); t++) {
				for (uint64_t i = 0; i < m; i++) {
					sample.checksum += targets[t]->erase(elements[t][order[i]]) ? 1 : 0;
				}
			}
		} else {
			for (uint64_t i = 0; i < count; i++) {
				sample.checksum += c->erase(p_keys.miss[i % MISS_MIN]) ? 1 : 0; // erase of an absent value
			}
		}
		sample.live_bytes = (mem() - (targets[0] == c ? base : before)) / targets.size();
		sample.finish();
		check(sample.checksum == (p_n ? targets.size() * m : 0));
		for (C *target : targets) {
			if (target != c) {
				delete target;
			}
		}
	}
	delete c;
	check(mem() == base);
}

template <typename C, typename Keys>
void bench_sequence(const char *p_container, const Keys &p_keys) {
	for (int n : SIZES) {
		bench_sequence_n<C>(p_container, p_keys, n);
	}
}

void bench_string_copy(int p_length, const char *p_name) {
	const uint64_t base = mem();
	String original;
	check(original.resize_uninitialized(p_length + 1) == OK);
	std::fill_n(original.ptrw(), p_length, U'x');
	original.ptrw()[p_length] = 0;
	Sample copy(p_name, 32768);
	for (int i = 0; i < 32768; i++) {
		String value = original;
		value[0] = char32_t(U'a' + i % 26);
		copy.checksum += value.ptr()[0];
		copy.live_bytes = mem() - base;
	}
	copy.finish();
	check(original.ptr()[0] == U'x');
}

void bench_hash(size_t p_size, const char *p_name) {
	std::vector<uint8_t> data(p_size + 1);
	for (size_t i = 0; i < data.size(); i++) {
		data[i] = uint8_t(i * 37);
	}
	const uint64_t repeats = std::max<uint64_t>(4096, 16 * 1024 * 1024 / p_size);
	Sample hash(p_name, repeats);
	for (uint64_t i = 0; i < repeats; i++) {
#if BENCH_MODERN_HASH
		hash.checksum += hash_buffer(data.data() + 1, p_size, i);
#else
		hash.checksum += hash_murmur3_buffer(data.data() + 1, int(p_size), uint32_t(i));
#endif
	}
	hash.finish();
	check(hash.checksum != 0);
}

#ifdef BENCH_CONTROLS
// Controls hash Godot Strings with String::hash(); each library mixes a non-avalanching hash itself.
struct BenchStringHash {
	size_t operator()(const String &p_string) const { return p_string.hash(); }
};

template <typename CI, typename CS>
void bench_control(const char *p_lib, const I64Keys &p_i64, const StrKeys &p_str) {
	const std::string name = std::string("ctl_") + p_lib;
	bench_map<CI, false>(name.c_str(), p_i64, CONTROL_SIZES, OPS_CONTROL);
	bench_map<CS, false>(name.c_str(), p_str, CONTROL_SIZES, OPS_CONTROL);
}
#endif
} // namespace

int main() {
	check(mem() == 0);
	std::puts("case,ns_per_op,operations,live_bytes,allocs,checksum");
	const I64Keys i64(MAX_N);
	const PtrKeys ptr(MAX_N);
	const StrKeys str(MAX_N);

	bench_map<HashMap<int64_t, int>, false>("hashmap", i64, SIZES);
	bench_map<HashMap<const uint64_t *, int>, false>("hashmap", ptr, SIZES);
	bench_map<HashMap<String, int>, false>("hashmap", str, SIZES);
	bench_map<AHashMap<int64_t, int>, false>("ahashmap", i64, SIZES);
	bench_map<AHashMap<const uint64_t *, int>, false>("ahashmap", ptr, SIZES);
	bench_map<AHashMap<String, int>, false>("ahashmap", str, SIZES);
	bench_map<HashSet<int64_t>, true>("hashset", i64, SIZES);
	bench_map<HashSet<const uint64_t *>, true>("hashset", ptr, SIZES);
	bench_map<HashSet<String>, true>("hashset", str, SIZES);
	bench_map<RBMap<int64_t, int>, false>("rbmap", i64, SIZES);
	bench_map<RBMap<const uint64_t *, int>, false>("rbmap", ptr, SIZES);
	bench_map<RBMap<String, int>, false>("rbmap", str, SIZES, OPS_ALL, RB_STR_MAX_N);
	bench_map<RBSet<int64_t>, true>("rbset", i64, SIZES);
	bench_map<RBSet<const uint64_t *>, true>("rbset", ptr, SIZES);
	bench_map<RBSet<String>, true>("rbset", str, SIZES, OPS_ALL, RB_STR_MAX_N);
	bench_tiny<HashMap<const uint64_t *, uint32_t>, false>("hashmap_tiny", ptr);
	bench_tiny<HashSet<const uint64_t *>, true>("hashset_tiny", ptr);

	bench_sequence<List<int64_t>>("list", i64);
	bench_sequence<Vector<int64_t>>("vector", i64);
	bench_sequence<Vector<String>>("vector", str);
	bench_sequence<LocalVector<int64_t>>("localvector", i64);
	bench_sequence<LocalVector<String>>("localvector", str);

	bench_string_copy(16, "string_copy_detach_16");
	bench_string_copy(128, "string_copy_detach_128");
	bench_hash(8, "hash_bytes_8");
	bench_hash(32, "hash_bytes_32");
	bench_hash(128, "hash_bytes_128");
	bench_hash(256, "hash_bytes_256");
	bench_hash(4096, "hash_bytes_4096");
	bench_hash(65536, "hash_bytes_65536");

#ifdef BENCH_CONTROLS
	bench_control<ankerl::unordered_dense::map<int64_t, int>, ankerl::unordered_dense::map<String, int, BenchStringHash>>("unordered_dense", i64, str);
	bench_control<boost::unordered_flat_map<int64_t, int>, boost::unordered_flat_map<String, int, BenchStringHash>>("boost_flat", i64, str);
	bench_control<boost::unordered_node_map<int64_t, int>, boost::unordered_node_map<String, int, BenchStringHash>>("boost_node", i64, str);
	bench_control<gtl::flat_hash_map<int64_t, int>, gtl::flat_hash_map<String, int, BenchStringHash>>("gtl_flat", i64, str);
	bench_control<gtl::btree_map<int64_t, int>, gtl::btree_map<String, int>>("gtl_btree", i64, str);
	bench_control<emhash8::HashMap<int64_t, int>, emhash8::HashMap<String, int, BenchStringHash>>("emhash8", i64, str);
	bench_control<std::map<int64_t, int>, std::map<String, int>>("std_map", i64, str);
	bench_control<std::unordered_map<int64_t, int>, std::unordered_map<String, int, BenchStringHash>>("std_umap", i64, str);
#endif
	return 0;
}
