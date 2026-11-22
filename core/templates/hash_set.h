/**************************************************************************/
/*  hash_set.h                                                            */
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

/**
 * Set container using robin hood hashing.
 *
 * Elements are not pointer stable.
 * The element order is arbitrary.
 *
 * Elements are relocated by naive memory moves; they must not store their own address (see GH-100509).
 *
 * Core container guidance:
 * https://docs.godotengine.org/en/latest/engine_details/architecture/core_types.html#containers
 */
template <typename TKey,
		typename Hasher = HashMapHasherDefault,
		typename Comparator = HashMapComparatorDefault<TKey>>
class _WARN_UNUSED_ HashSet {
public:
	static constexpr uint32_t MIN_CAPACITY_INDEX = 2; // Use a prime.
	static constexpr float MAX_OCCUPANCY = 0.75;
	static constexpr uint32_t EMPTY_HASH = 0;

private:
	TKey *_keys = nullptr;
	uint32_t *_hash_idx_to_key_idx = nullptr;
#if FORK_BACKEND_HASH_TAGS
	// DELETED control bytes. The tag path keeps no key to slot index (erase finds a moved key's
	// slot by its hash), so this takes that pointer's place.
	uint32_t _deleted = 0;
#else
	uint32_t *_key_idx_to_hash_idx = nullptr;
#endif
	uint32_t *_hashes = nullptr;

	uint32_t _capacity_idx = 0;
	uint32_t _size = 0;

#if FORK_BACKEND_HASH_TAGS
	// Fork (FORK_BACKEND_HASH_TAGS): one allocation per table and resize (D013) holding the key
	// hashes, the key index of each slot, the capacity + HashTags::WIDTH control bytes, then the
	// keys at their alignment. A table holds three quarters of its capacity in keys (its growth
	// point). _hashes is the block.
	_FORCE_INLINE_ static constexpr uint32_t _key_capacity(uint32_t p_capacity) {
		return p_capacity - p_capacity / 4;
	}

	_FORCE_INLINE_ static size_t _keys_offset(uint32_t p_capacity) {
		static_assert(alignof(TKey) <= alignof(std::max_align_t), "HashSet keys cannot be over-aligned.");
		constexpr size_t keys_align = alignof(TKey) > alignof(uint32_t) ? alignof(TKey) : alignof(uint32_t);
		return (sizeof(uint32_t) * (size_t(_key_capacity(p_capacity)) + p_capacity) + p_capacity + HashTags::WIDTH + keys_align - 1) & ~(keys_align - 1);
	}

	// Points the arrays into a new block; control bytes EMPTY.
	void _allocate_block(uint32_t p_capacity) {
		uint8_t *block = reinterpret_cast<uint8_t *>(Memory::alloc_static(_keys_offset(p_capacity) + sizeof(TKey) * size_t(_key_capacity(p_capacity))));
		_hashes = reinterpret_cast<uint32_t *>(block);
		_hash_idx_to_key_idx = _hashes + _key_capacity(p_capacity);
		_keys = reinterpret_cast<TKey *>(block + _keys_offset(p_capacity));
		memset(_hash_idx_to_key_idx + p_capacity, HashTags::EMPTY, p_capacity + HashTags::WIDTH);
		_deleted = 0;
	}

	void _free_block() {
		Memory::free_static(_hashes);
		_keys = nullptr;
		_hashes = nullptr;
		_hash_idx_to_key_idx = nullptr;
	}
#else
	// The three index arrays and the keys share one allocation (one block per
	// table and resize instead of four): hashes, key to hash index, hash to key
	// index, then the keys at their alignment. _hashes is the block.
	// (In the functions, not the class: a HashSet member may name an incomplete key type.)
	_FORCE_INLINE_ static size_t _keys_offset(uint32_t p_capacity) {
		static_assert(alignof(TKey) <= alignof(std::max_align_t), "HashSet keys cannot be over-aligned.");
		constexpr size_t keys_align = alignof(TKey) > alignof(uint32_t) ? alignof(TKey) : alignof(uint32_t);
		return (sizeof(uint32_t) * 3 * size_t(p_capacity) + keys_align - 1) & ~(keys_align - 1);
	}

