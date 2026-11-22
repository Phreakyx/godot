/**************************************************************************/
/*  test_fork_backends.cpp                                                */
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

TEST_FORCE_LINK(test_fork_backends)

#include "core/templates/fork_backends.h"

#if FORK_BACKEND_ALLOCATOR
#include "thirdparty/mimalloc/include/mimalloc.h"

#include <cstdlib>
#endif

namespace TestForkBackends {

TEST_CASE("[Fork][Backends] Selected backends") {
	MESSAGE("FORK_BACKEND_SELFTEST=", FORK_BACKEND_SELFTEST);
	CHECK((FORK_BACKEND_SELFTEST == 0 || FORK_BACKEND_SELFTEST == 1));
	MESSAGE("FORK_BACKEND_ALLOCATOR=", FORK_BACKEND_ALLOCATOR);
	CHECK((FORK_BACKEND_ALLOCATOR == 0 || FORK_BACKEND_ALLOCATOR == 1 || FORK_BACKEND_ALLOCATOR == 3));
	MESSAGE("FORK_BACKEND_HASHMAP_POOL=", FORK_BACKEND_HASHMAP_POOL);
	CHECK((FORK_BACKEND_HASHMAP_POOL == 0 || FORK_BACKEND_HASHMAP_POOL == 1));
	MESSAGE("FORK_BACKEND_HASH_POW2=", FORK_BACKEND_HASH_POW2);
	CHECK((FORK_BACKEND_HASH_POW2 == 0 || FORK_BACKEND_HASH_POW2 == 1 || FORK_BACKEND_HASH_POW2 == 2));
	MESSAGE("FORK_BACKEND_TABLE_KEY_HASH=", FORK_BACKEND_TABLE_KEY_HASH);
	CHECK((FORK_BACKEND_TABLE_KEY_HASH == 0 || FORK_BACKEND_TABLE_KEY_HASH == 1));
	MESSAGE("FORK_BACKEND_BYTE_HASH=", FORK_BACKEND_BYTE_HASH);
	CHECK((FORK_BACKEND_BYTE_HASH == 0 || FORK_BACKEND_BYTE_HASH == 1));
	MESSAGE("FORK_BACKEND_HASH_TAGS=", FORK_BACKEND_HASH_TAGS);
	CHECK((FORK_BACKEND_HASH_TAGS == 0 || FORK_BACKEND_HASH_TAGS == 1));
}

#if FORK_BACKEND_ALLOCATOR
// Which allocations mimalloc owns: with 1 (D-01) only Memory:: calls, so a raw malloc block is
// the C runtime's; with 3 the process-wide override owns malloc, operator new and the blocks
// glibc allocates inside itself (realpath; mimalloc does not export realpath on Linux).
TEST_CASE("[Fork][Backends] Allocator scope") {
	constexpr bool override_all = FORK_BACKEND_ALLOCATOR == 3;
	void *raw = malloc(64);
	CHECK(mi_any_heap_contains(raw) == override_all);
	free(raw);
#if FORK_BACKEND_ALLOCATOR == 3
	void *object = ::operator new(64);
	CHECK(mi_any_heap_contains(object));
	::operator delete(object);
	char *resolved = realpath(".", nullptr);
	REQUIRE(resolved != nullptr);
	CHECK(mi_any_heap_contains(resolved));
	free(resolved);
#endif
}
#endif // FORK_BACKEND_ALLOCATOR

} // namespace TestForkBackends
