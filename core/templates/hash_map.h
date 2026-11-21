/**************************************************************************/
/*  hash_map.h                                                            */
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

#include "core/os/memory.h"
#include "core/string/print_string.h" // IWYU pragma: keep. `WARN_VERBOSE` macro.
#include "core/templates/fork_backends.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/pair.h"
#include "core/templates/sort_list.h"

#include <initializer_list>
#if FORK_BACKEND_HASHMAP_POOL
#include <bit>
#endif

template <typename TKey, typename TValue>
struct HashMapElement {
	HashMapElement *next = nullptr;
	HashMapElement *prev = nullptr;
	KeyValue<TKey, TValue> data;
	HashMapElement() {}
	HashMapElement(const TKey &p_key, const TValue &p_value) :
			data(p_key, p_value) {}
};

/**
 * Key-value container (aka hash table or dictionary) using robin-hood hashing.
 *
 * Key-values are pointer-stable across HashMap mutations.
 * Remembers insertion order and iterates by it.
 *
 * Core container guidance:
 * https://docs.godotengine.org/en/latest/engine_details/architecture/core_types.html#containers
 */
template <typename TKey, typename TValue,
		typename Hasher = HashMapHasherDefault,
		typename Comparator = HashMapComparatorDefault<TKey>,
		typename Allocator = DefaultTypedAllocator<HashMapElement<TKey, TValue>>>
class _WARN_UNUSED_ HashMap : private Allocator {
public:
	static constexpr uint32_t MIN_CAPACITY_INDEX = 2; // Use a prime.
	static constexpr float MAX_OCCUPANCY = 0.75;
	static constexpr uint32_t EMPTY_HASH = 0;
	using KV = KeyValue<TKey, TValue>; // Type alias for easier access to KeyValue.

private:
	HashMapElement<TKey, TValue> **_elements = nullptr;
	uint32_t *_hashes = nullptr;
	HashMapElement<TKey, TValue> *_head_element = nullptr;
	HashMapElement<TKey, TValue> *_tail_element = nullptr;

	uint32_t _capacity_idx = 0;
	uint32_t _size = 0;

	// Fork: node storage (FORK_BACKEND_HASHMAP_POOL, see fork_backends.h and
	// kb/project/hashmap-node-pool.md). With 0, every node is one Allocator allocation (upstream).
#if FORK_BACKEND_HASHMAP_POOL
	// With 1 and the default Allocator, a per-map pool. Page p holds (1 << _pool_first_shift) << p
	// nodes until a page reaches about 64 KiB of nodes; every later page has that size. Pages never
	// move, so element pointers stay valid. Node i is the i-th node the bump pointer handed out
	// (its stable index, decoded by _pool_locate()). A freed node is destroyed and its first word
	// (the `next` field) links the free list, which is taken first. A move takes the pool with the
	// arrays; clear() and the destructor release every page.
	using _PoolNode = HashMapElement<TKey, TValue>;
	static constexpr bool _USE_POOL = std::is_same_v<Allocator, DefaultTypedAllocator<_PoolNode>>;

	// Pages 0 and 1 are kept in the map, so a map of up to three nodes (with a one-node page 0)
	// allocates no page table. From page 2 on, _pool_store is the table of every page.
	void *_pool_store = nullptr; // Page 0 while there are at most two pages, else the page table.
	_PoolNode *_pool_page1 = nullptr; // Page 1 while there are two pages.
	_PoolNode *_pool_free = nullptr;
	uint32_t _pool_next = 0; // Nodes handed out by the bump pointer.
	uint8_t _pool_first_shift = 0; // Page 0 holds 1 << _pool_first_shift nodes.

	// A function, not a constant, so TValue may still be incomplete where the map is declared.
	static constexpr uint32_t _pool_cap_shift() {
		constexpr size_t nodes = size_t(65536) / sizeof(_PoolNode);
		return nodes > 1 ? uint32_t(std::bit_width(nodes)) - 1 : 0;
	}