	// Points the four arrays into a new block; hashes zeroed (EMPTY_HASH).
	void _allocate_block(uint32_t p_capacity) {
		static_assert(EMPTY_HASH == 0, "Assuming EMPTY_HASH = 0 for the memset below.");
		uint8_t *block = reinterpret_cast<uint8_t *>(Memory::alloc_static(_keys_offset(p_capacity) + sizeof(TKey) * size_t(p_capacity)));
		_hashes = reinterpret_cast<uint32_t *>(block);
		_key_idx_to_hash_idx = _hashes + p_capacity;
		_hash_idx_to_key_idx = _key_idx_to_hash_idx + p_capacity;
		_keys = reinterpret_cast<TKey *>(block + _keys_offset(p_capacity));
		memset(_hashes, EMPTY_HASH, sizeof(uint32_t) * p_capacity);
	}

	void _free_block() {
		Memory::free_static(_hashes);
		_keys = nullptr;
		_hashes = nullptr;
		_hash_idx_to_key_idx = nullptr;
		_key_idx_to_hash_idx = nullptr;
	}
#endif // FORK_BACKEND_HASH_TAGS

	// Fork: slot arithmetic (FORK_BACKEND_HASH_POW2, see fork_backends.h and
	// kb/project/hash-pow2-mixer.md). With 0, prime capacities and fastmod (upstream); with 1 or
	// 2, power-of-two capacities from 8 (index 2) and a mask.
	// With FORK_BACKEND_HASH_TAGS, power-of-two capacities from 16 (index 2), whatever
	// FORK_BACKEND_HASH_POW2 says.
	static constexpr uint32_t _FIRST_CAPACITY_INDEX = (FORK_BACKEND_HASH_POW2 || FORK_BACKEND_HASH_TAGS) ? MIN_CAPACITY_INDEX : 0;

	_FORCE_INLINE_ static constexpr uint32_t _capacity_of(uint32_t p_idx) {
#if FORK_BACKEND_HASH_TAGS
		return 4u << p_idx;
#elif FORK_BACKEND_HASH_POW2
		return 2u << p_idx;
#else
		return hash_table_size_primes[p_idx];
#endif
	}

	_FORCE_INLINE_ static constexpr uint64_t _capacity_inv_of([[maybe_unused]] uint32_t p_idx) {
#if FORK_BACKEND_HASH_POW2
		return 0; // No fastmod.
#else
		return hash_table_size_primes_inv[p_idx];
#endif
	}

	// The slot of p_n (a hash, or an index one past a slot) in a table of p_capacity slots.
	_FORCE_INLINE_ static uint32_t _home(uint32_t p_n, [[maybe_unused]] uint64_t p_capacity_inv, uint32_t p_capacity) {
#if FORK_BACKEND_HASH_POW2
		return p_n & (p_capacity - 1);
#else
		return fastmod(p_n, p_capacity_inv, p_capacity);
#endif
	}

	_FORCE_INLINE_ uint32_t _hash(const TKey &p_key) const {
		uint32_t hash = hash_table_key<Hasher>(p_key);
#if FORK_BACKEND_HASH_TAGS
		hash = hash_table_mix<Hasher, TKey, 2>(hash);
#elif FORK_BACKEND_HASH_POW2
		hash = hash_table_mix<Hasher, TKey>(hash);
#endif

		if (unlikely(hash == EMPTY_HASH)) {
			hash = EMPTY_HASH + 1;
		}

		return hash;
	}

	_FORCE_INLINE_ static constexpr void _increment_mod(uint32_t &r_idx, const uint32_t p_capacity) {
#if FORK_BACKEND_HASH_POW2
		r_idx = (r_idx + 1) & (p_capacity - 1);
#else
		r_idx++;
		// `if` is faster than both fastmod and mod.
		if (unlikely(r_idx == p_capacity)) {
			r_idx = 0;
		}
#endif
	}

