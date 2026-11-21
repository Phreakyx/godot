/**************************************************************************/
/*  test_fork_bench_variant_keys.cpp                                      */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_fork_bench_variant_keys)

#include "core/os/fork_alloc_counters.h"
#include "core/string/string_name.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <type_traits>

// Fork-owned. Micro-benchmark cases for the keys the standalone harness
// (devtools/bench/micro/containers_bench.cpp) cannot link: StringName and Variant keys,
// Dictionary, and StringName creation. Every case is skipped by default and has no
// "Fork" in its name, so --test and the *Fork* contract runs never run one.
// devtools/bench/bench.py runs them as workload micro_engine on the bench tests builds:
// --test --test-case="*Bench*" --no-skip. Case names follow the standalone harness,
// <container>_<key>_<op>_<N>. Each prints one BENCH line: ns_per_op, operations,
// allocs (Godot allocations in the timed region; n/a without FORK_ALLOC_COUNTERS) and
// an order-independent checksum, which the case also checks.

namespace TestForkBenchVariantKeys {

using Clock = std::chrono::steady_clock;

// About TARGET element operations per case, as in the standalone harness.
constexpr uint64_t TARGET = 1 << 17;
constexpr int SIZES[] = { 0, 1, 16, 256, 4096, 32768, 1048576 };
constexpr int MAX_N = 1048576;
constexpr int COPY_MAX_N = 32768; // copy at 1M is left out (one process under 15 s)
constexpr int MISS_MIN = 1024; // miss keys for N = 0

struct Sample {
	String name;
	uint64_t operations;
	uint64_t checksum = 0;
	uint64_t allocs_start = 0;
	Clock::time_point start;

	Sample(const String &p_name, uint64_t p_operations) :
			name(p_name), operations(p_operations) {
#if FORK_ALLOC_COUNTERS
		allocs_start = fork_alloc_counters_allocs();
#endif
		start = Clock::now();
	}

