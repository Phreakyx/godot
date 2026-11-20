/**************************************************************************/
/*  test_fork_layouts.cpp                                                 */
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

TEST_FORCE_LINK(test_fork_layouts)

#include "core/os/memory.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

#include <cstring>

// Reads live CowData headers the way the C# glue does: offsets from the formula of
// godotsharp_initialize_marshaling_information() (modules/mono/glue/runtime_interop.cpp), the
// field types of InteropStructs.cs. The compile-time half of the pin is in cowdata.h,
// array.cpp and dictionary.cpp.

namespace TestForkLayouts {

constexpr size_t REF_COUNT_OFFSET = 0;
constexpr size_t CAPACITY_OFFSET = Memory::get_aligned_address(
		REF_COUNT_OFFSET + sizeof(SafeNumeric<uint64_t>), alignof(uint64_t));
constexpr size_t SIZE_OFFSET = Memory::get_aligned_address(
		CAPACITY_OFFSET + sizeof(uint64_t), alignof(uint64_t));
constexpr size_t DATA_OFFSET = Memory::get_aligned_address(
		SIZE_OFFSET + sizeof(uint64_t), Memory::MAX_ALIGN);

struct Header {
	uint64_t refcount = 0;
	uint64_t capacity = 0;
	uint64_t size = 0;
};

static Header read_header(const void *p_data) {
	const uint8_t *base = static_cast<const uint8_t *>(p_data) - DATA_OFFSET;
	Header header;
	memcpy(&header.refcount, base + REF_COUNT_OFFSET, sizeof(uint64_t));
	memcpy(&header.capacity, base + CAPACITY_OFFSET, sizeof(uint64_t));
	memcpy(&header.size, base + SIZE_OFFSET, sizeof(uint64_t));
	return header;
}

TEST_CASE("[Fork][Layout] String header read with the C# offsets") {
	const String text = String("abc");
	REQUIRE(text.ptr() != nullptr);

	Header header = read_header(text.ptr());
	CHECK(header.refcount == 1);
	CHECK(header.size == uint64_t(text.length() + 1));
	CHECK(header.size == 4);
	CHECK(header.capacity >= header.size);

	const String copy = text;
	CHECK(copy.ptr() == text.ptr());
	header = read_header(text.ptr());
	CHECK(header.refcount == 2);
}

TEST_CASE("[Fork][Layout] PackedInt32Array header read with the C# offsets") {
	PackedInt32Array array;
	for (int32_t i = 0; i < 5; i++) {
		array.push_back(i);
	}
	REQUIRE(array.ptr() != nullptr);

	Header header = read_header(array.ptr());
	CHECK(header.refcount == 1);
	CHECK(header.size == 5);
	CHECK(header.capacity >= header.size);

	const Variant shared = array;
	header = read_header(array.ptr());
	CHECK(header.refcount == 2);
}

TEST_CASE("[Fork][Layout] Vector data pointer follows the write proxy") {
	Vector<int> vector = { 1, 2, 3 };
	static_assert(sizeof(Vector<int>) == 2 * sizeof(void *));

	const void *data = nullptr;
	memcpy(&data, reinterpret_cast<const uint8_t *>(&vector) + sizeof(void *), sizeof(void *));
	CHECK(data == vector.ptr());
}

TEST_CASE("[Fork][Layout] Header offsets are pointer aligned and ordered") {
	CHECK(CAPACITY_OFFSET == sizeof(uint64_t));
	CHECK(SIZE_OFFSET == 2 * sizeof(uint64_t));
	CHECK(DATA_OFFSET >= 3 * sizeof(uint64_t));
	CHECK(DATA_OFFSET % Memory::MAX_ALIGN == 0);
}

} // namespace TestForkLayouts