	static _FORCE_INLINE_ uint32_t _get_probe_length(const uint32_t p_hash_idx, const uint32_t p_hash, const uint32_t p_capacity, [[maybe_unused]] const uint64_t p_capacity_inv) {
#if FORK_BACKEND_HASH_POW2
		return (p_hash_idx - (p_hash & (p_capacity - 1))) & (p_capacity - 1);
#else
		const uint32_t original_idx = fastmod(p_hash, p_capacity_inv, p_capacity);
		const uint32_t distance_idx = p_hash_idx - original_idx + p_capacity;
		// At most p_capacity over 0, so we can use an if (faster than fastmod).
		return distance_idx >= p_capacity ? distance_idx - p_capacity : distance_idx;
#endif
	}

#if FORK_BACKEND_HASH_TAGS
	// Fork (FORK_BACKEND_HASH_TAGS, kb/project/hash-simd-tags.md): slots are found by groups of
	// control bytes (HashTags in hashfuncs.h). FORK_BACKEND_HASH_POW2 does not apply here: the
	// capacities are powers of two from 16 either way, and the hash is mixed with hash_fmix32.
	_FORCE_INLINE_ uint8_t *_ctrl() const {
		return reinterpret_cast<uint8_t *>(_hash_idx_to_key_idx + _capacity_of(_capacity_idx));
	}

	bool _lookup_slot(const TKey &p_key, uint32_t p_hash, uint32_t &r_slot) const {
		const uint32_t *slots = _hash_idx_to_key_idx;
		return HashTags::find(_ctrl(), _capacity_of(_capacity_idx) - 1, p_hash, [&](uint32_t p_slot) { return Comparator::compare(_keys[slots[p_slot]], p_key); }, r_slot);
	}

	bool _lookup_key_idx(const TKey &p_key, uint32_t &r_key_idx) const {
		if (_keys == nullptr || _size == 0) {
			return false; // Failed lookups, no elements
		}
		uint32_t slot = 0;
		if (!_lookup_slot(p_key, _hash(p_key), slot)) {
			return false;
		}
		r_key_idx = _hash_idx_to_key_idx[slot];
		return true;
	}

	// Puts key p_key_idx in the first free slot of its probe sequence.
	void _insert_slot(uint32_t p_hash, uint32_t p_key_idx) {
		const uint32_t mask = _capacity_of(_capacity_idx) - 1;
		uint8_t *ctrl = _ctrl();
		const uint32_t slot = HashTags::find_free(ctrl, mask, p_hash);
		if (ctrl[slot] == HashTags::DELETED) {
			_deleted--;
		}
		HashTags::set(ctrl, mask, slot, HashTags::tag(p_hash));
		_hash_idx_to_key_idx[slot] = p_key_idx;
	}

	// A new block for p_new_capacity_idx (never below the current one), slots built from the
	// key hashes; drops every DELETED byte.
	void _resize_and_rehash(uint32_t p_new_capacity_idx) {
		_capacity_idx = MAX((uint32_t)MIN_CAPACITY_INDEX, p_new_capacity_idx);

		uint32_t *old_hashes = _hashes;
		TKey *old_keys = _keys;

		_allocate_block(_capacity_of(_capacity_idx));
		// Keys move by memory copy, as the class requires (see above).
		if (_size > 0) {
			memcpy(static_cast<void *>(_keys), static_cast<const void *>(old_keys), sizeof(TKey) * _size);
			memcpy(_hashes, old_hashes, sizeof(uint32_t) * _size);
		}
		for (uint32_t i = 0; i < _size; i++) {
			_insert_slot(_hashes[i], i);
		}

		Memory::free_static(old_hashes);
	}

	// Returns key index.
	_FORCE_INLINE_ int32_t _insert(const TKey &p_key) {
		const uint32_t capacity = _capacity_of(_capacity_idx);
		if (unlikely(_keys == nullptr)) {
			// Allocate on demand to save memory.
			_allocate_block(capacity);
		}

		const uint32_t hash = _hash(p_key);
		uint32_t slot = 0;
		if (_lookup_slot(p_key, hash, slot)) {
			return _hash_idx_to_key_idx[slot];
		}

		const uint32_t max_size = _key_capacity(capacity);
		if (_size + _deleted >= max_size) {
			// Grow when full or when few bytes are DELETED, else drop the DELETED bytes in place.
			if (_size >= max_size || _deleted * 4 < _size) {
				ERR_FAIL_COND_V_MSG(_capacity_idx + 1 == HASH_TABLE_SIZE_MAX, -1, "Hash table maximum capacity reached, aborting insertion.");
				_resize_and_rehash(_capacity_idx + 1);
			} else {
				_resize_and_rehash(_capacity_idx);
			}
		}

		memnew_placement(&_keys[_size], TKey(p_key));
		_hashes[_size] = hash;
		_insert_slot(hash, _size);
		_size++;
		return _size - 1;
	}