	// Page p holds 1 << _pool_page_shift(p) nodes.
	_FORCE_INLINE_ uint32_t _pool_page_shift(uint32_t p_page) const {
		const uint32_t first = _pool_first_shift;
		const uint32_t cap = MAX(_pool_cap_shift(), first);
		return p_page < cap - first ? first + p_page : cap;
	}

	// Page and offset of node p_index: one bit scan in the growing pages, shifts past the cap.
	_FORCE_INLINE_ void _pool_locate(uint32_t p_index, uint32_t &r_page, uint32_t &r_offset) const {
		const uint32_t first = _pool_first_shift;
		const uint32_t cap = MAX(_pool_cap_shift(), first);
		const uint32_t growing_pages = cap - first;
		const uint32_t growing_nodes = ((1u << growing_pages) - 1) << first;
		if (likely(p_index < growing_nodes)) {
			r_page = uint32_t(std::bit_width((p_index >> first) + 1)) - 1;
			r_offset = p_index - (((1u << r_page) - 1) << first);
		} else {
			const uint32_t rest = p_index - growing_nodes;
			r_page = growing_pages + (rest >> cap);
			r_offset = rest & ((1u << cap) - 1);
		}
	}

	// True while the pool has at most two pages (no page table).
	_FORCE_INLINE_ bool _pool_inline() const {
		return _pool_next <= (uint64_t(1) << _pool_page_shift(0)) + (uint64_t(1) << _pool_page_shift(1));
	}

	_FORCE_INLINE_ _PoolNode *_pool_page(uint32_t p_page) const {
		if (_pool_inline()) {
			return p_page == 0 ? static_cast<_PoolNode *>(_pool_store) : _pool_page1;
		}
		return static_cast<_PoolNode **>(_pool_store)[p_page];
	}

	// The node with stable index p_index (for an index-based table, HASH-02).
	_FORCE_INLINE_ _PoolNode *_pool_node(uint32_t p_index) const {
		DEV_ASSERT(p_index < _pool_next);
		uint32_t page = 0;
		uint32_t offset = 0;
		_pool_locate(p_index, page, offset);
		return _pool_page(page) + offset;
	}

	_PoolNode *_pool_add_page(uint32_t p_page) {
		_PoolNode *page = static_cast<_PoolNode *>(Memory::alloc_static(sizeof(_PoolNode) << _pool_page_shift(p_page)));
		if (p_page == 0) {
			_pool_store = page;
		} else if (p_page == 1) {
			_pool_page1 = page;
		} else {
			_PoolNode **table = static_cast<_PoolNode **>(_pool_store);
			if (p_page == 2) {
				table = static_cast<_PoolNode **>(Memory::alloc_static(sizeof(_PoolNode *) * 4));
				table[0] = static_cast<_PoolNode *>(_pool_store);
				table[1] = _pool_page1;
				_pool_page1 = nullptr;
			} else if ((p_page & (p_page - 1)) == 0) {
				table = static_cast<_PoolNode **>(Memory::realloc_static(table, sizeof(_PoolNode *) * p_page * 2));
			}
			table[p_page] = page;
			_pool_store = table;
		}
		return page;
	}

	_FORCE_INLINE_ _PoolNode *_pool_alloc() {
		_PoolNode *node = _pool_free;
		if (node != nullptr) {
			_pool_free = *reinterpret_cast<_PoolNode **>(node);
			return node;
		}
		uint32_t page = 0;
		uint32_t offset = 0;
		_pool_locate(_pool_next, page, offset);
		_pool_next++;
		node = offset == 0 ? _pool_add_page(page) : _pool_page(page) + offset;
		DEV_ASSERT(_pool_node(_pool_next - 1) == node);
		return node;
	}

	// Releases every page; the nodes must be destroyed already.
	void _pool_reset() {
		if (_pool_next == 0) {
			return;
		}
		uint32_t last = 0;
		uint32_t offset = 0;
		_pool_locate(_pool_next - 1, last, offset);
		if (last < 2) {
			Memory::free_static(_pool_store);
			if (last == 1) {
				Memory::free_static(_pool_page1);
			}
		} else {
			_PoolNode **table = static_cast<_PoolNode **>(_pool_store);
			for (uint32_t i = 0; i <= last; i++) {
				Memory::free_static(table[i]);
			}
			Memory::free_static(table);
		}
		_pool_store = nullptr;
		_pool_page1 = nullptr;
		_pool_free = nullptr;
		_pool_next = 0;
	}

