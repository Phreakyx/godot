/**************************************************************************/
/*  test_fork_hash_contracts.cpp                                          */
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

TEST_FORCE_LINK(test_fork_hash_contracts)

#include "core/string/string_name.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

// Contracts that every HashMap, AHashMap and HashSet backend must keep. They pin today's
// behavior; the containers carry no thread-safety guarantee, so every case is single-threaded.

namespace TestForkHashContracts {

template <typename M>
static Vector<int> map_keys(const M &p_map) {
	Vector<int> keys;
	for (const KeyValue<int, int> &E : p_map) {
		keys.push_back(E.key);
	}
	return keys;
}

static Vector<int> set_keys(const HashSet<int> &p_set) {
	Vector<int> keys;
	for (const int &key : p_set) {
		keys.push_back(key);
	}
	return keys;
}

struct ValueLess {
	bool operator()(const KeyValue<int, int> &p_a, const KeyValue<int, int> &p_b) const {
		return p_a.value < p_b.value;
	}
};

TEST_CASE("[Fork][HashMap] Iteration follows insertion order") {
	HashMap<int, int> map;
	map.insert(5, 0);
	map.insert(3, 0);
	map.insert(9, 0);
	map.insert(1, 0);
	CHECK(map_keys(map) == Vector<int>{ 5, 3, 9, 1 });
	CHECK(map.last()->key == 1);
}

TEST_CASE("[Fork][HashMap] Front insert puts the element first") {
	HashMap<int, int> map;
	map.insert(1, 1, true);
	map.insert(2, 2);
	map.insert(3, 3, true);
	CHECK(map_keys(map) == Vector<int>{ 3, 1, 2 });
	CHECK(map.last()->key == 2);

	// An existing key keeps its position: the front flag only applies to new keys.
	map.insert(2, 22, true);
	CHECK(map_keys(map) == Vector<int>{ 3, 1, 2 });
	CHECK(map[2] == 22);
}

TEST_CASE("[Fork][HashMap] sort orders by key") {
	HashMap<int, int> map;
	map.insert(3, 30);
	map.insert(1, 10);
	map.insert(2, 20);
	map.sort();
	CHECK(map_keys(map) == Vector<int>{ 1, 2, 3 });
	CHECK(map.last()->key == 3);
	CHECK(map[2] == 20);
}

TEST_CASE("[Fork][HashMap] sort_custom keeps the insertion order of equal elements") {
	HashMap<int, int> map;
	map.insert(10, 1);
	map.insert(20, 0);
	map.insert(30, 1);
	map.insert(40, 0);
	map.sort_custom<ValueLess>();
	CHECK(map_keys(map) == Vector<int>{ 20, 40, 10, 30 });
	CHECK(map.last()->key == 30);
}

TEST_CASE("[Fork][HashMap] replace_key keeps position and address") {
	HashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	map.insert(3, 30);
	int *value = map.getptr(2);
	CHECK(map.replace_key(2, 200));
	CHECK(map_keys(map) == Vector<int>{ 1, 200, 3 });
	CHECK(map.getptr(200) == value);
	CHECK(*value == 20);
	CHECK_FALSE(map.has(2));
	CHECK(map.replace_key(200, 200));
	CHECK(map.size() == 3);
}

TEST_CASE("[Fork][HashMap] Pointer stability through growth and erase of other keys") {
	HashMap<int, int> map;
	map.insert(1, 10);
	int *value = map.getptr(1);
	for (int i = 2; i < 10000; i++) {
		map.insert(i, i);
	}
	for (int i = 2; i < 10000; i += 2) {
		map.erase(i);
	}
	CHECK(map.getptr(1) == value);
	CHECK(*value == 10);
	CHECK(map.begin()->key == 1);
	CHECK(map.size() == 5000);
}

TEST_CASE("[Fork][HashMap] Erase during iteration of other elements") {
	HashMap<int, int> map;
	for (int i = 1; i <= 6; i++) {
		map.insert(i, i);
	}
	Vector<int> visited;
	for (const KeyValue<int, int> &E : map) {
		visited.push_back(E.key);
		if (E.key == 3) {
			map.erase(1); // Already visited.
			map.erase(5); // Not visited yet: the iteration skips it.
		}
	}
	CHECK(visited == Vector<int>{ 1, 2, 3, 4, 6 });
	CHECK(map_keys(map) == Vector<int>{ 2, 3, 4, 6 });

	// remove(it) of the current element is safe when the next iterator is taken first.
	for (HashMap<int, int>::Iterator it = map.begin(); it;) {
		HashMap<int, int>::Iterator next = it;
		++next;
		if (it->key % 2 == 0) {
			map.remove(it);
		}
		it = next;
	}
	CHECK(map_keys(map) == Vector<int>{ 3 });
}

TEST_CASE("[Fork][HashMap] Inserting an existing key keeps one element at its position") {
	HashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	map.insert(3, 30);
	int *value = map.getptr(2);
	HashMap<int, int>::Iterator it = map.insert(2, 99);
	CHECK(it->key == 2);
	CHECK(&it->value == value);
	CHECK(map[2] == 99);
	CHECK(map.size() == 3);
	CHECK(map_keys(map) == Vector<int>{ 1, 2, 3 });
	map[2] = 7;
	CHECK(map.size() == 3);
	CHECK(*value == 7);
}

TEST_CASE("[Fork][HashMap] Erase twice returns true then false") {
	HashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	CHECK(map.erase(1));
	CHECK_FALSE(map.erase(1));
	CHECK(map.size() == 1);
	CHECK(map_keys(map) == Vector<int>{ 2 });
}

TEST_CASE("[Fork][HashMap] Empty map") {
	HashMap<int, int> map;
	CHECK(map.is_empty());
	CHECK(map.begin() == map.end());
	CHECK(map.last() == map.end());
	CHECK(map.getptr(1) == nullptr);
	CHECK(map.find(1) == map.end());
	CHECK_FALSE(map.has(1));
	CHECK_FALSE(map.erase(1));
	CHECK(map_keys(map).is_empty());

	map.insert(1, 10);
	map.erase(1);
	CHECK(map.begin() == map.end());
	CHECK(map.getptr(1) == nullptr);
	CHECK_FALSE(map.erase(1));
}

TEST_CASE("[Fork][HashMap] Single element") {
	HashMap<int, int> map;
	map.insert(7, 70);
	HashMap<int, int>::Iterator it = map.begin();
	CHECK(it->key == 7);
	CHECK(map.last() == it);
	CHECK(map.find(7) == it);
	++it;
	CHECK(it == map.end());
	CHECK(map.erase(7));
	CHECK(map.is_empty());
	CHECK(map.begin() == map.end());
}

// The pins below guard a backend that owns node storage per map (FORK_BACKEND_HASHMAP_POOL).

TEST_CASE("[Fork][HashMap] Element pointers survive a move after the source is destroyed") {
	using IntMap = HashMap<int, int>;
	IntMap *source = memnew(IntMap);
	Vector<int *> values;
	for (int i = 0; i < 100; i++) {
		source->insert(i, i * 10);
		values.push_back(source->getptr(i));
	}
	IntMap moved(std::move(*source));
	memdelete(source);
	for (int i = 0; i < 100; i++) {
		CHECK(*values[i] == i * 10);
		CHECK(moved.getptr(i) == values[i]);
	}

	// Move assignment into a map that holds elements of its own.
	source = memnew(IntMap);
	source->insert(7, 70);
	int *seven = source->getptr(7);
	moved = std::move(*source);
	memdelete(source);
	CHECK(moved.size() == 1);
	CHECK(moved.getptr(7) == seven);
	CHECK(*seven == 70);
	moved.insert(8, 80);
	CHECK(map_keys(moved) == Vector<int>{ 7, 8 });
}

TEST_CASE("[Fork][HashMap] clear() then insert") {
	HashMap<int, int> map;
	for (int i = 0; i < 100; i++) {
		map.insert(i, i);
	}
	map.clear();
	CHECK(map.is_empty());
	CHECK(map.begin() == map.end());
	for (int i = 0; i < 10; i++) {
		map.insert(100 - i, i);
	}
	CHECK(map.size() == 10);
	CHECK(map_keys(map) == Vector<int>{ 100, 99, 98, 97, 96, 95, 94, 93, 92, 91 });
	CHECK(map[95] == 5);
	map.clear();
	map.clear();
	CHECK(map.is_empty());
}

TEST_CASE("[Fork][HashMap] A copy is independent of its source") {
	HashMap<int, int> source;
	for (int i = 1; i <= 5; i++) {
		source.insert(i, i * 10);
	}
	HashMap<int, int> copy(source);
	copy.insert(6, 60);
	copy.erase(2);
	copy[1] = 11;
	CHECK(map_keys(source) == Vector<int>{ 1, 2, 3, 4, 5 });
	CHECK(source[1] == 10);
	CHECK(source[2] == 20);
	CHECK(map_keys(copy) == Vector<int>{ 1, 3, 4, 5, 6 });

	HashMap<int, int> assigned;
	assigned.insert(9, 90);
	assigned = source;
	assigned.erase(5);
	CHECK(map_keys(assigned) == Vector<int>{ 1, 2, 3, 4 });
	CHECK(map_keys(source) == Vector<int>{ 1, 2, 3, 4, 5 });
}

TEST_CASE("[Fork][HashMap] reserve() before the first insert keeps pointers stable") {
	HashMap<int, int> map;
	map.reserve(64);
	Vector<int *> values;
	for (int i = 0; i < 64; i++) {
		map.insert(i, i + 1);
		values.push_back(map.getptr(i));
	}
	for (int i = 0; i < 64; i++) {
		CHECK(map.getptr(i) == values[i]);
		CHECK(*values[i] == i + 1);
	}
	map.insert(64, 65); // past the reserved count
	CHECK(map.getptr(0) == values[0]);
	CHECK(map.size() == 65);
}

TEST_CASE("[Fork][HashMap] Pointers stay valid across node page boundaries") {
	HashMap<int, int> map;
	Vector<int *> values;
	bool stable = true;
	for (int i = 0; i < 40; i++) {
		map.insert(i, i);
		values.push_back(map.getptr(i));
		for (int j = 0; j <= i; j++) {
			stable = stable && map.getptr(j) == values[j] && *values[j] == j;
		}
	}
	CHECK(stable);

	// Erase, then insert new keys into the freed nodes: the survivors stay put, the order follows insertion.
	for (int i = 0; i < 40; i += 3) {
		map.erase(i);
	}
	for (int i = 100; i < 110; i++) {
		map.insert(i, i);
	}
	for (int i = 0; i < 40; i++) {
		if (i % 3) {
			CHECK(map.getptr(i) == values[i]);
		}
	}
	CHECK(map.size() == 26 + 10);
	CHECK(map.last()->key == 109);
}

TEST_CASE("[Fork][HashMap] Re-inserting an existing key at each page size keeps size and address") {
	for (int n : { 1, 2, 3, 4, 7, 8, 15, 16 }) {
		HashMap<int, int> map;
		for (int i = 0; i < n; i++) {
			map.insert(i, i);
		}
		int *last = map.getptr(n - 1);
		map.insert(n - 1, -1);
		map[0] = -2;
		CHECK(map.size() == uint32_t(n));
		CHECK(map.getptr(n - 1) == last);
		CHECK(map.last()->key == n - 1);
		CHECK(map[0] == -2);
	}
}

// The pins below cross the small-map threshold (FORK_BACKEND_HASHMAP_SMALL): below it a map
// has no index and walks its list; past it the index is built from the list. They run on every
// build and cross 8 (and 4) either way.
static constexpr int FORK_SMALL_THRESHOLD_FOR_TESTS = 8;

template <typename K>
static K small_key(int p_i) {
	if constexpr (std::is_same_v<K, String>) {
		return "k" + itos(p_i);
	} else {
		return K(p_i);
	}
}

template <typename K>
static void check_small_growth() {
	HashMap<K, int> map;
	Vector<int *> values;
	bool stable = true;
	bool ordered = true;
	for (int i = 0; i < FORK_SMALL_THRESHOLD_FOR_TESTS + 3; i++) {
		map.insert(small_key<K>(i), i);
		values.push_back(map.getptr(small_key<K>(i)));
		int j = 0;
		for (const KeyValue<K, int> &E : map) {
			ordered = ordered && E.key == small_key<K>(j) && &E.value == values[j];
			j++;
		}
		ordered = ordered && j == i + 1;
		for (j = 0; j <= i; j++) {
			stable = stable && map.getptr(small_key<K>(j)) == values[j] && *values[j] == j;
		}
		stable = stable && !map.has(small_key<K>(i + 1));
	}
	CHECK(stable);
	CHECK(ordered);
	CHECK(map.size() == uint32_t(FORK_SMALL_THRESHOLD_FOR_TESTS + 3));
}

TEST_CASE("[Fork][HashMap] Element pointers and order hold while a small map grows past the threshold") {
	check_small_growth<int>();
	check_small_growth<String>();
	check_small_growth<int64_t>();
}

TEST_CASE("[Fork][HashMap] Erasing back below the threshold, then inserting again") {
	constexpr int count = FORK_SMALL_THRESHOLD_FOR_TESTS + 3;
	HashMap<int, int> map;
	for (int i = 0; i < count; i++) {
		map.insert(i, i * 10);
	}
	int *kept = map.getptr(count - 1);
	for (int i = 0; i < count - 2; i++) {
		CHECK(map.erase(i));
	}
	CHECK(map_keys(map) == Vector<int>{ count - 2, count - 1 });
	CHECK(map.getptr(count - 1) == kept);
	CHECK_FALSE(map.has(0));
	for (int i = 100; i < 100 + count; i++) {
		map.insert(i, i);
	}
	map.insert(-1, -1, true); // Front insert.
	CHECK(map.size() == uint32_t(count + 3));
	CHECK(map.begin()->key == -1);
	CHECK(map.last()->key == 100 + count - 1);
	CHECK(map.getptr(count - 1) == kept);
	CHECK(*kept == (count - 1) * 10);
	for (int i = 100; i < 100 + count; i++) {
		REQUIRE(map.getptr(i) != nullptr);
		CHECK(*map.getptr(i) == i);
	}
	for (int i = 0; i < count - 2; i++) {
		CHECK_FALSE(map.has(i));
	}

	// A copy of a small map and of a map past the threshold keeps the order.
	HashMap<int, int> small;
	small.insert(3, 3);
	small.insert(1, 1);
	HashMap<int, int> small_copy(small);
	HashMap<int, int> large_copy;
	large_copy = map;
	small_copy.insert(2, 2);
	CHECK(map_keys(small_copy) == Vector<int>{ 3, 1, 2 });
	CHECK(map_keys(small) == Vector<int>{ 3, 1 });
	CHECK(map_keys(large_copy) == map_keys(map));
	CHECK(large_copy.has(100));
	CHECK_FALSE(large_copy.has(0));
}

TEST_CASE("[Fork][HashMap] reserve() past the threshold, then inserts") {
	HashMap<int, int> map;
	map.reserve(FORK_SMALL_THRESHOLD_FOR_TESTS + 10);
	CHECK_FALSE(map.has(0));
	CHECK(map.getptr(0) == nullptr);
	for (int i = 0; i < FORK_SMALL_THRESHOLD_FOR_TESTS + 10; i++) {
		map.insert(i, i + 1);
		REQUIRE(map.getptr(i) != nullptr);
		CHECK(*map.getptr(i) == i + 1);
		CHECK(map.find(0) == map.begin());
		CHECK_FALSE(map.has(i + 1));
	}
	CHECK(map.last()->key == FORK_SMALL_THRESHOLD_FOR_TESTS + 9);

	HashMap<int, int> sized(FORK_SMALL_THRESHOLD_FOR_TESTS + 10);
	sized[5] = 50;
	CHECK(sized.has(5));
	CHECK(sized[5] == 50);
	CHECK(sized.size() == 1);

	HashMap<int, int> listed{ { 5, 0 }, { 3, 1 }, { 9, 2 } };
	CHECK(map_keys(listed) == Vector<int>{ 5, 3, 9 });
	CHECK(listed[9] == 2);
}

TEST_CASE("[Fork][HashMap] Erase, remove and replace_key below the threshold") {
	HashMap<int, int> map;
	for (int i = 1; i <= 5; i++) {
		map.insert(i, i);
	}
	int *four = map.getptr(4);
	Vector<int> visited;
	for (const KeyValue<int, int> &E : map) {
		visited.push_back(E.key);
		if (E.key == 2) {
			map.erase(1); // Head, already visited.
			map.erase(3); // Not visited yet.
			map.erase(5); // Tail.
		}
	}
	CHECK(visited == Vector<int>{ 1, 2, 4 });
	CHECK(map_keys(map) == Vector<int>{ 2, 4 });
	CHECK(map.last()->key == 4);
	CHECK(map.getptr(4) == four);
	CHECK_FALSE(map.erase(3));

	CHECK(map.replace_key(4, 40));
	CHECK(map.getptr(40) == four);
	CHECK_FALSE(map.has(4));
	CHECK(map_keys(map) == Vector<int>{ 2, 40 });

	for (HashMap<int, int>::Iterator it = map.begin(); it;) {
		HashMap<int, int>::Iterator next = it;
		++next;
		map.remove(it);
		it = next;
	}
	CHECK(map.is_empty());
	CHECK(map.begin() == map.end());
	map.insert(7, 7);
	CHECK(map_keys(map) == Vector<int>{ 7 });
}

TEST_CASE("[Fork][HashMap] clear() past the threshold, then inserts below it") {
	HashMap<int, int> map;
	for (int i = 0; i < FORK_SMALL_THRESHOLD_FOR_TESTS * 4; i++) {
		map.insert(i, i);
	}
	map.clear();
	CHECK(map.is_empty());
	CHECK_FALSE(map.has(0));
	CHECK(map.find(1) == map.end());
	for (int i = 0; i < FORK_SMALL_THRESHOLD_FOR_TESTS / 2; i++) {
		map.insert(50 - i, i);
	}
	CHECK(map_keys(map) == Vector<int>{ 50, 49, 48, 47 });
	CHECK(map[48] == 2);
	CHECK_FALSE(map.has(0));
	for (int i = 0; i < FORK_SMALL_THRESHOLD_FOR_TESTS * 2; i++) {
		map.insert(i, -i);
	}
	CHECK(map.size() == uint32_t(FORK_SMALL_THRESHOLD_FOR_TESTS / 2 + FORK_SMALL_THRESHOLD_FOR_TESTS * 2));
	CHECK(map[48] == 2);
	CHECK(map[3] == -3);
	CHECK(map.last()->key == FORK_SMALL_THRESHOLD_FOR_TESTS * 2 - 1);
}

#ifdef DEBUG_ENABLED
TEST_CASE("[Fork][HashMap] Node memory of empty, single-element and cleared maps") {
	using E = HashMapElement<int, int>;
	const uint64_t before = Memory::get_mem_usage();
	using IntMap = HashMap<int, int>;
	IntMap *map = memnew(IntMap);
	const uint64_t empty = Memory::get_mem_usage();
	CHECK(empty - before == sizeof(IntMap)); // nothing allocated before the first insert

	map->insert(1, 1);
	const uint64_t one = Memory::get_mem_usage();
	map->clear();
	const uint64_t cleared = Memory::get_mem_usage();
	CHECK(one - cleared == sizeof(E)); // one element owns one node's bytes
#if FORK_BACKEND_HASHMAP_SMALL
	CHECK(cleared == empty); // a one-element map has no index
#else
	CHECK(cleared > empty); // the index stays allocated, as upstream keeps it
#endif

	for (int i = 0; i < 3; i++) {
		map->insert(i, i);
	}
#if FORK_BACKEND_HASHMAP_SMALL == 0 || FORK_BACKEND_HASHMAP_SMALL >= 3
	CHECK(Memory::get_mem_usage() - cleared == 3 * sizeof(E)); // nodes only: the index is kept, or a small map has none
#endif
	map->clear();
	CHECK(Memory::get_mem_usage() == cleared); // clear() returns every node byte

	memdelete(map);
	CHECK(Memory::get_mem_usage() == before);
}
#endif

TEST_CASE("[Fork][AHashMap] Iteration follows insertion order") {
	AHashMap<int, int> map;
	map.insert(5, 0);
	map.insert(3, 0);
	map.insert(9, 0);
	map.insert(1, 0);
	CHECK(map_keys(map) == Vector<int>{ 5, 3, 9, 1 });
	CHECK(map.get_index(5) == 0);
	CHECK(map.get_index(1) == 3);
}

TEST_CASE("[Fork][AHashMap] Erase moves the last element into the hole") {
	AHashMap<int, int> map;
	for (int i = 1; i <= 5; i++) {
		map.insert(i, i * 10);
	}
	int *hole = map.getptr(2);
	int *first = map.getptr(1);
	int *third = map.getptr(3);
	CHECK(map.erase(2));
	CHECK(map_keys(map) == Vector<int>{ 1, 5, 3, 4 });
	CHECK(map.get_index(5) == 1);
	CHECK(map.get_index(2) == -1);
	CHECK(map.get_by_index(1).key == 5);
	CHECK(map.get_by_index(1).value == 50);
	// Only the moved element changes address here. Growth reallocates the element array, so
	// AHashMap element pointers are not stable across inserts.
	CHECK(map.getptr(5) == hole);
	CHECK(map.getptr(1) == first);
	CHECK(map.getptr(3) == third);

	// Erasing the last element moves nothing.
	CHECK(map.erase(4));
	CHECK(map_keys(map) == Vector<int>{ 1, 5, 3 });
}

TEST_CASE("[Fork][AHashMap] erase_by_index") {
	AHashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	map.insert(3, 30);
	CHECK_FALSE(map.erase_by_index(3));
	CHECK(map.erase_by_index(0));
	CHECK(map_keys(map) == Vector<int>{ 3, 2 });
	CHECK(map.get_index(3) == 0);
	CHECK(map.size() == 2);
}

TEST_CASE("[Fork][AHashMap] insert_new appends") {
	AHashMap<int, int> map;
	map.insert_new(1, 10);
	AHashMap<int, int>::Iterator it = map.insert_new(2, 20);
	CHECK(it->key == 2);
	CHECK(it->value == 20);
	CHECK(map.get_index(2) == 1);
	CHECK(map_keys(map) == Vector<int>{ 1, 2 });
}

TEST_CASE("[Fork][AHashMap] replace_key keeps the index") {
	AHashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	map.insert(3, 30);
	CHECK(map.replace_key(2, 200));
	CHECK(map_keys(map) == Vector<int>{ 1, 200, 3 });
	CHECK(map.get_index(200) == 1);
	CHECK(map[200] == 20);
	CHECK_FALSE(map.has(2));
}

TEST_CASE("[Fork][AHashMap] Inserting an existing key keeps one element at its position") {
	AHashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	map.insert(3, 30);
	AHashMap<int, int>::Iterator it = map.insert(2, 99);
	CHECK(it->key == 2);
	CHECK(map[2] == 99);
	CHECK(map.size() == 3);
	CHECK(map.get_index(2) == 1);
	CHECK(map_keys(map) == Vector<int>{ 1, 2, 3 });
}

TEST_CASE("[Fork][AHashMap] Erase twice returns true then false") {
	AHashMap<int, int> map;
	map.insert(1, 10);
	map.insert(2, 20);
	CHECK(map.erase(1));
	CHECK_FALSE(map.erase(1));
	CHECK(map.size() == 1);
	CHECK(map_keys(map) == Vector<int>{ 2 });
}

TEST_CASE("[Fork][AHashMap] Empty and single-element maps") {
	AHashMap<int, int> map;
	CHECK(map.is_empty());
	CHECK(map.begin() == map.end());
	CHECK(map.getptr(1) == nullptr);
	CHECK(map.find(1) == map.end());
	CHECK(map.get_index(1) == -1);
	CHECK_FALSE(map.erase(1));
	CHECK_FALSE(map.erase_by_index(0));

	map.insert(7, 70);
	CHECK(map.get_index(7) == 0);
	CHECK(map.get_by_index(0).key == 7);
	AHashMap<int, int>::Iterator it = map.begin();
	CHECK(it->key == 7);
	CHECK(map.last() == it);
	++it;
	CHECK(it == map.end());

	CHECK(map.erase(7));
	CHECK(map.begin() == map.end());
	CHECK(map.getptr(7) == nullptr);
}

TEST_CASE("[Fork][HashSet] Iteration follows insertion order") {
	HashSet<int> set;
	set.insert(5);
	set.insert(3);
	set.insert(9);
	set.insert(1);
	CHECK(set_keys(set) == Vector<int>{ 5, 3, 9, 1 });
	CHECK(*set.last() == 1);
}

TEST_CASE("[Fork][HashSet] Erase moves the last key into the hole") {
	HashSet<int> set;
	for (int i = 1; i <= 5; i++) {
		set.insert(i);
	}
	CHECK(set.erase(2));
	CHECK(set_keys(set) == Vector<int>{ 1, 5, 3, 4 });
	CHECK(set.erase(4));
	CHECK(set_keys(set) == Vector<int>{ 1, 5, 3 });
	CHECK(set.has(5));
	CHECK_FALSE(set.has(2));
}

TEST_CASE("[Fork][HashSet] Inserting an existing key keeps one element at its position") {
	HashSet<int> set;
	set.insert(1);
	set.insert(2);
	set.insert(3);
	HashSet<int>::Iterator it = set.insert(2);
	CHECK(*it == 2);
	CHECK(it == set.find(2));
	CHECK(set.size() == 3);
	CHECK(set_keys(set) == Vector<int>{ 1, 2, 3 });
}

TEST_CASE("[Fork][HashSet] Erase twice returns true then false") {
	HashSet<int> set;
	set.insert(1);
	set.insert(2);
	CHECK(set.erase(1));
	CHECK_FALSE(set.erase(1));
	CHECK(set.size() == 1);
	CHECK(set_keys(set) == Vector<int>{ 2 });
}

TEST_CASE("[Fork][HashSet] Empty and single-element sets") {
	HashSet<int> set;
	CHECK(set.is_empty());
	CHECK(set.begin() == set.end());
	CHECK(set.last() == set.end());
	CHECK(set.find(1) == set.end());
	CHECK_FALSE(set.has(1));
	CHECK_FALSE(set.erase(1));

	set.insert(7);
	HashSet<int>::Iterator it = set.begin();
	CHECK(*it == 7);
	CHECK(set.last() == it);
	CHECK(set.find(7) == it);
	++it;
	CHECK(it == set.end());

	CHECK(set.erase(7));
	CHECK(set.begin() == set.end());
}

// The pins below guard the slot arithmetic (FORK_BACKEND_HASH_POW2): every key in one probe
// run, empty tables and the key types with their own hashes.

struct ConstantHasher {
	static uint32_t hash(const int &) { return 7; }
};

TEST_CASE("[Fork][HashMap] Keys with one hash are found, erased and iterated in insertion order") {
	HashMap<int, int, ConstantHasher> map;
	for (int i = 0; i < 50; i++) {
		map.insert(i, i * 10);
	}
	CHECK(map.size() == 50);
	for (int i = 0; i < 50; i++) {
		REQUIRE(map.getptr(i) != nullptr);
		CHECK(*map.getptr(i) == i * 10);
	}
	for (int i = 0; i < 50; i += 2) {
		CHECK(map.erase(i));
	}
	Vector<int> expected;
	for (int i = 1; i < 50; i += 2) {
		expected.push_back(i);
		CHECK(map.has(i));
		CHECK_FALSE(map.has(i - 1));
	}
	CHECK(map_keys(map) == expected);
}

TEST_CASE("[Fork][HashSet] Keys with one hash are found and iterate in insertion order") {
	HashSet<int, ConstantHasher> set;
	Vector<int> expected;
	for (int i = 0; i < 50; i++) {
		set.insert(i);
		expected.push_back(i);
	}
	Vector<int> keys;
	for (const int &key : set) {
		keys.push_back(key);
	}
	CHECK(keys == expected);
	for (int i = 0; i < 50; i++) {
		CHECK(set.has(i));
	}
	for (int i = 0; i < 50; i += 2) {
		CHECK(set.erase(i));
	}
	for (int i = 0; i < 50; i++) {
		CHECK(set.has(i) == (i % 2 == 1));
	}
	CHECK(set.size() == 25);
}

// The pins below guard the slot search (FORK_BACKEND_HASH_TAGS): keys with one tag, tables
// filled to each growth point (probe groups wrap past the last slot) and empty tables.

// The order of a dense container after erasing p_key: the last key moves into its place.
static void erase_moves_last(Vector<int> &r_keys, int p_key) {
	const int64_t idx = r_keys.find(p_key);
	REQUIRE(idx >= 0);
	r_keys.set(idx, r_keys[r_keys.size() - 1]);
	r_keys.remove_at(r_keys.size() - 1);
}

TEST_CASE("[Fork][AHashMap] Keys with one hash are found, erased and keep the dense order") {
	AHashMap<int, int, ConstantHasher> map;
	Vector<int> expected;
	for (int i = 0; i < 40; i++) {
		map.insert(i, i * 10);
		expected.push_back(i);
	}
	for (int i = 0; i < 40; i++) {
		REQUIRE(map.getptr(i) != nullptr);
		CHECK(*map.getptr(i) == i * 10);
	}
	for (int i = 0; i < 40; i += 2) {
		CHECK(map.erase(i));
		erase_moves_last(expected, i);
	}
	for (int i = 0; i < 40; i++) {
		CHECK(map.has(i) == (i % 2 == 1));
	}
	REQUIRE(map.size() == uint32_t(expected.size()));
	for (int i = 0; i < expected.size(); i++) {
		CHECK(map.get_by_index(i).key == expected[i]);
		CHECK(map.get_by_index(i).value == expected[i] * 10);
		CHECK(map.get_index(expected[i]) == i);
	}
}

template <typename M>
static bool ahashmap_finds_first(M &p_map, int p_count) {
	bool found = !p_map.has(p_count) && !p_map.has(-1 - p_count);
	for (int i = 0; i < p_count; i++) {
		found = found && p_map.has(i) && p_map.get_index(i) == i;
	}
	return found;
}

TEST_CASE("[Fork][AHashMap] Every key is found up to each growth point and after it") {
	AHashMap<int, int> map;
	AHashMap<int, int, ConstantHasher> one_hash;
	int n = 0;
	for (int growth = 0; growth < 4; growth++) {
		const uint32_t capacity = map.get_capacity();
		bool found = true;
		while (map.get_capacity() == capacity) {
			map.insert(n, n);
			one_hash.insert(n, n);
			n++;
			found = found && ahashmap_finds_first(map, n) && ahashmap_finds_first(one_hash, n);
		}
		CHECK(found);
		CHECK(map.get_capacity() == capacity * 2);
	}
}

#ifdef DEBUG_ENABLED
TEST_CASE("[Fork][AHashMap] An empty, reserved or copied empty map finds nothing and allocates nothing") {
	const uint64_t before = Memory::get_mem_usage();
	{
		AHashMap<int, int> map;
		CHECK_FALSE(map.has(1));
		CHECK(map.getptr(1) == nullptr);
		CHECK(map.find(1) == map.end());
		CHECK(map.get_index(1) == -1);
		CHECK_FALSE(map.erase(1));
		map.reserve(100);
		CHECK(map.get_capacity() >= 100);
		CHECK_FALSE(map.has(1));
		CHECK_FALSE(map.erase(1));
		AHashMap<int, int> copy(map);
		CHECK_FALSE(copy.has(1));
		AHashMap<int, int> moved(std::move(copy));
		CHECK_FALSE(moved.has(1));
		CHECK(Memory::get_mem_usage() == before);
	}
	CHECK(Memory::get_mem_usage() == before);

	// Emptied by erase: the table stays allocated and finds nothing.
	AHashMap<int, int> map;
	for (int i = 0; i < 20; i++) {
		map.insert(i, i);
	}
	for (int i = 0; i < 20; i++) {
		CHECK(map.erase(i));
	}
	bool none = map.is_empty() && map.begin() == map.end();
	for (int i = 0; i < 20; i++) {
		none = none && !map.has(i) && map.getptr(i) == nullptr && !map.erase(i);
	}
	CHECK(none);
}
#endif

TEST_CASE("[Fork][HashSet] Keys with one hash are found, erased and keep the dense order") {
	HashSet<int, ConstantHasher> set;
	Vector<int> expected;
	for (int i = 0; i < 40; i++) {
		set.insert(i);
		expected.push_back(i);
	}
	for (int i = 0; i < 40; i++) {
		CHECK(set.has(i));
	}
	for (int i = 0; i < 40; i += 2) {
		CHECK(set.erase(i));
		erase_moves_last(expected, i);
	}
	for (int i = 0; i < 40; i++) {
		CHECK(set.has(i) == (i % 2 == 1));
	}
	Vector<int> keys;
	for (const int &key : set) {
		keys.push_back(key);
	}
	CHECK(keys == expected);
}

template <typename S>
static bool hashset_finds_first(const S &p_set, int p_count) {
	bool found = !p_set.has(p_count) && !p_set.has(-1 - p_count);
	for (int i = 0; i < p_count; i++) {
		found = found && p_set.has(i);
	}
	return found;
}

TEST_CASE("[Fork][HashSet] Every key is found up to each growth point and after it") {
	HashSet<int> set;
	HashSet<int, ConstantHasher> one_hash;
	int n = 0;
	for (int growth = 0; growth < 4; growth++) {
		const uint32_t capacity = set.get_capacity();
		bool found = true;
		while (set.get_capacity() == capacity) {
			set.insert(n);
			one_hash.insert(n);
			n++;
			found = found && hashset_finds_first(set, n) && hashset_finds_first(one_hash, n);
		}
		CHECK(found);
		CHECK(set.get_capacity() > capacity);
	}
}

#ifdef DEBUG_ENABLED
TEST_CASE("[Fork][HashSet] A reserved, copied or emptied set finds nothing") {
	const uint64_t before = Memory::get_mem_usage();
	{
		HashSet<int> set;
		set.reserve(100);
		CHECK(set.get_capacity() >= 100);
		CHECK_FALSE(set.has(1));
		CHECK_FALSE(set.erase(1));
		HashSet<int> copy(set);
		CHECK_FALSE(copy.has(1));
		HashSet<int> moved(std::move(copy));
		CHECK_FALSE(moved.has(1));
		CHECK(Memory::get_mem_usage() == before);
	}
	CHECK(Memory::get_mem_usage() == before);

	HashSet<int> set;
	for (int i = 0; i < 20; i++) {
		set.insert(i);
	}
	for (int i = 0; i < 20; i++) {
		CHECK(set.erase(i));
	}
	bool none = set.is_empty() && set.begin() == set.end();
	for (int i = 0; i < 20; i++) {
		none = none && !set.has(i) && !set.erase(i);
	}
	CHECK(none);
}
#endif

#if FORK_BACKEND_HASH_TAGS
TEST_CASE("[Fork][HashTags] Group matches agree with a byte loop") {
	uint8_t bytes[64];
	uint32_t state = 1;
	bool agree = true;
	for (int round = 0; round < 2000; round++) {
		for (uint8_t &b : bytes) {
			state = state * 1664525u + 1013904223u;
			const uint32_t r = state >> 8;
			const uint8_t kinds[4] = { HashTags::EMPTY, HashTags::DELETED, uint8_t(r >> 2), uint8_t((r >> 2) & 0x7F) };
			b = kinds[r & 3];
		}
		const int offset = round % 48;
		const uint8_t probes[] = { HashTags::EMPTY, HashTags::DELETED, 0, 1, 0x7F, uint8_t(bytes[offset + 5] & 0x7F) };
		for (uint8_t probe : probes) {
			uint32_t expected = 0;
			for (int i = 0; i < 16; i++) {
				expected |= uint32_t(bytes[offset + i] == probe) << i;
			}
			agree = agree && HashTags::match(bytes + offset, probe) == expected && HashTags::match_swar(bytes + offset, probe) == expected;
		}
		uint32_t expected = 0;
		for (int i = 0; i < 16; i++) {
			expected |= uint32_t(bytes[offset + i] >= 0x80) << i;
		}
		agree = agree && HashTags::match_free(bytes + offset) == expected && HashTags::match_free_swar(bytes + offset) == expected;
	}
	CHECK(agree);
}
#endif

#ifdef DEBUG_ENABLED
TEST_CASE("[Fork][HashMap] An empty map finds nothing and allocates nothing") {
	const uint64_t before = Memory::get_mem_usage();
	{
		HashMap<int, int> map;
		CHECK_FALSE(map.has(1));
		CHECK(map.getptr(1) == nullptr);
		CHECK(map.find(1) == map.end());
		CHECK_FALSE(map.erase(1));
		CHECK(map.begin() == map.end());
		CHECK(Memory::get_mem_usage() == before);
	}
	CHECK(Memory::get_mem_usage() == before);
}

TEST_CASE("[Fork][HashSet] An empty set finds nothing and allocates nothing") {
	const uint64_t before = Memory::get_mem_usage();
	{
		HashSet<int> set;
		CHECK_FALSE(set.has(1));
		CHECK(set.find(1) == set.end());
		CHECK_FALSE(set.erase(1));
		CHECK(set.begin() == set.end());
		CHECK(Memory::get_mem_usage() == before);
	}
	CHECK(Memory::get_mem_usage() == before);
}
#endif

template <typename K>
static void check_key_type(const Vector<K> &p_keys) {
	HashMap<K, int> map;
	HashSet<K> set;
	for (int i = 0; i < p_keys.size(); i++) {
		map.insert(p_keys[i], i);
		set.insert(p_keys[i]);
	}
	CHECK(map.size() == uint32_t(p_keys.size()));
	CHECK(set.size() == uint32_t(p_keys.size()));
	int i = 0;
	for (const KeyValue<K, int> &E : map) {
		CHECK(E.key == p_keys[i]);
		CHECK(E.value == i);
		i++;
	}
	i = 0;
	for (const K &key : set) {
		CHECK(key == p_keys[i]);
		i++;
	}
	for (i = 0; i < p_keys.size(); i += 2) {
		CHECK(map.erase(p_keys[i]));
		CHECK(set.erase(p_keys[i]));
	}
	for (i = 0; i < p_keys.size(); i++) {
		const bool kept = i % 2 == 1;
		CHECK(map.has(p_keys[i]) == kept);
		CHECK(set.has(p_keys[i]) == kept);
		if (kept) {
			REQUIRE(map.getptr(p_keys[i]) != nullptr);
			CHECK(*map.getptr(p_keys[i]) == i);
		}
	}
}

TEST_CASE("[Fork][HashMap] String, StringName, int64 and pointer keys") {
	constexpr int count = 200;
	Vector<String> strings;
	Vector<StringName> names;
	Vector<int64_t> ints;
	Vector<const int *> pointers;
	static int targets[count];
	for (int i = 0; i < count; i++) {
		strings.push_back("key_" + itos(i));
		names.push_back(StringName("name_" + itos(i)));
		ints.push_back(int64_t(i) << 32); // Differences in the high bits only.
		pointers.push_back(&targets[i]); // Aligned: low bits always zero.
	}
	check_key_type(strings);
	check_key_type(names);
	check_key_type(ints);
	check_key_type(pointers);
}

} // namespace TestForkHashContracts