	void _init_from(const HashSet &p_other) {
		_capacity_idx = p_other._capacity_idx;
		_size = p_other._size;
		_deleted = 0;

		if (p_other._size == 0) {
			return;
		}

		const uint32_t capacity = _capacity_of(_capacity_idx);
		_allocate_block(capacity);

		for (uint32_t i = 0; i < _size; i++) {
			memnew_placement(&_keys[i], TKey(p_other._keys[i]));
		}
		memcpy(_hashes, p_other._hashes, sizeof(uint32_t) * _size);
		// The slots, then the control bytes.
		memcpy(_hash_idx_to_key_idx, p_other._hash_idx_to_key_idx, sizeof(uint32_t) * capacity + capacity + HashTags::WIDTH);
		_deleted = p_other._deleted;
	}
#else
	bool _lookup_key_idx(const TKey &p_key, uint32_t &r_key_idx) const {
		if (_keys == nullptr || _size == 0) {
			return false; // Failed lookups, no elements
		}

		const uint32_t capacity = _capacity_of(_capacity_idx);
		const uint64_t capacity_inv = _capacity_inv_of(_capacity_idx);
		uint32_t hash = _hash(p_key);
		uint32_t hash_idx = _home(hash, capacity_inv, capacity);
		uint32_t distance = 0;

		while (true) {
			if (_hashes[hash_idx] == EMPTY_HASH) {
				return false;
			}

			if (_hashes[hash_idx] == hash && Comparator::compare(_keys[_hash_idx_to_key_idx[hash_idx]], p_key)) {
				r_key_idx = _hash_idx_to_key_idx[hash_idx];
				return true;
			}

			if (distance > _get_probe_length(hash_idx, _hashes[hash_idx], capacity, capacity_inv)) {
				return false;
			}

			_increment_mod(hash_idx, capacity);
			distance++;
		}
	}

	uint32_t _insert_with_hash(uint32_t p_hash, uint32_t p_key_idx) {
		const uint32_t capacity = _capacity_of(_capacity_idx);
		const uint64_t capacity_inv = _capacity_inv_of(_capacity_idx);
		uint32_t hash = p_hash;
		uint32_t key_idx = p_key_idx;
		uint32_t distance = 0;
		uint32_t hash_idx = _home(hash, capacity_inv, capacity);

		while (true) {
			if (_hashes[hash_idx] == EMPTY_HASH) {
				_hashes[hash_idx] = hash;
				_key_idx_to_hash_idx[key_idx] = hash_idx;
				_hash_idx_to_key_idx[hash_idx] = key_idx;
				return hash_idx;
			}

			// Not an empty slot, let's check the probing length of the existing one.
			uint32_t existing_probe_len = _get_probe_length(hash_idx, _hashes[hash_idx], capacity, capacity_inv);
			if (existing_probe_len < distance) {
				_key_idx_to_hash_idx[key_idx] = hash_idx;
				SWAP(hash, _hashes[hash_idx]);
				SWAP(key_idx, _hash_idx_to_key_idx[hash_idx]);
				distance = existing_probe_len;
			}

			_increment_mod(hash_idx, capacity);
			distance++;
		}
	}

	void _resize_and_rehash(uint32_t p_new_capacity_idx) {
		// Capacity can't be 0.
		_capacity_idx = MAX((uint32_t)MIN_CAPACITY_INDEX, p_new_capacity_idx);

		uint32_t capacity = _capacity_of(_capacity_idx);

		uint32_t *old_hashes = _hashes;
		uint32_t *old_key_to_hash = _key_idx_to_hash_idx;
		TKey *old_keys = _keys;

		_allocate_block(capacity);
		// Keys move by memory copy, as the class requires (see above).
		if (_size > 0) {
			memcpy(static_cast<void *>(_keys), static_cast<const void *>(old_keys), sizeof(TKey) * _size);
		}

		for (uint32_t i = 0; i < _size; i++) {
			uint32_t h = old_hashes[old_key_to_hash[i]];
			_insert_with_hash(h, i);
		}

		Memory::free_static(old_hashes);
	}

