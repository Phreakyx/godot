---
type: Finding
title: Container contracts and the backend switch
description: tests/fork/ pins today's HashMap, AHashMap, HashSet, RBMap, RBSet, List, CowData, String, Packed*Array, Array, Dictionary and StringName behavior that every fork backend must keep, static_asserts pin the layouts the C# glue reads, and core/templates/fork_backends.h builds a same-source control and candidate side by side; no suspected bug found.
tags: [core, containers, testing, phase-1]
status: stable
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-20T00:00:00Z }
---

# Why

A faster container backend is accepted only when it keeps the behavior
engine code relies on and beats a control built from the same source. The
contract tests are that behavior, written down before the first candidate;
the backend switch is the control. Both exist before any backend changes.

# The backend switch (`core/templates/fork_backends.h`)

* One macro per container family, `FORK_BACKEND_<FAMILY>`. Value 0 is
  upstream's code: the control build and the merge anchor. Other values
  are fork candidates, placed next to upstream's code, never in its place.
* A family gets its macro with its first candidate. `FORK_BACKEND_SELFTEST`
  is read by no container: it proves that a define reaches every
  translation unit and the suffixed binary. Each macro's range is checked
  with `#if`/`#error` (the header is also included from C). The Phase 3
  candidates are in the table below.
* An accepted candidate becomes the default with upstream's code kept under
  the 0 branch; a rejected candidate is deleted.
* Same source, two binaries: `devtools/build/build.ps1 -Tests -Variant
  {name} -Defines "{MACRO}=1"` passes SCons `cppdefines` and `extra_suffix`.
  `extra_suffix` lands in the object, library and program suffixes, so the
  candidate never touches the control's objects
  (`bin/godot.windows.editor.x86_64.{name}.tests.console.exe` next to
  `bin/godot.windows.editor.x86_64.tests.console.exe`). `-Dev` builds with
  `dev_build=yes` (`DEV_ASSERT` compiled in) into
  `bin/godot.windows.editor.dev.x86_64.tests.console.exe`.
* `--test --test-case="*Fork*Backends*"` prints the value each binary was
  built with.

# Phase 3 candidates

Every macro defaults to 0, upstream's code. All of them on at once (the
`allhash` tests build: `FORK_BACKEND_ALLOCATOR=1`, `HASHMAP_POOL=1`,
`HASH_POW2=1`, `TABLE_KEY_HASH=1`, `BYTE_HASH=1`, `HASH_TAGS=1` and
`HASHMAP_SMALL=8`) pass the full suite, and the Fork cases pass in its dev
twin (2026-11-22, MSVC).

| Macro | Values | Container | What changes | Pins that cover it | Entry |
| --- | --- | --- | --- | --- | --- |
| `FORK_BACKEND_ALLOCATOR` | 0, 1, 3 (Linux only) | `Memory::` under every container | mimalloc under `alloc_static` and friends (1), or for the whole process (3) | `[Fork][Memory]`, `[Fork][Backends] Allocator scope`, the full suite | [mimalloc](/project/mimalloc-allocator.md) |
| `FORK_BACKEND_HASHMAP_POOL` | 0, 1 | HashMap | Nodes from a per-map pool of pages that never move | HashMap pointer, page-boundary, move and node-memory pins | [node pool](/project/hashmap-node-pool.md) |
| `FORK_BACKEND_HASH_POW2` | 0, 1, 2 | HashMap, HashSet | Power-of-two capacities and a mask; the table hash mixed once (1 multiply-xorshift, 2 `hash_fmix32`) | One-hash, empty-table and key-type pins | [pow2 mixer](/project/hash-pow2-mixer.md) |
| `FORK_BACKEND_TABLE_KEY_HASH` | 0, 1 | HashMap, AHashMap, HashSet (default hasher) | StringName keys by data pointer, pointer keys by one multiply, in `hash_table_key()` | `[Fork][Hash]` public outputs, key-type pins | [table-key hash](/project/table-key-hash.md) |
| `FORK_BACKEND_BYTE_HASH` | 0, 1 | String and CharString table keys | rapidhash V3 in place of wyhash below 256 bytes | `[Fork][Hash]`, `test_hash_buffer.cpp`, key-type pins | [rapidhash](/project/rapidhash-byte-hash.md) |
| `FORK_BACKEND_HASH_TAGS` | 0, 1 | AHashMap, HashSet | One control byte per slot, matched 16 at a time (SSE2, SWAR) | AHashMap and HashSet one-hash, growth-point and empty-table pins, `[Fork][HashTags]` | [tag probing](/project/hash-simd-tags.md) |
| `FORK_BACKEND_HASHMAP_SMALL` | 0, 1 to 16 (the threshold) | HashMap | No index at or below the threshold, list walk; index built from the list past it, kept on erase, dropped by `clear()` | The five threshold pins, node-memory pin, every HashMap pin | [small map](/project/hashmap-small-map.md) |