	void _pool_take(HashMap &p_other) {
		_pool_store = p_other._pool_store;
		_pool_page1 = p_other._pool_page1;
		_pool_free = p_other._pool_free;
		_pool_next = p_other._pool_next;
		_pool_first_shift = p_other._pool_first_shift;
		p_other._pool_store = nullptr;
		p_other._pool_page1 = nullptr;
		p_other._pool_free = nullptr;
		p_other._pool_next = 0;
		p_other._pool_first_shift = 0;
	}

	// Sizes page 0 for p_count nodes (the next power of two), while the pool is empty.
	void _pool_first_page(uint32_t p_count) {
		if (_pool_next == 0) {
			_pool_first_shift = p_count > 1 ? uint8_t(MIN(uint32_t(std::bit_width(p_count - 1)), 31u)) : 0;
		}
	}

	_FORCE_INLINE_ _PoolNode *_element_new(const TKey &p_key, const TValue &p_value) {
		if constexpr (_USE_POOL) {
			return memnew_placement(_pool_alloc(), _PoolNode(p_key, p_value));
		} else {
			return Allocator::new_allocation(_PoolNode(p_key, p_value));
		}
	}

	_FORCE_INLINE_ void _element_delete(_PoolNode *p_element) {
		if constexpr (_USE_POOL) {
			p_element->~_PoolNode();
			DEV_ASSERT(p_element != _pool_free);
			*reinterpret_cast<_PoolNode **>(p_element) = _pool_free;
			_pool_free = p_element;
		} else {
			Allocator::delete_allocation(p_element);
		}
	}
#else
	static constexpr bool _USE_POOL = false;
	_FORCE_INLINE_ void _pool_reset() {}
	_FORCE_INLINE_ void _pool_take(HashMap &) {}
	_FORCE_INLINE_ void _pool_first_page(uint32_t) {}
	_FORCE_INLINE_ HashMapElement<TKey, TValue> *_element_new(const TKey &p_key, const TValue &p_value) {
		return Allocator::new_allocation(HashMapElement<TKey, TValue>(p_key, p_value));
	}
	_FORCE_INLINE_ void _element_delete(HashMapElement<TKey, TValue> *p_element) {
		Allocator::delete_allocation(p_element);
	}
#endif

	_FORCE_INLINE_ static uint32_t _hash(const TKey &p_key) {
		uint32_t hash = hash_table_key<Hasher>(p_key);

		if (unlikely(hash == EMPTY_HASH)) {
			hash = EMPTY_HASH + 1;
		}

		return hash;
	}

	_FORCE_INLINE_ static constexpr void _increment_mod(uint32_t &r_idx, const uint32_t p_capacity) {
		r_idx++;
		// `if` is faster than both fastmod and mod.
		if (unlikely(r_idx == p_capacity)) {
			r_idx = 0;
		}
	}

	// Returns true when the probe length from `a` to `idx` is LESS THAN the probe length from `b` to `idx`.
	_FORCE_INLINE_ static constexpr bool _probe_length_cmp(const uint32_t p_a, const uint32_t p_b, const uint32_t p_idx) {
		if (unlikely(p_idx < p_b)) {
			return likely(p_idx >= p_a) || p_b < p_a;
		}
		return p_b < p_a && likely(p_idx >= p_a);
	}

	bool _lookup_idx(const TKey &p_key, uint32_t &r_idx) const {
		return _elements != nullptr && _size > 0 && _lookup_idx_unchecked(p_key, _hash(p_key), r_idx);
	}