	// Returns key index.
	_FORCE_INLINE_ int32_t _insert(const TKey &p_key) {
		uint32_t capacity = _capacity_of(_capacity_idx);
		if (unlikely(_keys == nullptr)) {
			// Allocate on demand to save memory.
			_allocate_block(capacity);
		}

		uint32_t key_idx = 0;
		bool exists = _lookup_key_idx(p_key, key_idx);

		if (exists) {
			return key_idx;
		} else {
			if (_size + 1 > MAX_OCCUPANCY * capacity) {
				ERR_FAIL_COND_V_MSG(_capacity_idx + 1 == HASH_TABLE_SIZE_MAX, -1, "Hash table maximum capacity reached, aborting insertion.");
				_resize_and_rehash(_capacity_idx + 1);
			}

			uint32_t hash = _hash(p_key);
			memnew_placement(&_keys[_size], TKey(p_key));
			_insert_with_hash(hash, _size);
			_size++;
			return _size - 1;
		}
	}

	void _init_from(const HashSet &p_other) {
		_capacity_idx = p_other._capacity_idx;
		_size = p_other._size;

		if (p_other._size == 0) {
			return;
		}

		uint32_t capacity = _capacity_of(_capacity_idx);

		_allocate_block(capacity);

		for (uint32_t i = 0; i < _size; i++) {
			memnew_placement(&_keys[i], TKey(p_other._keys[i]));
			_key_idx_to_hash_idx[i] = p_other._key_idx_to_hash_idx[i];
		}

		for (uint32_t i = 0; i < capacity; i++) {
			_hashes[i] = p_other._hashes[i];
			_hash_idx_to_key_idx[i] = p_other._hash_idx_to_key_idx[i];
		}
	}
#endif // FORK_BACKEND_HASH_TAGS

public:
	_FORCE_INLINE_ uint32_t get_capacity() const { return _capacity_of(_capacity_idx); }
	_FORCE_INLINE_ uint32_t size() const { return _size; }

	/* Standard Godot Container API */

	bool is_empty() const {
		return _size == 0;
	}

	void clear() {
		if (_keys == nullptr || _size == 0) {
			return;
		}

		uint32_t capacity = _capacity_of(_capacity_idx);
#if FORK_BACKEND_HASH_TAGS
		memset(_ctrl(), HashTags::EMPTY, capacity + HashTags::WIDTH);
		_deleted = 0;
#else
		memset(_hashes, EMPTY_HASH, sizeof(EMPTY_HASH) * capacity);
#endif

		if constexpr (!std::is_trivially_destructible_v<TKey>) {
			for (uint32_t i = 0; i < _size; i++) {
				_keys[i].~TKey();
			}
		}

		_size = 0;
	}

	_FORCE_INLINE_ bool has(const TKey &p_key) const {
		uint32_t _idx = 0;
		return _lookup_key_idx(p_key, _idx);
	}

