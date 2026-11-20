extends RefCounted
## The fork's Dictionary benchmark for the gdbench workload of devtools/bench/bench.py, in the shape of
## godot-benchmarks (benchmark_* methods; upstream has no Dictionary benchmark at the pinned commit).
## Insert, lookup hit, lookup miss, erase, iterate and duplicate, with String, StringName, int and
## Vector2 keys, at 1,000 and 100,000 entries. Every case does OPS entry operations (100 passes over
## 1,000 entries, one over 100,000). Keys are fixed (no random state); results go to _sink.

const SMALL = 1_000
const LARGE = 100_000
const OPS = 100_000

var _sink := 0
var _keys := {}  # key type -> size -> Array of keys present in the table
var _misses := {}  # key type -> size -> Array of keys not in the table
var _tables := {}  # key type -> size -> Dictionary with every key of _keys


func _init() -> void:
	for type: StringName in [&"string", &"string_name", &"int", &"vector2"]:
		_keys[type] = {}
		_misses[type] = {}
		_tables[type] = {}
		for size: int in [SMALL, LARGE]:
			var keys := []
			var misses := []
			for i in size:
				keys.append(_key(type, i))
				misses.append(_key(type, i + LARGE))
			var table := {}
			for key: Variant in keys:
				table[key] = 1
			_keys[type][size] = keys
			_misses[type][size] = misses
			_tables[type][size] = table


# Distinct keys spread over the 32-bit range (Knuth's multiplicative hash is a bijection mod 2^32).
func _key(p_type: StringName, p_index: int) -> Variant:
	var h := (p_index * 2654435761) % 4294967296
	match p_type:
		&"string":
			return "key_%d" % h
		&"string_name":
			return StringName("key_%d" % h)
		&"int":
			return h
	return Vector2(h & 0xffff, h >> 16)


func _insert(p_type: StringName, p_size: int) -> void:
	var keys: Array = _keys[p_type][p_size]
	for pass_index in OPS / p_size:
		var table := {}
		for key: Variant in keys:
			table[key] = pass_index
		_sink += table.size()


func _lookup_hit(p_type: StringName, p_size: int) -> void:
	var keys: Array = _keys[p_type][p_size]
	var table: Dictionary = _tables[p_type][p_size]
	var sum := 0
	for pass_index in OPS / p_size:
		for key: Variant in keys:
			sum += table[key]
	_sink += sum


func _lookup_miss(p_type: StringName, p_size: int) -> void:
	var misses: Array = _misses[p_type][p_size]
	var table: Dictionary = _tables[p_type][p_size]
	var sum := 0
	for pass_index in OPS / p_size:
		for key: Variant in misses:
			sum += table.get(key, 0)
	_sink += sum


# Each pass erases every key from a copy of the full table (the copy is the duplicate case's cost).
func _erase(p_type: StringName, p_size: int) -> void:
	var keys: Array = _keys[p_type][p_size]
	for pass_index in OPS / p_size:
		var table: Dictionary = _tables[p_type][p_size].duplicate()
		for key: Variant in keys:
			table.erase(key)
		_sink += table.size()


func _iterate(p_type: StringName, p_size: int) -> void:
	var table: Dictionary = _tables[p_type][p_size]
	var sum := 0
	for pass_index in OPS / p_size:
		for key: Variant in table:
			sum += 1
	_sink += sum


func _duplicate(p_type: StringName, p_size: int) -> void:
	for pass_index in OPS / p_size:
		_sink += _tables[p_type][p_size].duplicate().size()


func benchmark_insert_string_1k() -> void:
	_insert(&"string", SMALL)


func benchmark_insert_string_100k() -> void:
	_insert(&"string", LARGE)


func benchmark_insert_string_name_1k() -> void:
	_insert(&"string_name", SMALL)


func benchmark_insert_string_name_100k() -> void:
	_insert(&"string_name", LARGE)


func benchmark_insert_int_1k() -> void:
	_insert(&"int", SMALL)


func benchmark_insert_int_100k() -> void:
	_insert(&"int", LARGE)


func benchmark_insert_vector2_1k() -> void:
	_insert(&"vector2", SMALL)