	/// Note: Assumes that _elements != nullptr
	bool _lookup_idx_unchecked(const TKey &p_key, uint32_t p_hash, uint32_t &r_idx) const {
		const uint32_t capacity = hash_table_size_primes[_capacity_idx];
		const uint64_t capacity_inv = hash_table_size_primes_inv[_capacity_idx];
		const uint32_t start_idx = fastmod(p_hash, capacity_inv, capacity);
		uint32_t idx = start_idx;

		if (_hashes[idx] == EMPTY_HASH) {
			return false;
		}
		if (_hashes[idx] == p_hash && Comparator::compare(_elements[idx]->data.key, p_key)) {
			r_idx = idx;
			return true;
		}
		while (true) {
			_increment_mod(idx, capacity);
			if (_hashes[idx] == EMPTY_HASH) {
				return false;
			}
			if (_hashes[idx] != p_hash) {
				// Stop search if we probed further than this element.
				if (_probe_length_cmp(fastmod(_hashes[idx], capacity_inv, capacity), start_idx, idx)) {
					return false;
				}
			} else if (Comparator::compare(_elements[idx]->data.key, p_key)) {
				r_idx = idx;
				return true;
			}
		}
	}

	// Small helper for _insert_element
	_FORCE_INLINE_ void _insert_empty_at(const uint32_t p_idx, const uint32_t p_hash, HashMapElement<TKey, TValue> *p_value) {
		_elements[p_idx] = p_value;
		_hashes[p_idx] = p_hash;

		_size++;
	}

	void _insert_element(uint32_t p_hash, HashMapElement<TKey, TValue> *p_value) {
		const uint32_t capacity = hash_table_size_primes[_capacity_idx];
		const uint64_t capacity_inv = hash_table_size_primes_inv[_capacity_idx];
		uint32_t hash = p_hash;
		HashMapElement<TKey, TValue> *value = p_value;

		uint32_t start_idx = fastmod(hash, capacity_inv, capacity);
		uint32_t idx = start_idx;

		if (_hashes[idx] == EMPTY_HASH) {
			_insert_empty_at(idx, hash, value);
			return;
		}

		// Keep going until empty slot found.
		while (true) {
			_increment_mod(idx, capacity);
			if (_hashes[idx] == EMPTY_HASH) {
				_insert_empty_at(idx, hash, value);
				return;
			}

			// Not an empty slot, let's check the probing length of the existing one.
			uint32_t new_start_idx = fastmod(_hashes[idx], capacity_inv, capacity);
			if (_probe_length_cmp(new_start_idx, start_idx, idx)) {
				SWAP(hash, _hashes[idx]);
				SWAP(value, _elements[idx]);
				start_idx = new_start_idx;
			}
		}
	}

	void _resize_and_rehash(uint32_t p_new_capacity_idx) {
		uint32_t old_capacity = hash_table_size_primes[_capacity_idx];

		// Capacity can't be 0.
		_capacity_idx = MAX((uint32_t)MIN_CAPACITY_INDEX, p_new_capacity_idx);

		uint32_t capacity = hash_table_size_primes[_capacity_idx];

		HashMapElement<TKey, TValue> **old_elements = _elements;
		uint32_t *old_hashes = _hashes;

		_size = 0;
		static_assert(EMPTY_HASH == 0, "Assuming EMPTY_HASH = 0 for alloc_static_zeroed call");

		_hashes = reinterpret_cast<uint32_t *>(Memory::alloc_static_zeroed(sizeof(uint32_t) * capacity));
		_elements = reinterpret_cast<HashMapElement<TKey, TValue> **>(Memory::alloc_static(sizeof(HashMapElement<TKey, TValue> *) * capacity));

		if (old_capacity == 0) {
			// Nothing to do.
			return;
		}

		for (uint32_t i = 0; i < old_capacity; i++) {
			if (old_hashes[i] == EMPTY_HASH) {
				continue;
			}

			_insert_element(old_hashes[i], old_elements[i]);
		}

		Memory::free_static(old_elements);
		Memory::free_static(old_hashes);
	}