	bool erase(const TKey &p_key) {
#if FORK_BACKEND_HASH_TAGS
		if (_keys == nullptr || _size == 0) {
			return false;
		}
		const uint32_t mask = _capacity_of(_capacity_idx) - 1;
		uint8_t *ctrl = _ctrl();
		uint32_t slot = 0;
		if (!_lookup_slot(p_key, _hash(p_key), slot)) {
			return false;
		}

		const uint32_t key_idx = _hash_idx_to_key_idx[slot];
		if (HashTags::erase(ctrl, mask, slot)) {
			_deleted++;
		}
		_keys[key_idx].~TKey();
		_size--;
		if (key_idx < _size) {
			// Not the last key, move the last one here to keep keys contiguous.
			memnew_placement(&_keys[key_idx], TKey(_keys[_size]));
			_keys[_size].~TKey();
			const uint32_t moved_hash = _hashes[_size];
			uint32_t moved_slot = 0;
			[[maybe_unused]] const bool found = HashTags::find(ctrl, mask, moved_hash, [&](uint32_t p_slot) { return _hash_idx_to_key_idx[p_slot] == _size; }, moved_slot);
			DEV_ASSERT(found);
			_hash_idx_to_key_idx[moved_slot] = key_idx;
			_hashes[key_idx] = moved_hash;
		}

		return true;
#else
		uint32_t key_idx = 0;
		bool exists = _lookup_key_idx(p_key, key_idx);

		if (!exists) {
			return false;
		}

		uint32_t hash_idx = _key_idx_to_hash_idx[key_idx];

		const uint32_t capacity = _capacity_of(_capacity_idx);
		const uint64_t capacity_inv = _capacity_inv_of(_capacity_idx);
		uint32_t next_hash_idx = _home(hash_idx + 1, capacity_inv, capacity);
		while (_hashes[next_hash_idx] != EMPTY_HASH && _get_probe_length(next_hash_idx, _hashes[next_hash_idx], capacity, capacity_inv) != 0) {
			uint32_t cur_key_idx = _hash_idx_to_key_idx[hash_idx];
			uint32_t next_key_idx = _hash_idx_to_key_idx[next_hash_idx];
			SWAP(_key_idx_to_hash_idx[cur_key_idx], _key_idx_to_hash_idx[next_key_idx]);
			SWAP(_hashes[next_hash_idx], _hashes[hash_idx]);
			SWAP(_hash_idx_to_key_idx[next_hash_idx], _hash_idx_to_key_idx[hash_idx]);

			hash_idx = next_hash_idx;
			_increment_mod(next_hash_idx, capacity);
		}

		_hashes[hash_idx] = EMPTY_HASH;
		_keys[key_idx].~TKey();
		_size--;
		if (key_idx < _size) {
			// Not the last key, move the last one here to keep keys contiguous.
			memnew_placement(&_keys[key_idx], TKey(_keys[_size]));
			_keys[_size].~TKey();
			_key_idx_to_hash_idx[key_idx] = _key_idx_to_hash_idx[_size];
			_hash_idx_to_key_idx[_key_idx_to_hash_idx[_size]] = key_idx;
		}

		return true;
#endif
	}

	// Reserves space for a number of elements, useful to avoid many resizes and rehashes.
	// If adding a known (possibly large) number of elements at once, must be larger than old capacity.
	void reserve(uint32_t p_new_capacity) {
		uint32_t new_capacity_idx = _capacity_idx;

		while (_capacity_of(new_capacity_idx) < p_new_capacity) {
			ERR_FAIL_COND_MSG(new_capacity_idx + 1 == (uint32_t)HASH_TABLE_SIZE_MAX, nullptr);
			new_capacity_idx++;
		}

		if (new_capacity_idx == _capacity_idx) {
			if (p_new_capacity < _size) {
				WARN_VERBOSE("reserve() called with a capacity smaller than the current size. This is likely a mistake.");
			}
			return;
		}

		if (_keys == nullptr) {
			_capacity_idx = new_capacity_idx;
			return; // Unallocated yet.
		}
		_resize_and_rehash(new_capacity_idx);
	}

	/** Iterator API **/

	struct Iterator {
		_FORCE_INLINE_ const TKey &operator*() const {
			return _keys[_key_idx];
		}
		_FORCE_INLINE_ const TKey *operator->() const {
			return &_keys[_key_idx];
		}
		_FORCE_INLINE_ Iterator &operator++() {
			_key_idx++;
			if (_key_idx >= (int32_t)_num_keys) {
				_key_idx = -1;
				_keys = nullptr;
				_num_keys = 0;
			}
			return *this;
		}
		_FORCE_INLINE_ Iterator &operator--() {
			_key_idx--;
			if (_key_idx < 0) {
				_key_idx = -1;
				_keys = nullptr;
				_num_keys = 0;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const Iterator &p_other) const { return _keys == p_other._keys && _key_idx == p_other._key_idx; }
		_FORCE_INLINE_ bool operator!=(const Iterator &p_other) const { return _keys != p_other._keys || _key_idx != p_other._key_idx; }

		_FORCE_INLINE_ explicit operator bool() const {
			return _keys != nullptr;
		}

		_FORCE_INLINE_ Iterator(const TKey *p_keys, uint32_t p_num_keys, int32_t p_key_idx = -1) {
			_keys = p_keys;
			_num_keys = p_num_keys;
			_key_idx = p_key_idx;
		}
		_FORCE_INLINE_ Iterator() {}
		_FORCE_INLINE_ Iterator(const Iterator &p_it) {
			_keys = p_it._keys;
			_num_keys = p_it._num_keys;
			_key_idx = p_it._key_idx;
		}
		_FORCE_INLINE_ void operator=(const Iterator &p_it) {
			_keys = p_it._keys;
			_num_keys = p_it._num_keys;
			_key_idx = p_it._key_idx;
		}

	private:
		const TKey *_keys = nullptr;
		uint32_t _num_keys = 0;
		int32_t _key_idx = -1;
	};