	// Reads the clock and the counter first; formatting and printing are outside the timed region.
	void finish() {
		std::atomic_signal_fence(std::memory_order_seq_cst);
		const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
		char allocs[24] = "n/a";
#if FORK_ALLOC_COUNTERS
		snprintf(allocs, sizeof(allocs), "%llu", static_cast<unsigned long long>(fork_alloc_counters_allocs() - allocs_start));
#endif
		CHECK(ns > 0.0);
		CHECK(operations > 0);
		char line[256];
		snprintf(line, sizeof(line), "BENCH: workload=micro_engine case=%s ns_per_op=%.6f operations=%llu allocs=%s checksum=%llu",
				name.utf8().get_data(), ns / operations, static_cast<unsigned long long>(operations), allocs,
				static_cast<unsigned long long>(checksum));
		print_line(String(line));
	}
};

String case_name(const char *p_container, const char *p_key, const char *p_op, int p_n) {
	return vformat("%s_%s_%s_%d", p_container, p_key, p_op, p_n);
}

// A permutation of 0..N-1: the query order of every container of size N.
LocalVector<int> shuffled(int p_size) {
	LocalVector<int> result;
	result.resize(p_size);
	for (int i = 0; i < p_size; i++) {
		result[i] = i;
	}
	uint32_t state = 0x7137;
	for (int i = p_size - 1; i > 0; i--) {
		state = state * 1664525U + 1013904223U;
		SWAP(result[i], result[state % (i + 1)]);
	}
	return result;
}

uint64_t passes(int p_n) {
	return p_n ? MAX(uint64_t(1), TARGET / p_n) : TARGET;
}

// Containers built per case: fewer for N from 1 to 15, where building one costs far more than an element operation.
uint64_t builds(int p_n) {
	return p_n ? MAX(uint64_t(1), TARGET / MAX(p_n, 16)) : TARGET;
}

// Lookups and erases touch at most TARGET keys of a larger container.
uint64_t touched(int p_n) {
	return MIN(uint64_t(p_n), TARGET);
}

uint64_t key_value(uint64_t p_index) {
	return p_index * 0x9E3779B97F4A7C15ULL; // odd multiplier: distinct for distinct indices
}

// 16 ASCII decimal digits; below 2^50 < 10^16, and the multiplier is a bijection modulo 2^50.
String str_key(uint64_t p_index) {
	uint64_t value = key_value(p_index) & ((1ULL << 50) - 1);
	String result;
	if (result.resize_uninitialized(17) != OK) {
		return String(); // duplicate keys, caught by the size checks
	}
	char32_t *dst = result.ptrw();
	for (int i = 15; i >= 0; i--) {
		dst[i] = U'0' + char32_t(value % 10);
		value /= 10;
	}
	dst[16] = 0;
	return result;
}

// Inserted keys (even indices) and keys never inserted (odd indices); a case of size N uses the first N.
template <typename K>
struct Keys {
	const char *name = nullptr;
	LocalVector<K> hit;
	LocalVector<K> miss;
};

template <typename K, typename F>
Keys<K> make_keys(const char *p_name, F p_make) {
	Keys<K> keys;
	keys.name = p_name;
	keys.hit.reserve(MAX_N);
	keys.miss.reserve(MAX_N);
	for (int i = 0; i < MAX_N; i++) {
		keys.hit.push_back(p_make(2 * uint64_t(i)));
		keys.miss.push_back(p_make(2 * uint64_t(i) + 1));
	}
	return keys;
}

// One interface over HashMap, AHashMap and Dictionary (copy is duplicate(): a Dictionary copy shares).
template <typename C>
constexpr bool IS_DICTIONARY = std::is_same_v<C, Dictionary>;

template <typename C, typename K>
void put(C &p_c, const K &p_key, int p_value) {
	if constexpr (IS_DICTIONARY<C>) {
		p_c.set(p_key, p_value);
	} else {
		p_c.insert(p_key, p_value);
	}
}

template <typename C, typename K>
int64_t find(const C &p_c, const K &p_key) {
	const auto *value = p_c.getptr(p_key);
	return value ? int64_t(*value) : -1;
}

template <typename C>
C copy_of(const C &p_c) {
	if constexpr (IS_DICTIONARY<C>) {
		return p_c.duplicate();
	} else {
		return C(p_c);
	}
}

template <typename C>
uint64_t sum(const C &p_c) {
	uint64_t total = 0;
	for (const auto &entry : p_c) {
		total += uint64_t(int64_t(entry.value));
	}
	return total;
}

// insert, hit, miss, iterate, copy and erase of one container type at size N.
template <typename C, typename K>
void bench_n(const char *p_container, const Keys<K> &p_keys, int p_n) {
	const char *key = p_keys.name;
	const LocalVector<int> order = shuffled(p_n);
	const uint64_t mask = p_n ? uint64_t(p_n - 1) : 0; // every N is 0 or a power of two
	const uint64_t n = p_n;
	const uint64_t m = touched(p_n);
	C *c = nullptr;
	{
		const uint64_t repeats = builds(p_n);
		Sample sample(case_name(p_container, key, "insert", p_n), p_n ? repeats * n : repeats);
		for (uint64_t r = 0; r < repeats; r++) {
			C *filled = new C;
			for (int i = 0; i < p_n; i++) {
				put(*filled, p_keys.hit[i], i);
			}
			sample.checksum += filled->size();
			if (r + 1 < repeats) {
				delete filled; // the last one stays for the other cases
			} else {
				c = filled;
			}
		}
		sample.finish();
		CHECK(sample.checksum == repeats * n);
	}
	if (p_n) {
		const uint64_t count = passes(p_n);
		uint64_t expected = 0;
		for (uint64_t pass = 0; pass < count; pass++) {
			for (uint64_t i = 0; i < m; i++) {
				expected += uint64_t(order[(i + pass * 97) & mask]);
			}
		}
		Sample sample(case_name(p_container, key, "hit", p_n), count * m);
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			for (uint64_t i = 0; i < m; i++) {
				sample.checksum += uint64_t(find(*c, p_keys.hit[order[(i + pass * 97) & mask]]));
			}
		}
		sample.finish();
		CHECK(sample.checksum == expected);
	}
	{
		const uint64_t count = passes(p_n);
		const uint64_t lookups = p_n ? m : 1;
		Sample sample(case_name(p_container, key, "miss", p_n), count * lookups);
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			for (uint64_t i = 0; i < lookups; i++) {
				const uint64_t index = p_n ? uint64_t(order[(i + pass * 97) & mask]) : pass % MISS_MIN;
				sample.checksum += c->has(p_keys.miss[index]);
			}
		}
		sample.finish();
		CHECK(sample.checksum == 0);
	}
	{
		const uint64_t count = passes(p_n);
		Sample sample(case_name(p_container, key, "iterate", p_n), p_n ? count * n : count);
		for (uint64_t pass = 0; pass < count; pass++) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
			sample.checksum += sum(*c);
		}
		sample.finish();
		CHECK(sample.checksum == count * (n ? n * (n - 1) / 2 : 0));
	}
	if (p_n <= COPY_MAX_N) {
		const uint64_t count = builds(p_n);
		Sample sample(case_name(p_container, key, "copy", p_n), p_n ? count * n : count);
		for (uint64_t r = 0; r < count; r++) {
			const C copy = copy_of(*c);
			sample.checksum += copy.size();
		}
		sample.finish();
		CHECK(sample.checksum == count * n);
	}
	{
		// Small N erases from copies made untimed, so each case runs about TARGET erases.
		const uint64_t count = p_n ? builds(p_n) : TARGET;
		LocalVector<C *> targets;
		if (p_n && count > 1) {
			for (uint64_t r = 0; r < count; r++) {
				targets.push_back(new C(copy_of(*c)));
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
		sample.finish();
		CHECK(sample.checksum == (p_n ? targets.size() * m : 0));
		for (C *target : targets) {
			if (target != c) {
				delete target;
			}
		}
	}
	delete c;
}

template <typename C, typename K>
void bench_container(const char *p_container, const Keys<K> &p_keys) {
	for (int n : SIZES) {
		bench_n<C>(p_container, p_keys, n);
	}
}

// Vector2 keys: the generic Variant hash and comparison (x up to 2^20 is exact in a float).
Keys<Variant> vector2_keys() {
	return make_keys<Variant>("variant", [](uint64_t p_index) { return Variant(Vector2(real_t(p_index >> 1), real_t((p_index & 1) ? 1.5 : 0.5))); });
}

// One key set for every StringName-keyed container: interning 2M new names takes about 2 s,
// which a case per container repeated three times per process (D-03 budget).
TEST_CASE("[Bench] sname_keys" * doctest::skip()) {
	const Keys<StringName> keys = make_keys<StringName>("sname", [](uint64_t p_index) { return StringName(str_key(p_index)); });
	bench_container<HashMap<StringName, int>>("hashmap", keys);
	bench_container<AHashMap<StringName, int>>("ahashmap", keys);
	Keys<Variant> variants;
	variants.name = keys.name;
	variants.hit.reserve(MAX_N);
	variants.miss.reserve(MAX_N);
	for (int i = 0; i < MAX_N; i++) {
		variants.hit.push_back(Variant(keys.hit[i]));
		variants.miss.push_back(Variant(keys.miss[i]));
	}
	bench_container<Dictionary>("dict", variants);
}

TEST_CASE("[Bench] hashmap_variant" * doctest::skip()) {
	bench_container<HashMap<Variant, Variant>>("hashmap", vector2_keys());
}

TEST_CASE("[Bench] dict_str" * doctest::skip()) {
	bench_container<Dictionary>("dict", make_keys<Variant>("str", [](uint64_t p_index) { return Variant(str_key(p_index)); }));
}

TEST_CASE("[Bench] dict_i64" * doctest::skip()) {
	bench_container<Dictionary>("dict", make_keys<Variant>("i64", [](uint64_t p_index) { return Variant(int64_t(key_value(p_index))); }));
}

TEST_CASE("[Bench] dict_variant" * doctest::skip()) {
	bench_container<Dictionary>("dict", vector2_keys());
}

// StringName creation and comparison over N existing names, in query order: sname_<op>_<N>.
// p_body(a, b) runs one operation on names a and b (b follows a in the query order);
// a creation includes the StringName's destruction. N = 0 has no name to use.
template <typename F>
void bench_sname(const char *p_op, F p_body, bool p_compare) {
	for (int n : SIZES) {
		if (!n) {
			continue;
		}
		const LocalVector<int> order = shuffled(n);
		const uint64_t mask = uint64_t(n - 1);
		Sample sample(vformat("sname_%s_%d", p_op, n), TARGET);
		for (uint64_t i = 0; i < TARGET; i++) {
			sample.checksum += p_body(order[i & mask], order[(i + 1) & mask]) ? 1 : 0;
		}
		sample.finish();
		// Created names equal the existing one; two names in a row differ unless N is 1.
		CHECK(sample.checksum == (p_compare && n > 1 ? 0 : TARGET));
	}
}

struct Names {
	LocalVector<String> strings;
	LocalVector<StringName> names;

	Names() {
		strings.reserve(MAX_N);
		names.reserve(MAX_N);
		for (int i = 0; i < MAX_N; i++) {
			strings.push_back(str_key(uint64_t(i)));
			names.push_back(StringName(strings[i]));
		}
	}
};

// One set of names for the three StringName operations (as sname_keys, for the budget).
TEST_CASE("[Bench] sname_ops" * doctest::skip()) {
	const Names names;
	bench_sname("from_string", [&](int p_a, int) { return StringName(names.strings[p_a]) == names.names[p_a]; }, false);
	LocalVector<CharString> cstrs;
	cstrs.reserve(MAX_N);
	for (const String &string : names.strings) {
		cstrs.push_back(string.ascii());
	}
	bench_sname("from_cstr", [&](int p_a, int) { return StringName(cstrs[p_a].get_data()) == names.names[p_a]; }, false);
	bench_sname("compare", [&](int p_a, int p_b) { return names.names[p_a] == names.names[p_b]; }, true);
}

} // namespace TestForkBenchVariantKeys