	_FORCE_INLINE_ HashMapElement<TKey, TValue> *_insert(const TKey &p_key, const TValue &p_value, uint32_t p_hash, bool p_front_insert = false) {
		uint32_t capacity = hash_table_size_primes[_capacity_idx];
		if (unlikely(_elements == nullptr)) {
			// Allocate on demand to save memory.

			static_assert(EMPTY_HASH == 0, "Assuming EMPTY_HASH = 0 for alloc_static_zeroed call");
			_hashes = reinterpret_cast<uint32_t *>(Memory::alloc_static_zeroed(sizeof(uint32_t) * capacity));
			_elements = reinterpret_cast<HashMapElement<TKey, TValue> **>(Memory::alloc_static(sizeof(HashMapElement<TKey, TValue> *) * capacity));
		}

		if (_size + 1 > MAX_OCCUPANCY * capacity) {
			ERR_FAIL_COND_V_MSG(_capacity_idx + 1 == HASH_TABLE_SIZE_MAX, nullptr, "Hash table maximum capacity reached, aborting insertion.");
			_resize_and_rehash(_capacity_idx + 1);
		}

		HashMapElement<TKey, TValue> *elem = _element_new(p_key, p_value);

		if (_tail_element == nullptr) {
			_head_element = elem;
			_tail_element = elem;
		} else if (p_front_insert) {
			_head_element->prev = elem;
			elem->next = _head_element;
			_head_element = elem;
		} else {
			_tail_element->next = elem;
			elem->prev = _tail_element;
			_tail_element = elem;
		}

		_insert_element(p_hash, elem);
		return elem;
	}

	void _clear_data() {
		if constexpr (_USE_POOL) {
			// Fork: destroy the nodes, then release the pages at once (no free list).
			if constexpr (!std::is_trivially_destructible_v<HashMapElement<TKey, TValue>>) {
				HashMapElement<TKey, TValue> *current = _tail_element;
				while (current != nullptr) {
					HashMapElement<TKey, TValue> *prev = current->prev;
					current->~HashMapElement<TKey, TValue>();
					current = prev;
				}
			}
			_pool_reset();
		} else {
			HashMapElement<TKey, TValue> *current = _tail_element;
			while (current != nullptr) {
				HashMapElement<TKey, TValue> *prev = current->prev;
				Allocator::delete_allocation(current);
				current = prev;
			}
		}
	}

public:
	_FORCE_INLINE_ uint32_t get_capacity() const { return hash_table_size_primes[_capacity_idx]; }
	_FORCE_INLINE_ uint32_t size() const { return _size; }

	/* Standard Godot Container API */

	bool is_empty() const {
		return _size == 0;
	}

	void clear() {
		if (_elements == nullptr || _size == 0) {
			_pool_reset(); // Fork: the nodes of erased elements.
			return;
		}

		_clear_data();
		memset(_hashes, EMPTY_HASH, get_capacity() * sizeof(uint32_t));

		_tail_element = nullptr;
		_head_element = nullptr;
		_size = 0;
	}

	void sort() {
		sort_custom<KeyValueSort<TKey, TValue>>();
	}

	template <typename C>
	void sort_custom() {
		if (size() < 2) {
			return;
		}

		using E = HashMapElement<TKey, TValue>;
		SortList<E, KeyValue<TKey, TValue>, &E::data, &E::prev, &E::next, C> sorter;
		sorter.sort(_head_element, _tail_element);
	}

