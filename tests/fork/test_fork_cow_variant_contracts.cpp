/**************************************************************************/
/*  test_fork_cow_variant_contracts.cpp                                   */
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

TEST_FORCE_LINK(test_fork_cow_variant_contracts)

#include "core/math/math_funcs.h"
#include "core/os/thread.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Contracts that every CowData, String, Packed*Array, Array, Dictionary and StringName backend
// must keep. They pin today's behavior. Only StringName promises thread safety (its global
// table is locked); the other cases are single-threaded.

namespace TestForkCowVariantContracts {

// Visits a dictionary with next(), the way GDScript's `for k in dict` does: the cursor is a copy
// of the last key, so the loop ends when that key is no longer in the dictionary.
template <typename F>
static Array visit_keys(Dictionary &p_dict, F p_on_key) {
	Array visited;
	const Variant *next = p_dict.next(nullptr);
	while (next != nullptr) {
		const Variant key = *next;
		visited.push_back(key);
		p_on_key(key);
		next = p_dict.next(&key);
	}
	return visited;
}

static Dictionary abc_dictionary() {
	Dictionary dict;
	dict["a"] = 1;
	dict["b"] = 2;
	dict["c"] = 3;
	return dict;
}

TEST_CASE("[Fork][CowData] A copy shares the buffer until the first write") {
	Vector<int> original = { 1, 2, 3 };
	Vector<int> copy = original;
	CHECK(copy.ptr() == original.ptr());

	copy.set(0, 9);
	CHECK(copy.ptr() != original.ptr());
	CHECK(original[0] == 1);
	CHECK(copy[0] == 9);
}

TEST_CASE("[Fork][CowData] A detached buffer is not copied again") {
	Vector<int> original = { 1, 2, 3 };
	Vector<int> copy = original;
	copy.set(0, 9);
	const int *detached = copy.ptr();

	copy.set(1, 8);
	copy.write[2] = 7;
	CHECK(copy.ptr() == detached);
	CHECK(copy == Vector<int>{ 9, 8, 7 });
	CHECK(original == Vector<int>{ 1, 2, 3 });
}

TEST_CASE("[Fork][CowData] Empty Vector") {
	Vector<int> empty;
	CHECK(empty.size() == 0);
	CHECK(empty.is_empty());
	CHECK(empty.ptr() == nullptr);

	Vector<int> emptied = { 1 };
	emptied.clear();
	CHECK(emptied.ptr() == nullptr);
}

TEST_CASE("[Fork][String] NUL terminator and size") {
	const String text = String("abc");
	CHECK(text.length() == 3);
	CHECK(text.size() == text.length() + 1);
	CHECK(text.ptr()[text.length()] == 0);
}

TEST_CASE("[Fork][String] Empty String") {
	const String empty;
	CHECK(empty.length() == 0);
	CHECK(empty.size() == 0);
	CHECK(empty.ptr() == nullptr);
	REQUIRE(empty.get_data() != nullptr);
	CHECK(empty.get_data()[0] == 0);
	CHECK(String("").ptr() == nullptr);
}

TEST_CASE("[Fork][String] Length counts UTF-32 code points") {
	// "a", e-acute, euro sign, grinning face: 1, 2, 3 and 4 bytes in UTF-8.
	const String text = String::utf8("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
	CHECK(text.length() == 4);
	CHECK(text[1] == 0x00E9);
	CHECK(text[3] == 0x1F600);
}

TEST_CASE("[Fork][String] No Unicode normalization") {
	const String precomposed = String(U"\u00E9");
	const String decomposed = String(U"e\u0301");
	CHECK(precomposed.length() == 1);
	CHECK(decomposed.length() == 2);
	CHECK(precomposed != decomposed);
}

TEST_CASE("[Fork][PackedArray] Copy-on-write through Variant") {
	const PackedInt32Array original = { 1, 2, 3 };
	const Variant variant = original;
	PackedInt32Array copy = variant;
	CHECK(copy.ptr() == original.ptr());

	copy.set(0, 9);
	CHECK(copy.ptr() != original.ptr());
	CHECK(original[0] == 1);
	const PackedInt32Array from_variant = variant;
	CHECK(from_variant[0] == 1);
	CHECK(from_variant.ptr() == original.ptr());
}

TEST_CASE("[Fork][PackedArray] Empty packed arrays") {
	const PackedInt32Array empty;
	CHECK(empty.size() == 0);
	CHECK(empty.ptr() == nullptr);

	const Variant variant = empty;
	const PackedInt32Array back = variant;
	CHECK(back.is_empty());
	CHECK(back.ptr() == nullptr);
	CHECK(PackedStringArray().ptr() == nullptr);
}

TEST_CASE("[Fork][Array] Copies share, duplicate() detaches") {
	Array original;
	original.push_back(1);
	Array copy = original;
	copy.push_back(2);
	CHECK(original.size() == 2);
	CHECK(original.id() == copy.id());

	Array duplicate = original.duplicate();
	duplicate.push_back(3);
	CHECK(original.size() == 2);
	CHECK(duplicate.size() == 3);
	CHECK(duplicate.id() != original.id());
}

TEST_CASE("[Fork][Array] Empty Array") {
	const Array empty;
	CHECK(empty.size() == 0);
	CHECK(empty.is_empty());
	CHECK(empty.duplicate().is_empty());
}

TEST_CASE("[Fork][Dictionary] String and StringName are one key, the first key type is kept") {
	Dictionary string_first;
	string_first["a"] = 1;
	string_first[StringName("a")] = 2;
	CHECK(string_first.size() == 1);
	CHECK(int(string_first["a"]) == 2);
	CHECK(string_first.keys()[0].get_type() == Variant::STRING);

	Dictionary name_first;
	name_first[StringName("a")] = 1;
	name_first["a"] = 2;
	CHECK(name_first.size() == 1);
	CHECK(int(name_first[StringName("a")]) == 2);
	CHECK(name_first.keys()[0].get_type() == Variant::STRING_NAME);
}

TEST_CASE("[Fork][Dictionary] int 1 and float 1.0 are different keys") {
	Dictionary dict;
	dict[1] = "int";
	dict[1.0] = "float";
	CHECK(dict.size() == 2);
	CHECK(String(dict[1]) == "int");
	CHECK(String(dict[1.0]) == "float");
}

TEST_CASE("[Fork][Dictionary] NaN is one key and can be found") {
	Dictionary dict;
	dict[Math::NaN] = 1;
	dict[Math::NaN] = 2;
	CHECK(dict.size() == 1);
	CHECK(dict.has(Math::NaN));
	CHECK(int(dict[Math::NaN]) == 2);
}

TEST_CASE("[Fork][Dictionary] Insertion order, re-insert moves the key to the end") {
	Dictionary dict = abc_dictionary();
	CHECK(dict.keys() == Array{ "a", "b", "c" });

	dict["a"] = 10;
	CHECK(dict.keys() == Array{ "a", "b", "c" });

	dict.erase("a");
	dict["a"] = 1;
	CHECK(dict.keys() == Array{ "b", "c", "a" });
}

TEST_CASE("[Fork][Dictionary] next() loop with mutation") {
	SUBCASE("Erasing the current key ends the loop") {
		Dictionary dict = abc_dictionary();
		const Array visited = visit_keys(dict, [&](const Variant &p_key) { dict.erase(p_key); });
		CHECK(visited == Array{ "a" });
		CHECK(dict.keys() == Array{ "b", "c" });
	}
	SUBCASE("A not yet visited key that is erased is skipped") {
		Dictionary dict = abc_dictionary();
		const Array visited = visit_keys(dict, [&](const Variant &p_key) {
			if (p_key == Variant("a")) {
				dict.erase("b");
			}
		});
		CHECK(visited == Array{ "a", "c" });
	}
	SUBCASE("A key inserted during the loop is visited") {
		Dictionary dict = abc_dictionary();
		const Array visited = visit_keys(dict, [&](const Variant &p_key) {
			if (p_key == Variant("a")) {
				dict["d"] = 4;
			}
		});
		CHECK(visited == Array{ "a", "b", "c", "d" });
	}
}

TEST_CASE("[Fork][Dictionary] Empty Dictionary") {
	const Dictionary empty;
	CHECK(empty.size() == 0);
	CHECK(empty.is_empty());
	CHECK(empty.next(nullptr) == nullptr);
	CHECK(empty.keys().is_empty());
}

TEST_CASE("[Fork][StringName] Same text, same pointer") {
	const StringName from_char("fork_name");
	const StringName from_string(String("fork_name"));
	CHECK(from_char.data_unique_pointer() != nullptr);
	CHECK(from_char.data_unique_pointer() == from_string.data_unique_pointer());
	CHECK(from_char == from_string);
}

TEST_CASE("[Fork][StringName] Empty StringName") {
	const StringName empty;
	CHECK(empty.is_empty());
	CHECK(empty.data_unique_pointer() == nullptr);
	CHECK(StringName("") == empty);
	CHECK(StringName(String()).data_unique_pointer() == nullptr);
}

constexpr int STRESS_THREADS = 8;
constexpr int STRESS_TEXTS = 64;
constexpr int STRESS_ITERATIONS = 20000;
constexpr int STRESS_RUNS = 3;

struct StressWorker {
	const Vector<String> *texts = nullptr;
	int seed = 0;
	Vector<StringName> kept;
};

static void stress_worker(void *p_userdata) {
	StressWorker *worker = static_cast<StressWorker *>(p_userdata);
	const Vector<String> &texts = *worker->texts;
	for (int i = 0; i < STRESS_ITERATIONS; i++) {
		const StringName transient(texts[(i * 7 + worker->seed) % STRESS_TEXTS]);
	}
	worker->kept.resize(STRESS_TEXTS);
	for (int i = 0; i < STRESS_TEXTS; i++) {
		worker->kept.write[i] = StringName(texts[i]);
	}
}

// Returns the number of texts whose StringName pointer differs between the main thread and a
// worker.
static int run_stress(const Vector<String> &p_texts) {
	Vector<StringName> held;
	for (int i = 0; i < STRESS_TEXTS; i += 2) {
		held.push_back(StringName(p_texts[i]));
	}

	StressWorker workers[STRESS_THREADS];
	Thread threads[STRESS_THREADS];
	for (int t = 0; t < STRESS_THREADS; t++) {
		workers[t].texts = &p_texts;
		workers[t].seed = t;
		threads[t].start(stress_worker, &workers[t]);
	}
	for (int t = 0; t < STRESS_THREADS; t++) {
		threads[t].wait_to_finish();
	}

	int mismatches = 0;
	for (int i = 0; i < STRESS_TEXTS; i++) {
		const StringName main_name(p_texts[i]);
		for (int t = 0; t < STRESS_THREADS; t++) {
			if (workers[t].kept[i].data_unique_pointer() != main_name.data_unique_pointer()) {
				mismatches++;
			}
		}
	}
	return mismatches;
}

TEST_CASE("[Fork][StringName] Threaded create and destroy keeps one identity per text") {
	Vector<String> texts;
	for (int i = 0; i < STRESS_TEXTS; i++) {
		texts.push_back("fork_stress_" + itos(i));
	}
	for (int run = 0; run < STRESS_RUNS; run++) {
		CHECK(run_stress(texts) == 0);
	}
}

} // namespace TestForkCowVariantContracts