HASH-02 (`FORK_BACKEND_HASHMAP_SLOTS`) was rejected before it was built
([slot array](/project/hashmap-slot-array.md)).

# What the contracts pin

Cases are named `[Fork][<Container>] ...`; `--test --test-case="*Fork*"`
runs them all. They pass in the production tests build and in the dev
build.

**HashMap** (`tests/fork/test_fork_hash_contracts.cpp`):
* Iteration follows insertion order; `last()` is the last inserted.
* `insert(k, v, true)` puts a new element first; for an existing key the
  flag is ignored and the element keeps its place.
* `sort()` orders by key; `sort_custom<C>()` keeps the insertion order of
  elements that compare equal (stable merge sort).
* `replace_key()` keeps the element's position and its address.
* A `getptr()` pointer stays valid with its value through growth to 10,000
  keys and erase of other keys.
* Erasing other elements while iterating: an already visited one changes
  nothing, a not yet visited one is skipped. `remove(it)` of the current
  element is safe when the next iterator is taken first.
* Inserting an existing key keeps one element at its position, at the same
  address, and replaces the value; the size does not change.
* Erase twice returns true then false. Empty map: `begin() == end()`,
  `last() == end()`, `getptr()` is nullptr, `erase()` returns false, also
  after the last element was erased. One element: `begin() == last()`.
* Across the small-map threshold (`FORK_SMALL_THRESHOLD_FOR_TESTS`, 8, the
  HASH-07 pins, [small map](/project/hashmap-small-map.md)): pointers and
  order hold while a map grows past it one insert at a time (int, int64,
  String keys); erasing back below it and inserting again; `reserve()` past
  it before the first insert; erase, `remove()` and `replace_key()` below
  it; `clear()` past it, then inserts. A one-element map with the macro set
  allocates only its node.

**AHashMap**:
* Iteration follows insertion order while nothing is erased; `get_index()`
  is the position.