	TValue &get(const TKey &p_key) _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);
		CRASH_COND_MSG(!exists, "HashMap key not found.");
		return _elements[idx]->data.value;
	}

	const TValue &get(const TKey &p_key) const _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);
		CRASH_COND_MSG(!exists, "HashMap key not found.");
		return _elements[idx]->data.value;
	}

	const TValue *getptr(const TKey &p_key) const _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);

		if (exists) {
			return &_elements[idx]->data.value;
		}
		return nullptr;
	}

	TValue *getptr(const TKey &p_key) _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);

		if (exists) {
			return &_elements[idx]->data.value;
		}
		return nullptr;
	}

	_FORCE_INLINE_ bool has(const TKey &p_key) const {
		uint32_t _idx = 0;
		return _lookup_idx(p_key, _idx);
	}

	bool erase(const TKey &p_key) {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);

		if (!exists) {
			return false;
		}

		const uint32_t capacity = hash_table_size_primes[_capacity_idx];
		const uint64_t capacity_inv = hash_table_size_primes_inv[_capacity_idx];

		uint32_t next_idx = idx;
		_increment_mod(next_idx, capacity);
		while (_hashes[next_idx] != EMPTY_HASH && next_idx != fastmod(_hashes[next_idx], capacity_inv, capacity)) {
			SWAP(_hashes[next_idx], _hashes[idx]);
			SWAP(_elements[next_idx], _elements[idx]);
			idx = next_idx;
			_increment_mod(next_idx, capacity);
		}

		_hashes[idx] = EMPTY_HASH;

		if (_head_element == _elements[idx]) {
			_head_element = _elements[idx]->next;
		}

		if (_tail_element == _elements[idx]) {
			_tail_element = _elements[idx]->prev;
		}

		if (_elements[idx]->prev) {
			_elements[idx]->prev->next = _elements[idx]->next;
		}

		if (_elements[idx]->next) {
			_elements[idx]->next->prev = _elements[idx]->prev;
		}

		_element_delete(_elements[idx]);

		_size--;
		return true;
	}

	// Replace the key of an entry in-place, without invalidating iterators or changing the entries position during iteration.
	// p_old_key must exist in the map and p_new_key must not, unless it is equal to p_old_key.
	bool replace_key(const TKey &p_old_key, const TKey &p_new_key) {
		ERR_FAIL_COND_V(_elements == nullptr || _size == 0, false);
		if (Comparator::compare(p_old_key, p_new_key)) {
			return true;
		}
		const uint32_t new_hash = _hash(p_new_key);
		uint32_t idx = 0;
		ERR_FAIL_COND_V(_lookup_idx_unchecked(p_new_key, new_hash, idx), false);
		ERR_FAIL_COND_V(!_lookup_idx(p_old_key, idx), false);
		HashMapElement<TKey, TValue> *element = _elements[idx];

		// Delete the old entries in _hashes and _elements.
		const uint32_t capacity = hash_table_size_primes[_capacity_idx];
		const uint64_t capacity_inv = hash_table_size_primes_inv[_capacity_idx];

		uint32_t next_idx = idx;
		_increment_mod(next_idx, capacity);
		while (_hashes[next_idx] != EMPTY_HASH && next_idx != fastmod(_hashes[next_idx], capacity_inv, capacity)) {
			SWAP(_hashes[next_idx], _hashes[idx]);
			SWAP(_elements[next_idx], _elements[idx]);
			idx = next_idx;
			_increment_mod(next_idx, capacity);
		}

		_hashes[idx] = EMPTY_HASH;

		// _insert_element will increment this again.
		_size--;

		// Update the HashMapElement with the new key and reinsert it.
		const_cast<TKey &>(element->data.key) = p_new_key;
		_insert_element(new_hash, element);

		return true;
	}

	// Reserves space for a number of elements, useful to avoid many resizes and rehashes.
	// If adding a known (possibly large) number of elements at once, must be larger than old capacity.
	void reserve(uint32_t p_new_capacity) {
		_pool_first_page(p_new_capacity);
		uint32_t new_idx = _capacity_idx;

		while (hash_table_size_primes[new_idx] < p_new_capacity) {
			ERR_FAIL_COND_MSG(new_idx + 1 == (uint32_t)HASH_TABLE_SIZE_MAX, nullptr);
			new_idx++;
		}

		if (new_idx == _capacity_idx) {
			if (p_new_capacity < _size) {
				WARN_VERBOSE("reserve() called with a capacity smaller than the current size. This is likely a mistake.");
			}
			return;
		}

		if (_elements == nullptr) {
			_capacity_idx = new_idx;
			return; // Unallocated yet.
		}
		_resize_and_rehash(new_idx);
	}

	/** Iterator API **/

	struct ConstIterator {
		_FORCE_INLINE_ const KeyValue<TKey, TValue> &operator*() const {
			return E->data;
		}
		_FORCE_INLINE_ const KeyValue<TKey, TValue> *operator->() const { return &E->data; }
		_FORCE_INLINE_ ConstIterator &operator++() {
			if (E) {
				E = E->next;
			}
			return *this;
		}
		_FORCE_INLINE_ ConstIterator &operator--() {
			if (E) {
				E = E->prev;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const ConstIterator &p_other) const { return E == p_other.E; }
		_FORCE_INLINE_ bool operator!=(const ConstIterator &p_other) const { return E != p_other.E; }

		_FORCE_INLINE_ explicit operator bool() const {
			return E != nullptr;
		}

		_FORCE_INLINE_ ConstIterator(const HashMapElement<TKey, TValue> *p_element) { E = p_element; }
		_FORCE_INLINE_ ConstIterator() {}
		_FORCE_INLINE_ ConstIterator(const ConstIterator &p_it) { E = p_it.E; }
		_FORCE_INLINE_ void operator=(const ConstIterator &p_it) {
			E = p_it.E;
		}

	private:
		const HashMapElement<TKey, TValue> *E = nullptr;
	};

	struct Iterator {
		_FORCE_INLINE_ KeyValue<TKey, TValue> &operator*() const {
			return E->data;
		}
		_FORCE_INLINE_ KeyValue<TKey, TValue> *operator->() const { return &E->data; }
		_FORCE_INLINE_ Iterator &operator++() {
			if (E) {
				E = E->next;
			}
			return *this;
		}
		_FORCE_INLINE_ Iterator &operator--() {
			if (E) {
				E = E->prev;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const Iterator &p_other) const { return E == p_other.E; }
		_FORCE_INLINE_ bool operator!=(const Iterator &p_other) const { return E != p_other.E; }

		_FORCE_INLINE_ explicit operator bool() const {
			return E != nullptr;
		}

		_FORCE_INLINE_ Iterator(HashMapElement<TKey, TValue> *p_element) { E = p_element; }
		_FORCE_INLINE_ Iterator() {}
		_FORCE_INLINE_ Iterator(const Iterator &p_it) { E = p_it.E; }
		_FORCE_INLINE_ void operator=(const Iterator &p_it) {
			E = p_it.E;
		}

		operator ConstIterator() const {
			return ConstIterator(E);
		}

	private:
		HashMapElement<TKey, TValue> *E = nullptr;
	};

	_FORCE_INLINE_ Iterator begin() _LIFETIME_BOUND_ {
		return Iterator(_head_element);
	}
	_FORCE_INLINE_ Iterator end() _LIFETIME_BOUND_ {
		return Iterator(nullptr);
	}
	_FORCE_INLINE_ Iterator last() _LIFETIME_BOUND_ {
		return Iterator(_tail_element);
	}

	_FORCE_INLINE_ Iterator find(const TKey &p_key) _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);
		if (!exists) {
			return end();
		}
		return Iterator(_elements[idx]);
	}

	_FORCE_INLINE_ void remove(const Iterator &p_iter) {
		if (p_iter) {
			erase(p_iter->key);
		}
	}

	_FORCE_INLINE_ ConstIterator begin() const _LIFETIME_BOUND_ {
		return ConstIterator(_head_element);
	}
	_FORCE_INLINE_ ConstIterator end() const _LIFETIME_BOUND_ {
		return ConstIterator(nullptr);
	}
	_FORCE_INLINE_ ConstIterator last() const _LIFETIME_BOUND_ {
		return ConstIterator(_tail_element);
	}

	_FORCE_INLINE_ ConstIterator find(const TKey &p_key) const _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);
		if (!exists) {
			return end();
		}
		return ConstIterator(_elements[idx]);
	}

	/* Indexing */

	const TValue &operator[](const TKey &p_key) const _LIFETIME_BOUND_ {
		uint32_t idx = 0;
		bool exists = _lookup_idx(p_key, idx);
		CRASH_COND(!exists);
		return _elements[idx]->data.value;
	}

	TValue &operator[](const TKey &p_key) _LIFETIME_BOUND_ {
		const uint32_t hash = _hash(p_key);
		uint32_t idx = 0;
		bool exists = _elements && _size > 0 && _lookup_idx_unchecked(p_key, hash, idx);
		if (!exists) {
			return _insert(p_key, TValue(), hash)->data.value;
		} else {
			return _elements[idx]->data.value;
		}
	}

	/* Insert */

	Iterator insert(const TKey &p_key, const TValue &p_value, bool p_front_insert = false) {
		const uint32_t hash = _hash(p_key);
		uint32_t idx = 0;
		bool exists = _elements && _size > 0 && _lookup_idx_unchecked(p_key, hash, idx);
		if (!exists) {
			return Iterator(_insert(p_key, p_value, hash, p_front_insert));
		} else {
			_elements[idx]->data.value = p_value;
			return Iterator(_elements[idx]);
		}
	}

	/* Constructors */

	explicit HashMap(const HashMap &p_other) {
		reserve(hash_table_size_primes[p_other._capacity_idx]);
		_pool_first_page(p_other._size);

		if (p_other._size == 0) {
			return;
		}

		for (const KeyValue<TKey, TValue> &E : p_other) {
			insert(E.key, E.value);
		}
	}

	HashMap(HashMap &&p_other) {
		_elements = p_other._elements;
		_hashes = p_other._hashes;
		_head_element = p_other._head_element;
		_tail_element = p_other._tail_element;
		_capacity_idx = p_other._capacity_idx;
		_size = p_other._size;

		p_other._elements = nullptr;
		p_other._hashes = nullptr;
		p_other._head_element = nullptr;
		p_other._tail_element = nullptr;
		p_other._capacity_idx = MIN_CAPACITY_INDEX;
		p_other._size = 0;
		_pool_take(p_other);
	}

	void operator=(const HashMap &p_other) {
		if (this == &p_other) {
			return; // Ignore self assignment.
		}
		if (_size != 0) {
			clear();
		}
		_pool_reset();

		reserve(hash_table_size_primes[p_other._capacity_idx]);
		_pool_first_page(p_other._size);

		if (p_other._elements == nullptr) {
			return; // Nothing to copy.
		}

		for (const KeyValue<TKey, TValue> &E : p_other) {
			insert(E.key, E.value);
		}
	}

	HashMap &operator=(HashMap &&p_other) {
		if (this == &p_other) {
			return *this;
		}

		if (_size != 0) {
			clear();
		}
		_pool_reset();
		if (_elements != nullptr) {
			Memory::free_static(_elements);
			Memory::free_static(_hashes);
		}

		_elements = p_other._elements;
		_hashes = p_other._hashes;
		_head_element = p_other._head_element;
		_tail_element = p_other._tail_element;
		_capacity_idx = p_other._capacity_idx;
		_size = p_other._size;

		p_other._elements = nullptr;
		p_other._hashes = nullptr;
		p_other._head_element = nullptr;
		p_other._tail_element = nullptr;
		p_other._capacity_idx = MIN_CAPACITY_INDEX;
		p_other._size = 0;
		_pool_take(p_other);

		return *this;
	}

	explicit HashMap(uint32_t p_initial_capacity) {
		// Capacity can't be 0.
		_capacity_idx = 0;
		reserve(p_initial_capacity);
	}
	HashMap() {
		_capacity_idx = MIN_CAPACITY_INDEX;
	}

	HashMap(std::initializer_list<KeyValue<TKey, TValue>> p_init) {
		reserve(p_init.size());
		for (const KeyValue<TKey, TValue> &E : p_init) {
			insert(E.key, E.value);
		}
	}

	uint32_t debug_get_hash(uint32_t p_idx) {
		if (_size == 0) {
			return 0;
		}
		ERR_FAIL_INDEX_V(p_idx, get_capacity(), 0);
		return _hashes[p_idx];
	}
	Iterator debug_get_element(uint32_t p_idx) {
		if (_size == 0) {
			return Iterator();
		}
		ERR_FAIL_INDEX_V(p_idx, get_capacity(), Iterator());
		return Iterator(_elements[p_idx]);
	}

	~HashMap() {
		_clear_data();

		if (_elements != nullptr) {
			Memory::free_static(_elements);
			Memory::free_static(_hashes);
		}
	}
};
