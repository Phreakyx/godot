/**************************************************************************/
/*  test_fork_memory.cpp                                                  */
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

TEST_FORCE_LINK(test_fork_memory)

#include "core/os/memory.h"

#ifdef TOOLS_ENABLED
#include "core/variant/typed_array.h"
#include "editor/themes/builtin_fonts.gen.h"
#include "servers/text/text_server.h"
#endif

namespace TestForkMemory {

// Pins of the Memory:: behavior that every allocator under it (FORK_BACKEND_ALLOCATOR)
// must keep. Release builds (template_release) take the paths without the prepad header.

TEST_CASE("[Fork][Memory] realloc to 0 bytes frees and returns nullptr") {
	void *p = Memory::alloc_static(64, false);
	REQUIRE(p != nullptr);
	CHECK(Memory::realloc_static(p, 0, false) == nullptr);

	void *q = Memory::alloc_static(64, true);
	REQUIRE(q != nullptr);
	CHECK(Memory::realloc_static(q, 0, true) == nullptr);
}

TEST_CASE("[Fork][Memory] realloc keeps the first bytes") {
	uint8_t *p = (uint8_t *)Memory::alloc_static(100, false);
	REQUIRE(p != nullptr);
	for (int i = 0; i < 100; i++) {
		p[i] = (uint8_t)i;
	}
	p = (uint8_t *)Memory::realloc_static(p, 100000, false);
	REQUIRE(p != nullptr);
	bool same = true;
	for (int i = 0; i < 100; i++) {
		same = same && p[i] == (uint8_t)i;
	}
	CHECK(same);
	p = (uint8_t *)Memory::realloc_static(p, 10, false);
	REQUIRE(p != nullptr);
	same = true;
	for (int i = 0; i < 10; i++) {
		same = same && p[i] == (uint8_t)i;
	}
	CHECK(same);
	Memory::free_static(p, false);
}

TEST_CASE("[Fork][Memory] aligned alloc, realloc and free") {
	const size_t alignments[] = { 16, 64, 4096 };
	for (size_t alignment : alignments) {
		uint8_t *p = (uint8_t *)Memory::alloc_aligned_static(200, alignment);
		REQUIRE(p != nullptr);
		CHECK(((uintptr_t)p % alignment) == 0);
		for (int i = 0; i < 200; i++) {
			p[i] = (uint8_t)(i * 3);
		}
		p = (uint8_t *)Memory::realloc_aligned_static(p, 5000, 200, alignment);
		REQUIRE(p != nullptr);
		CHECK(((uintptr_t)p % alignment) == 0);
		bool same = true;
		for (int i = 0; i < 200; i++) {
			same = same && p[i] == (uint8_t)(i * 3);
		}
		CHECK(same);
		Memory::free_aligned_static(p);
	}
}

TEST_CASE("[Fork][Memory] memnew_arr and memdelete_arr round trip") {
	int *arr = memnew_arr(int, 1000);
	REQUIRE(arr != nullptr);
	CHECK(memarr_len(arr) == 1000);
	for (int i = 0; i < 1000; i++) {
		arr[i] = i * 7;
	}
	bool same = true;
	for (int i = 0; i < 1000; i++) {
		same = same && arr[i] == i * 7;
	}
	CHECK(same);
	memdelete_arr(arr);
}

#ifdef TOOLS_ENABLED
// Found by the allocator audit (kb/project/allocator-audit.md): with more than
// ScriptIterator::EMOJI_STACK_DEPTH (32) emoji runs in one script run, the emoji stack moved to
// the heap without its first 32 entries (garbage text ranges), and the next script run used and
// freed the freed block again (modules/text_server_adv/script_iterator.cpp).
TEST_CASE("[Fork][Memory] Shaping more than 32 emoji runs before another script run") {
	String text;
	for (int i = 0; i < 40; i++) {
		text += U"a😀";
	}
	text += U" ЖЖ b😀";

	for (int i = 0; i < TextServerManager::get_singleton()->get_interface_count(); i++) {
		Ref<TextServer> ts = TextServerManager::get_singleton()->get_interface(i);
		REQUIRE(ts.is_valid());
		if (!ts->has_feature(TextServer::FEATURE_FONT_DYNAMIC)) {
			continue;
		}
		RID font = ts->create_font();
		ts->font_set_data_ptr(font, _font_Inter_Regular, _font_Inter_Regular_size);
		ts->font_set_allow_system_fallback(font, false);
		TypedArray<RID> fonts;
		fonts.push_back(font);

		RID ctx = ts->create_shaped_text();
		CHECK(ts->shaped_text_add_string(ctx, text, fonts, 16));
		CHECK(ts->shaped_text_shape(ctx));
		const Glyph *glyphs = ts->shaped_text_get_glyphs(ctx);
		const int64_t glyph_count = ts->shaped_text_get_glyph_count(ctx);
		CHECK(glyph_count > 0);
		// Every character is in exactly one glyph cluster, in order.
		int64_t next = 0;
		bool contiguous = true;
		for (int64_t j = 0; j < glyph_count; j++) {
			if (glyphs[j].start == next) {
				next = glyphs[j].end;
			} else {
				contiguous = contiguous && glyphs[j].start < next && glyphs[j].end == next;
			}
		}
		CHECK(contiguous);
		CHECK(next == text.length());
		ts->free_rid(ctx);
		ts->free_rid(font);
	}
}
#endif // TOOLS_ENABLED

} // namespace TestForkMemory