* `erase()` moves the last element into the erased index (its address
  becomes the hole's address); the other elements keep index and address.
  Erasing the last element moves nothing. Growth reallocates the element
  array, so element pointers are not stable across inserts.
* `erase_by_index()` follows the same rule and returns false for an index
  at or past `size()`. `get_index()` of a missing key is -1.
* `insert_new()` appends; `replace_key()` keeps the index.
* Existing key, erase twice, empty and single-element cases as for HashMap.
* With one hash for every key (a constant hasher) all keys are found and
  erased, and the dense order follows erase-moves-last; every key is found
  after each insert up to and past the growth points; an empty, reserved,
  copied, moved or emptied map finds nothing, and allocates nothing before
  the first insert (the HASH-06 pins, [tag probing](/project/hash-simd-tags.md)).

**HashSet**:
* Iteration follows insertion order while nothing is erased.
* `erase()` moves the last key into the hole; erasing the last key moves
  nothing. The set has no public index API.
* `insert()` of an existing key returns an iterator to it and changes
  neither size nor order. Erase twice, empty and single-element cases.
* The same one-hash, growth-point and empty-table pins as AHashMap.

**RBMap** (`tests/fork/test_fork_tree_list_contracts.cpp`):
* Iteration follows the comparator (default and a custom one, in
  `front()`/`next()` order and in range-for).
* `front()->prev()` and `back()->next()` are nullptr; `prev()` walks back.
* An `Element*` stays valid through inserts and erases of other elements.
* `find_closest(k)` is the greatest key not above `k` (nullptr below all);
  `lower_bound(k)` is the first key not below `k` (nullptr above all).
* Inserting a key equal under the comparator returns the existing element,
  keeps its stored key and replaces the value.
* Empty map, also after erasing the last element: `find`, `front`, `back`,
  `find_closest` and `lower_bound` return nullptr, `erase()` returns false.

**RBSet**: comparator order, ends, element stability, `lower_bound()`,
equal insert returning the existing element (stored value kept) and the
empty cases, as for RBMap. RBSet has no `find_closest()`.

**List**:
* Erasing the current element during iteration (`E->erase()` or
  `list.erase(E)`) is O(1) and the loop continues from a saved `next()`.
* `insert_before()`/`insert_after()` link at the element; a null position
  appends for both.
* Duplicates are kept; `erase(value)` removes the first match only.
* An `Element*` stays valid through inserts and erases of other elements.
* `sort()` and `sort_custom<C>()` keep the order of equal elements.
* Empty list, also after erasing the last element: `front()` and `back()`
  are nullptr.

**CowData and Vector** (`tests/fork/test_fork_cow_variant_contracts.cpp`):
* A copy shares the buffer (same `ptr()`) until the first write, which
  detaches it; the other copy keeps its values.
* A detached buffer is not copied again: a second write keeps `ptr()`.
* An empty or cleared Vector has size 0 and `ptr()` nullptr.

**String**:
* `ptr()[length()]` is 0 and `size()` is `length() + 1`.
* An empty String has length 0, size 0 and `ptr()` nullptr (also
  `String("")`); `get_data()` points to a 0 character, never nullptr.
* `length()` counts UTF-32 code points: "a", e-acute, euro sign and an
  emoji from UTF-8 give 4.
* No normalization: precomposed U+00E9 and "e" + U+0301 are different
  strings of length 1 and 2.

**Packed arrays**: a Packed array put in a Variant and read back shares
the buffer; a write detaches the reader only, the Variant still shares
the original. Empty Packed arrays have `ptr()` nullptr, also through a
Variant.

**Array**: copies share one array (same `id()`, a push through one is
seen through the other); `duplicate()` gives a new one.

**Dictionary**:
* String "a" and StringName &"a" are one key in both orders; the size stays
  1, the value is replaced and the key type inserted first is kept.
* int 1 and float 1.0 are two different keys.
* NaN is one key: inserted twice the size is 1, and `has(NaN)` finds it.
* Iteration follows insertion order; assigning an existing key keeps its
  place, erasing and inserting it again moves it to the end.
* A `next()` loop whose cursor is a copy of the last key (what GDScript's
  `for k in dict` does, `gdscript_vm.cpp`): erasing the current key ends
  the loop, an erased key not yet visited is skipped, an inserted key is
  visited. `modules/gdscript/tests/scripts/runtime/fork/dictionary_iteration_mutation.gd`
  pins the same in GDScript, with an `.out` file generated by the engine.
* An empty Dictionary: `next(nullptr)` is nullptr, `keys()` is empty.

**StringName**:
* The same text from `const char *` and from `String` gives one
  `data_unique_pointer()`.
* An empty StringName has `data_unique_pointer()` nullptr and equals
  `StringName("")` and `StringName(String())`.
* Threads: 8 threads each create and destroy StringNames of 64 texts
  20,000 times while the main thread holds half of them; afterwards every
  text gives the same pointer on the main thread as in every worker. The
  case runs the stress three times, and passed three runs in a row.

**Public hash outputs** (`tests/fork/test_fork_hash_outputs.cpp`,
`[Fork][Hash]`, D-11): literal values recorded from the control before any
hash candidate changed code (2026-11-22, `c1753151ec`): `String::hash()` of
"", "a", "Godot" and a 300-character string; `StringName::hash()` of
"Godot" and of an empty StringName; `Variant::hash()` of 42, 1.5, "Godot",
`Vector2(1, 2)`, `[1, "a"]` and `{"a": 1}`; `HashMapHasherDefault::hash()`
of `int32_t(-7)`, `int64_t(1) << 40`, `UINT64_MAX`, the pointer 0x1000,
String and StringName "Godot" and `Vector3(1, 2, 3)`. They feed API
fingerprints, script `hash()` and saved data; every table-hash candidate
(byte hash, StringName and pointer table hash, power-of-two mixer) must pass
them unchanged.

# Layouts the C# glue reads

The C# glue (`modules/mono/glue/GodotSharp/GodotSharp/Core/NativeInterop/InteropStructs.cs`)
reads engine memory directly. It finds the CowData header with offsets
from `godotsharp_initialize_marshaling_information()` in
`modules/mono/glue/runtime_interop.cpp`, which repeats the CowData formula
with `uint64_t`, and mirrors the leading fields of `ArrayPrivate` and
`DictionaryPrivate`. A layout change there breaks C# at run time, so it now
fails the build instead:

* `core/templates/cowdata.h`: the reference count is a 64-bit
  `SafeNumeric` at offset 0, and `CAPACITY_OFFSET`, `SIZE_OFFSET` and
  `DATA_OFFSET` equal the `runtime_interop.cpp` formula. The asserts sit in
  `class CowData` outside any `#ifdef`, so editor and template builds both
  check them (the GDExtension size asserts in `extension_api_dump.cpp` are
  editor-only).
* `core/variant/array.cpp`: `SafeRefCount` is 4 bytes at offset 0, the
  `Vector<Variant>` (write proxy plus data pointer, two pointers) at
  `sizeof(void *)`, `read_only` at `3 * sizeof(void *)`.
* `core/variant/dictionary.cpp`: `refcount` at 0, `read_only` at
  `sizeof(void *)`.

The asserts compare formulas, never literal offsets: `alignof(max_align_t)`
is 8 with MSVC and 16 with GCC and Clang on x86-64, so `DATA_OFFSET` is 24
on Windows and 32 on Linux. `offsetof` on these non-standard-layout structs
is wrapped in Godot's GCC and Clang warning macros for
`-Winvalid-offsetof`.

Checked on 2026-11-20: MSVC builds pass, and a GCC 16 (MinGW-w64) syntax
check of copies with a changed layout (`USize` 32-bit, `refcount` and
`read_only` swapped in `DictionaryPrivate`, a field after the array in
`ArrayPrivate`) failed every time with "static assertion failed"; the real
files pass under GCC with no warning. `tests/fork/test_fork_layouts.cpp`
reads a live String and PackedInt32Array header with the C# offsets
(refcount 1, then 2 after a copy; capacity at least size; size is
`length() + 1` for String, 5 for 5 elements) and finds Vector's data
pointer right after its write proxy. Only 64-bit targets are checked.
On Linux (GCC and Clang, 2026-11-21) the asserts compile and the
layout reads pass with `DATA_OFFSET` 32 ([Linux builds](/project/linux-build.md)).

When upstream changes the CowData header or these structs, update the C#
structs, the formula and the asserts together; the release notes then say
"new export templates required".

# Thread safety

HashMap, AHashMap and HashSet carry no thread-safety guarantee; neither do
RBMap, RBSet and List. The contracts use them single-threaded only, and a
backend need not add any.
StringName is the exception: its global table is locked, and the threaded
case above pins that.

# Suspected bugs

None found. Every case passed with the behavior read from the code. Two
behaviors that look surprising are pinned as intended, not as bugs:
AHashMap and HashSet lose insertion order on erase (last element moved
into the hole, by design), and `List::insert_before(nullptr, v)` appends. The
CowData, String and Variant container cases found none either. Pinned as
intended: int 1 and float 1.0 are different Dictionary keys, NaN is one
key (`hash_compare` treats NaN as equal), the first key type of a String or
StringName key is kept, and erasing the current key ends a `for k in dict`
loop without an error.

# Source files covered

`core/templates/fork_backends.h`, `core/templates/hash_map.h`,
`core/templates/a_hash_map.h`, `core/templates/hash_set.h`,
`core/templates/rb_map.h`, `core/templates/rb_set.h`,
`core/templates/list.h`, `core/templates/sort_list.h`,
`core/templates/cowdata.h`, `core/templates/vector.h`,
`core/string/ustring.h`, `core/string/string_name.cpp`,
`core/variant/array.cpp`, `core/variant/dictionary.cpp`,
`modules/mono/glue/runtime_interop.cpp`,
`modules/mono/glue/GodotSharp/GodotSharp/Core/NativeInterop/InteropStructs.cs`;
tests in `tests/fork/` and `modules/gdscript/tests/scripts/runtime/fork/`; build switches in `devtools/build/build.ps1` and
`devtools/common.ps1`. Procedure: [CUSTOM_BUILD.md](../../CUSTOM_BUILD.md)
"Test".