func benchmark_insert_vector2_100k() -> void:
	_insert(&"vector2", LARGE)


func benchmark_lookup_hit_string_1k() -> void:
	_lookup_hit(&"string", SMALL)


func benchmark_lookup_hit_string_100k() -> void:
	_lookup_hit(&"string", LARGE)


func benchmark_lookup_hit_string_name_1k() -> void:
	_lookup_hit(&"string_name", SMALL)


func benchmark_lookup_hit_string_name_100k() -> void:
	_lookup_hit(&"string_name", LARGE)


func benchmark_lookup_hit_int_1k() -> void:
	_lookup_hit(&"int", SMALL)


func benchmark_lookup_hit_int_100k() -> void:
	_lookup_hit(&"int", LARGE)


func benchmark_lookup_hit_vector2_1k() -> void:
	_lookup_hit(&"vector2", SMALL)


func benchmark_lookup_hit_vector2_100k() -> void:
	_lookup_hit(&"vector2", LARGE)


func benchmark_lookup_miss_string_1k() -> void:
	_lookup_miss(&"string", SMALL)


func benchmark_lookup_miss_string_100k() -> void:
	_lookup_miss(&"string", LARGE)


func benchmark_lookup_miss_string_name_1k() -> void:
	_lookup_miss(&"string_name", SMALL)


func benchmark_lookup_miss_string_name_100k() -> void:
	_lookup_miss(&"string_name", LARGE)


func benchmark_lookup_miss_int_1k() -> void:
	_lookup_miss(&"int", SMALL)


func benchmark_lookup_miss_int_100k() -> void:
	_lookup_miss(&"int", LARGE)


func benchmark_lookup_miss_vector2_1k() -> void:
	_lookup_miss(&"vector2", SMALL)


func benchmark_lookup_miss_vector2_100k() -> void:
	_lookup_miss(&"vector2", LARGE)


func benchmark_erase_string_1k() -> void:
	_erase(&"string", SMALL)


func benchmark_erase_string_100k() -> void:
	_erase(&"string", LARGE)


func benchmark_erase_string_name_1k() -> void:
	_erase(&"string_name", SMALL)


func benchmark_erase_string_name_100k() -> void:
	_erase(&"string_name", LARGE)


func benchmark_erase_int_1k() -> void:
	_erase(&"int", SMALL)


func benchmark_erase_int_100k() -> void:
	_erase(&"int", LARGE)


func benchmark_erase_vector2_1k() -> void:
	_erase(&"vector2", SMALL)


func benchmark_erase_vector2_100k() -> void:
	_erase(&"vector2", LARGE)


func benchmark_iterate_string_1k() -> void:
	_iterate(&"string", SMALL)


func benchmark_iterate_string_100k() -> void:
	_iterate(&"string", LARGE)


func benchmark_iterate_string_name_1k() -> void:
	_iterate(&"string_name", SMALL)


func benchmark_iterate_string_name_100k() -> void:
	_iterate(&"string_name", LARGE)


func benchmark_iterate_int_1k() -> void:
	_iterate(&"int", SMALL)


func benchmark_iterate_int_100k() -> void:
	_iterate(&"int", LARGE)


func benchmark_iterate_vector2_1k() -> void:
	_iterate(&"vector2", SMALL)


func benchmark_iterate_vector2_100k() -> void:
	_iterate(&"vector2", LARGE)


func benchmark_duplicate_string_1k() -> void:
	_duplicate(&"string", SMALL)


func benchmark_duplicate_string_100k() -> void:
	_duplicate(&"string", LARGE)


func benchmark_duplicate_string_name_1k() -> void:
	_duplicate(&"string_name", SMALL)


func benchmark_duplicate_string_name_100k() -> void:
	_duplicate(&"string_name", LARGE)


func benchmark_duplicate_int_1k() -> void:
	_duplicate(&"int", SMALL)


func benchmark_duplicate_int_100k() -> void:
	_duplicate(&"int", LARGE)


func benchmark_duplicate_vector2_1k() -> void:
	_duplicate(&"vector2", SMALL)


func benchmark_duplicate_vector2_100k() -> void:
	_duplicate(&"vector2", LARGE)
