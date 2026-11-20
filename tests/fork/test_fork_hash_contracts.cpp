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

} // namespace TestForkHashContracts
