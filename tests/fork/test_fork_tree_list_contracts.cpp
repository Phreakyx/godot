/**************************************************************************/
/*  test_fork_tree_list_contracts.cpp                                     */
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

TEST_FORCE_LINK(test_fork_tree_list_contracts)

#include "core/templates/list.h"
#include "core/templates/rb_map.h"
#include "core/templates/rb_set.h"
#include "core/templates/vector.h"

// Contracts that every RBMap, RBSet and List backend must keep. They pin today's behavior.

namespace TestForkTreeListContracts {

struct Greater {
	bool operator()(int p_a, int p_b) const { return p_a > p_b; }
};

// Keys in the same group of ten compare equal.
struct TensLess {
	bool operator()(int p_a, int p_b) const { return p_a / 10 < p_b / 10; }
};

struct Tagged {
	int key = 0;
	int tag = 0;
	bool operator<(const Tagged &p_other) const { return key < p_other.key; }
};

struct TaggedGreater {
	bool operator()(const Tagged &p_a, const Tagged &p_b) const { return p_a.key > p_b.key; }
};

template <typename M>
static Vector<int> map_keys(const M &p_map) {
	Vector<int> keys;
	for (const typename M::Element *E = p_map.front(); E; E = E->next()) {
		keys.push_back(E->key());
	}
	return keys;
}

template <typename M>
static Vector<int> map_keys_backward(const M &p_map) {
	Vector<int> keys;
	for (const typename M::Element *E = p_map.back(); E; E = E->prev()) {
		keys.push_back(E->key());
	}
	return keys;
}

template <typename S>
static Vector<int> set_values(const S &p_set) {
	Vector<int> values;
	for (const typename S::Element *E = p_set.front(); E; E = E->next()) {
		values.push_back(E->get());
	}
	return values;
}

static Vector<int> list_values(const List<int> &p_list) {
	Vector<int> values;
	for (const List<int>::Element *E = p_list.front(); E; E = E->next()) {
		values.push_back(E->get());
	}
	return values;
}

static Vector<int> tags(const List<Tagged> &p_list) {
	Vector<int> values;
	for (const List<Tagged>::Element *E = p_list.front(); E; E = E->next()) {
		values.push_back(E->get().tag);
	}
	return values;
}

TEST_CASE("[Fork][RBMap] Iteration follows the comparator") {
	RBMap<int, int> map;
	map.insert(5, 0);
	map.insert(1, 0);
	map.insert(3, 0);
	CHECK(map_keys(map) == Vector<int>{ 1, 3, 5 });

	RBMap<int, int, Greater> reversed;
	reversed.insert(5, 0);
	reversed.insert(1, 0);
	reversed.insert(3, 0);
	CHECK(map_keys(reversed) == Vector<int>{ 5, 3, 1 });
	Vector<int> iterated;
	for (const KeyValue<int, int> &E : reversed) {
		iterated.push_back(E.key);
	}
	CHECK(iterated == Vector<int>{ 5, 3, 1 });
}

TEST_CASE("[Fork][RBMap] next() and prev() return nullptr past both ends") {
	RBMap<int, int> map;
	map.insert(3, 0);
	map.insert(1, 0);
	map.insert(2, 0);
	CHECK(map.front()->key() == 1);
	CHECK(map.back()->key() == 3);
	CHECK(map.front()->prev() == nullptr);
	CHECK(map.back()->next() == nullptr);
	CHECK(map_keys_backward(map) == Vector<int>{ 3, 2, 1 });
}

TEST_CASE("[Fork][RBMap] Element pointers survive inserts and erases of other elements") {
	RBMap<int, int> map;
	RBMap<int, int>::Element *element = map.insert(500, 1);
	for (int i = 0; i < 1000; i++) {
		if (i != 500) {
			map.insert(i, i);
		}
	}
	for (int i = 0; i < 1000; i += 2) {
		if (i != 500) {
			map.erase(i);
		}
	}
	CHECK(map.find(500) == element);
	CHECK(element->value() == 1);
	CHECK(element->prev()->key() == 499);
	CHECK(element->next()->key() == 501);
	CHECK(map.size() == 501);
}

TEST_CASE("[Fork][RBMap] find_closest returns the greatest key not above the key") {
	RBMap<int, int> map;
	map.insert(10, 0);
	map.insert(20, 0);
	map.insert(30, 0);
	CHECK(map.find_closest(5) == nullptr);
	CHECK(map.find_closest(15)->key() == 10);
	CHECK(map.find_closest(20)->key() == 20);
	CHECK(map.find_closest(35)->key() == 30);
}

TEST_CASE("[Fork][RBMap] lower_bound returns the first key not below the key") {
	RBMap<int, int> map;
	map.insert(10, 0);
	map.insert(20, 0);
	map.insert(30, 0);
	CHECK(map.lower_bound(5)->key() == 10);
	CHECK(map.lower_bound(15)->key() == 20);
	CHECK(map.lower_bound(20)->key() == 20);
	CHECK(map.lower_bound(35) == nullptr);
}

TEST_CASE("[Fork][RBMap] Inserting an equal key returns the existing element") {
	RBMap<int, int> map;
	RBMap<int, int>::Element *element = map.insert(1, 10);
	CHECK(map.insert(1, 20) == element);
	CHECK(map.size() == 1);
	CHECK(element->value() == 20);

	// Equal under the comparator is enough: the stored key stays, the value is replaced.
	RBMap<int, int, TensLess> grouped;
	RBMap<int, int, TensLess>::Element *first = grouped.insert(11, 1);
	CHECK(grouped.insert(15, 2) == first);
	CHECK(grouped.size() == 1);
	CHECK(first->key() == 11);
	CHECK(first->value() == 2);
}

TEST_CASE("[Fork][RBMap] Empty map") {
	RBMap<int, int> map;
	CHECK(map.is_empty());
	CHECK(map.find(1) == nullptr);
	CHECK(map.front() == nullptr);
	CHECK(map.back() == nullptr);
	CHECK(map.find_closest(1) == nullptr);
	CHECK(map.lower_bound(1) == nullptr);
	CHECK_FALSE(map.erase(1));
	CHECK(map.begin() == map.end());

	map.erase(map.insert(1, 10));
	CHECK(map.is_empty());
	CHECK(map.front() == nullptr);
	CHECK(map.back() == nullptr);
	CHECK(map.find(1) == nullptr);
	CHECK(map.find_closest(1) == nullptr);
	CHECK(map.lower_bound(1) == nullptr);
}

TEST_CASE("[Fork][RBSet] Iteration follows the comparator") {
	RBSet<int> set;
	set.insert(5);
	set.insert(1);
	set.insert(3);
	CHECK(set_values(set) == Vector<int>{ 1, 3, 5 });

	RBSet<int, Greater> reversed;
	reversed.insert(5);
	reversed.insert(1);
	reversed.insert(3);
	CHECK(set_values(reversed) == Vector<int>{ 5, 3, 1 });
	CHECK(reversed.front()->prev() == nullptr);
	CHECK(reversed.back()->next() == nullptr);
}

TEST_CASE("[Fork][RBSet] Element pointers survive inserts and erases of other elements") {
	RBSet<int> set;
	RBSet<int>::Element *element = set.insert(500);
	for (int i = 0; i < 1000; i++) {
		set.insert(i);
	}
	for (int i = 0; i < 1000; i += 2) {
		if (i != 500) {
			set.erase(i);
		}
	}
	CHECK(set.find(500) == element);
	CHECK(element->prev()->get() == 499);
	CHECK(element->next()->get() == 501);
	CHECK(set.size() == 501);
}

TEST_CASE("[Fork][RBSet] lower_bound returns the first value not below the value") {
	RBSet<int> set;
	set.insert(10);
	set.insert(20);
	set.insert(30);
	CHECK(set.lower_bound(5)->get() == 10);
	CHECK(set.lower_bound(15)->get() == 20);
	CHECK(set.lower_bound(20)->get() == 20);
	CHECK(set.lower_bound(35) == nullptr);
}

TEST_CASE("[Fork][RBSet] Inserting an equal value returns the existing element") {
	RBSet<int> set;
	RBSet<int>::Element *element = set.insert(1);
	CHECK(set.insert(1) == element);
	CHECK(set.size() == 1);

	RBSet<int, TensLess> grouped;
	RBSet<int, TensLess>::Element *first = grouped.insert(11);
	CHECK(grouped.insert(15) == first);
	CHECK(grouped.size() == 1);
	CHECK(first->get() == 11);
}

TEST_CASE("[Fork][RBSet] Empty set") {
	RBSet<int> set;
	CHECK(set.is_empty());
	CHECK(set.find(1) == nullptr);
	CHECK(set.front() == nullptr);
	CHECK(set.back() == nullptr);
	CHECK(set.lower_bound(1) == nullptr);
	CHECK_FALSE(set.erase(1));

	set.erase(set.insert(1));
	CHECK(set.is_empty());
	CHECK(set.front() == nullptr);
	CHECK(set.lower_bound(1) == nullptr);
}

TEST_CASE("[Fork][List] Erase of the current element during iteration") {
	List<int> list;
	for (int i = 1; i <= 6; i++) {
		list.push_back(i);
	}
	Vector<int> visited;
	for (List<int>::Element *E = list.front(); E;) {
		List<int>::Element *next = E->next();
		visited.push_back(E->get());
		if (E->get() % 2 == 0) {
			if (E->get() == 2) {
				E->erase();
			} else {
				list.erase(E);
			}
		}
		E = next;
	}
	CHECK(visited == Vector<int>{ 1, 2, 3, 4, 5, 6 });
	CHECK(list_values(list) == Vector<int>{ 1, 3, 5 });
	CHECK(list.size() == 3);
	CHECK(list.back()->get() == 5);
}

TEST_CASE("[Fork][List] insert_before and insert_after") {
	List<int> list;
	List<int>::Element *one = list.push_back(1);
	List<int>::Element *three = list.push_back(3);
	list.insert_before(three, 2);
	list.insert_after(three, 4);
	List<int>::Element *zero = list.insert_before(one, 0);
	CHECK(list.front() == zero);
	CHECK(list_values(list) == Vector<int>{ 0, 1, 2, 3, 4 });
	CHECK(list.back()->get() == 4);

	// A null position appends, for insert_before as well as insert_after.
	list.insert_after(nullptr, 5);
	list.insert_before(nullptr, 6);
	CHECK(list_values(list) == Vector<int>{ 0, 1, 2, 3, 4, 5, 6 });
	CHECK(list.size() == 7);
}

TEST_CASE("[Fork][List] Duplicates are kept") {
	List<int> list;
	List<int>::Element *first = list.push_back(1);
	List<int>::Element *second = list.push_back(1);
	list.push_back(2);
	CHECK(list.size() == 3);
	CHECK(list.find(1) == first);
	CHECK(list.erase(1)); // Erases the first match only.
	CHECK(list_values(list) == Vector<int>{ 1, 2 });
	CHECK(list.front() == second);
}

TEST_CASE("[Fork][List] Element pointers survive inserts and erases of other elements") {
	List<int> list;
	List<int>::Element *element = list.push_back(500);
	for (int i = 0; i < 1000; i++) {
		list.push_front(i);
		list.push_back(i);
	}
	for (List<int>::Element *E = list.front(); E;) {
		List<int>::Element *next = E->next();
		if (E != element && E->get() % 2 == 0) {
			list.erase(E);
		}
		E = next;
	}
	CHECK(element->get() == 500);
	CHECK(element->prev()->get() == 1);
	CHECK(element->next()->get() == 1);
	CHECK(list.size() == 1001);
}

TEST_CASE("[Fork][List] sort keeps the order of equal elements") {
	List<Tagged> list;
	list.push_back({ 2, 0 });
	list.push_back({ 1, 1 });
	list.push_back({ 2, 2 });
	list.push_back({ 1, 3 });
	list.sort();
	CHECK(tags(list) == Vector<int>{ 1, 3, 0, 2 });
	CHECK(list.back()->get().tag == 2);

	list.sort_custom<TaggedGreater>();
	CHECK(tags(list) == Vector<int>{ 0, 2, 1, 3 });
	CHECK(list.front()->prev() == nullptr);
	CHECK(list.back()->next() == nullptr);
}

TEST_CASE("[Fork][List] Empty list") {
	List<int> list;
	CHECK(list.is_empty());
	CHECK(list.front() == nullptr);
	CHECK(list.back() == nullptr);
	CHECK(list.find(1) == nullptr);
	CHECK_FALSE(list.erase(1));
	CHECK(list.begin() == list.end());

	list.erase(list.push_back(1));
	CHECK(list.is_empty());
	CHECK(list.front() == nullptr);
	CHECK(list.back() == nullptr);
}

} // namespace TestForkTreeListContracts
