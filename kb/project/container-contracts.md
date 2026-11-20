---
type: Finding
title: Container contracts and the backend switch
description: tests/fork/ pins today's HashMap, AHashMap, HashSet, RBMap, RBSet and List behavior that every fork backend must keep, and core/templates/fork_backends.h builds a same-source control and candidate side by side; no suspected bug found.
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
* A family gets its macro with its first candidate. Today there is one
  macro, `FORK_BACKEND_SELFTEST`, read by no container: it proves that a
  define reaches every translation unit and the suffixed binary. A
  `static_assert` limits it to 0 or 1.
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

**HashSet**:
* Iteration follows insertion order while nothing is erased.
* `erase()` moves the last key into the hole; erasing the last key moves
  nothing. The set has no public index API.
* `insert()` of an existing key returns an iterator to it and changes
  neither size nor order. Erase twice, empty and single-element cases.

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

# Thread safety

HashMap, AHashMap and HashSet carry no thread-safety guarantee; neither do
RBMap, RBSet and List. The contracts use them single-threaded only, and a
backend need not add any.

# Suspected bugs

None found. Every case passed with the behavior read from the code. Two
behaviors that look surprising are pinned as intended, not as bugs:
AHashMap and HashSet lose insertion order on erase (last element moved
into the hole, by design), and `List::insert_before(nullptr, v)` appends.

# Source files covered

`core/templates/fork_backends.h`, `core/templates/hash_map.h`,
`core/templates/a_hash_map.h`, `core/templates/hash_set.h`,
`core/templates/rb_map.h`, `core/templates/rb_set.h`,
`core/templates/list.h`, `core/templates/sort_list.h`;
tests in `tests/fork/`; build switches in `devtools/build/build.ps1` and
`devtools/common.ps1`. Procedure: [CUSTOM_BUILD.md](../../CUSTOM_BUILD.md)
"Test".