	_FORCE_INLINE_ Iterator begin() const _LIFETIME_BOUND_ {
		return _size ? Iterator(_keys, _size, 0) : Iterator();
	}
	_FORCE_INLINE_ Iterator end() const _LIFETIME_BOUND_ {
		return Iterator();
	}
	_FORCE_INLINE_ Iterator last() const _LIFETIME_BOUND_ {
		if (_size == 0) {
			return Iterator();
		}
		return Iterator(_keys, _size, _size - 1);
	}

	_FORCE_INLINE_ Iterator find(const TKey &p_key) const _LIFETIME_BOUND_ {
		uint32_t key_idx = 0;
		bool exists = _lookup_key_idx(p_key, key_idx);
		if (!exists) {
			return end();
		}
		return Iterator(_keys, _size, key_idx);
	}

	_FORCE_INLINE_ void remove(const Iterator &p_iter) {
		if (p_iter) {
			erase(*p_iter);
		}
	}

	/* Insert */

	Iterator insert(const TKey &p_key) _LIFETIME_BOUND_ {
		uint32_t key_idx = _insert(p_key);
		return Iterator(_keys, _size, key_idx);
	}

	/* Constructors */

	explicit HashSet(const HashSet &p_other) {
		_init_from(p_other);
	}

	HashSet(HashSet &&p_other) {
		_keys = p_other._keys;
		_hash_idx_to_key_idx = p_other._hash_idx_to_key_idx;
#if FORK_BACKEND_HASH_TAGS
		_deleted = p_other._deleted;
		p_other._deleted = 0;
#else
		_key_idx_to_hash_idx = p_other._key_idx_to_hash_idx;
		p_other._key_idx_to_hash_idx = nullptr;
#endif
		_hashes = p_other._hashes;

		_capacity_idx = p_other._capacity_idx;
		_size = p_other._size;

		p_other._keys = nullptr;
		p_other._hash_idx_to_key_idx = nullptr;
		p_other._hashes = nullptr;

		p_other._capacity_idx = _FIRST_CAPACITY_INDEX;
		p_other._size = 0;
	}

	void operator=(const HashSet &p_other) {
		if (this == &p_other) {
			return; // Ignore self assignment.
		}

		clear();

		if (_keys != nullptr) {
			_free_block();
		}

		_init_from(p_other);
	}

	void operator=(HashSet &&p_other) {
		if (this == &p_other) {
			return; // Ignore self assignment.
		}

		SWAP(_keys, p_other._keys);
		SWAP(_hash_idx_to_key_idx, p_other._hash_idx_to_key_idx);
#if FORK_BACKEND_HASH_TAGS
		SWAP(_deleted, p_other._deleted);
#else
		SWAP(_key_idx_to_hash_idx, p_other._key_idx_to_hash_idx);
#endif
		SWAP(_hashes, p_other._hashes);

		SWAP(_capacity_idx, p_other._capacity_idx);
		SWAP(_size, p_other._size);
	}

	bool operator==(const HashSet &p_other) const {
		if (_size != p_other._size) {
			return false;
		}
		for (uint32_t i = 0; i < _size; i++) {
			if (!p_other.has(_keys[i])) {
				return false;
			}
		}
		return true;
	}
	bool operator!=(const HashSet &p_other) const {
		return !(*this == p_other);
	}

	explicit HashSet(uint32_t p_initial_capacity) {
		// Capacity can't be 0.
		_capacity_idx = _FIRST_CAPACITY_INDEX;
		reserve(p_initial_capacity);
	}
	HashSet() {
		_capacity_idx = MIN_CAPACITY_INDEX;
	}
	HashSet(std::initializer_list<TKey> p_init) {
		reserve(p_init.size());
		for (const TKey &E : p_init) {
			insert(E);
		}
	}

	void reset() {
		clear();

		if (_keys != nullptr) {
			_free_block();
		}
		_capacity_idx = MIN_CAPACITY_INDEX;
	}

	~HashSet() {
		clear();

		if (_keys != nullptr) {
			Memory::free_static(_hashes);
		}
	}
};
