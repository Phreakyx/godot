/**************************************************************************/
/*  test_fork_hash_outputs.cpp                                            */
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

TEST_FORCE_LINK(test_fork_hash_outputs)

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/hashfuncs.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Golden public hash outputs (D-11). String::hash, StringName::hash, Variant::hash and
// HashMapHasherDefault feed API fingerprints, script hash() and saved data, so no fork backend
// may move them. The literals were recorded from the control build before any hash candidate
// changed code; a mismatch means a candidate leaked into a public hash.

namespace TestForkHashOutputs {

static String long_string() {
	String s;
	for (int i = 0; i < 300; i++) {
		s += char32_t('a' + i % 26);
	}
	return s;
}

TEST_CASE("[Fork][Hash] String and StringName hash outputs") {
	CHECK(String().hash() == 5381U);
	CHECK(String("a").hash() == 177670U);
	CHECK(String("Godot").hash() == 222454306U);
	CHECK(long_string().hash() == 137225539U);
	CHECK(StringName("Godot").hash() == 222454306U);
	CHECK(StringName().hash() == 5381U);
}

TEST_CASE("[Fork][Hash] Variant hash outputs") {
	Array array;
	array.push_back(1);
	array.push_back("a");
	Dictionary dictionary;
	dictionary["a"] = 1;
	CHECK(Variant(42).hash() == 2136435707U);
	CHECK(Variant(1.5).hash() == 3990958085U);
	CHECK(Variant(String("Godot")).hash() == 222454306U);
	CHECK(Variant(Vector2(1, 2)).hash() == 4195279070U);
	CHECK(Variant(array).hash() == 3230335652U);
	CHECK(Variant(dictionary).hash() == 120477264U);
}

TEST_CASE("[Fork][Hash] HashMapHasherDefault outputs") {
	CHECK(HashMapHasherDefault::hash(int32_t(-7)) == 214441827U);
	CHECK(HashMapHasherDefault::hash(int64_t(1) << 40) == 1776532303U);
	CHECK(HashMapHasherDefault::hash(UINT64_MAX) == 532412650U);
	CHECK(HashMapHasherDefault::hash((const void *)uintptr_t(0x1000)) == 363334632U);
	CHECK(HashMapHasherDefault::hash(String("Godot")) == 222454306U);
	CHECK(HashMapHasherDefault::hash(StringName("Godot")) == 222454306U);
	CHECK(HashMapHasherDefault::hash(Vector3(1, 2, 3)) == 2596169709U);
}

} // namespace TestForkHashOutputs
