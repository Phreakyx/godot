---
type: Proposal
title: Godot core designs against current C++ practice
description: A review of thirteen Godot core designs (CowData, UTF-32 String, HashMap, List, RBMap, Variant, the object model, RefCounted, StringName, Memory, threading, error handling, upstream's C++ rules) against current C++ practice, WG21 papers and Agner Fog's optimization manuals - what each does, whether the fork can change it under its compatibility rules, the merge cost, and whether it is worth a measured candidate. Most are not dated, or are fixed upstream, or are pinned by GDExtension and C#; the shortlist is the allocator with a peak-memory fix, two Phase 3 hash follow-ups, fewer refcount operations at call sites and StringName without the global lock.
tags: [core, containers, design-review, cow, hashing, threading, memory, compatibility, msvc, gcc, clang]
status: draft
generated: { by: claude_code/claude-opus-5-5, at: 2026-11-23T00:00:00Z }
sources:
  - id: shares
    resource: family-shares.md
    title: Container family shares and build-or-skip decisions (i9-14900K, MSVC, GCC, Clang)
  - id: alloc
    resource: alloc-vs-pool.md
    title: ALLOC-02 - mimalloc against the HashMap node pool
  - id: tkh
    resource: table-key-hash.md
    title: StringName and pointer table hashes (HASH-05)
  - id: small
    resource: hashmap-small-map.md
    title: Small-map HashMap without an index (HASH-07)
  - id: n2668
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2008/n2668.htm
    title: "N2668, Concurrency Modifications to Basic String (WG21, 2008-06-12)"
  - id: libstdcxx-abi
    url: https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html
    title: libstdc++ manual, Dual ABI
  - id: qt-sharing
    url: https://doc.qt.io/qt-6/implicit-sharing.html
    title: Qt 6, Implicit Sharing
  - id: swift-utf8
    url: https://www.swift.org/blog/utf8-string/
    title: Swift blog, UTF-8 String (2019-03-20)
  - id: swisstables
    url: https://abseil.io/about/design/swisstables
    title: Abseil, Swiss Tables design notes
  - id: absl-containers
    url: https://abseil.io/docs/cpp/guides/container
    title: Abseil, Containers guide (flat_hash_map, node_hash_map, btree_map)
  - id: boost-unordered
    url: https://www.boost.org/doc/libs/latest/libs/unordered/doc/html/unordered/intro.html
    title: Boost.Unordered, Introduction
  - id: f14
    url: https://github.com/facebook/folly/blob/main/folly/container/F14.md
    title: Folly, F14 hash tables
  - id: stroustrup-lists
    url: https://isocpp.org/blog/2014/06/stroustrup-lists
    title: Stroustrup, "Are lists evil?" (isocpp.org, 2014-06-06)
  - id: p0429
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0429r9.pdf
    title: "P0429R9, A Standard flat_map (WG21, 2022-06-17)"
  - id: draft-flatmap
    url: https://eel.is/c++draft/flat.map.overview
    title: C++ working draft, [flat.map.overview]
  - id: p0088
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0088r3.html
    title: "P0088R3, Variant: a type-safe union for C++17 (v8) (WG21, 2016-06-23)"
  - id: jsc-value
    url: https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/JSCJSValue.h
    title: WebKit JavaScriptCore, JSCJSValue.h (NaN-encoded values)
  - id: pr103708
    url: https://github.com/godotengine/godot/pull/103708
    title: "godotengine/godot#103708, Optimize Object::cast_to by assuming no virtual and multiple inheritance"
  - id: pr110763
    url: https://github.com/godotengine/godot/pull/110763
    title: "godotengine/godot#110763, Use AncestralClass to speed up Object::cast_to when possible"
  - id: itanium-abi
    url: https://itanium-cxx-abi.github.io/cxx-abi/abi.html
    title: Itanium C++ ABI, 2.5.2 Virtual Table Components and Order
  - id: p2996
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/n5015.html
    title: "N5015, Editors' Report (applies P2996R13, Reflection for C++26, adopted 2025-06)"
  - id: p0468
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0468r1.html
    title: "P0468R1, An Intrusive Smart Pointer (WG21, 2018-06-19)"
  - id: boost-atomic
    url: https://www.boost.org/doc/libs/latest/libs/atomic/doc/html/atomic/usage_examples.html
    title: Boost.Atomic, Usage examples (reference counting)
  - id: pr118678
    url: https://github.com/godotengine/godot/pull/118678
    title: "godotengine/godot#118678, Fix race in RefCounted::unreference()"
  - id: n3778
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3778.html
    title: "N3778, C++ Sized Deallocation (WG21, 2013-09-27)"
  - id: p0035
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0035r4.html
    title: "P0035R4, Dynamic memory allocation for over-aligned data (WG21, 2016-06-21)"
  - id: draft-memres
    url: https://eel.is/c++draft/mem.res
    title: C++ working draft, [mem.res] memory resources (from N3916, Polymorphic Memory Resources r2)
  - id: butenhof
    url: https://groups.google.com/d/topic/comp.programming.threads/tcrTKnfP8HI
    title: Butenhof, "Re - recursive mutexes", comp.programming.threads, 2005-05-17
  - id: absl-sync
    url: https://abseil.io/docs/cpp/guides/synchronization
    title: Abseil, Synchronization guide
  - id: cpp-mappings
    url: https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
    title: C/C++11 mappings to processors (University of Cambridge)
  - id: draft-races
    url: https://eel.is/c++draft/intro.races
    title: C++ working draft, [intro.races] data races
  - id: p1135
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1135r6.html
    title: "P1135R6, The C++20 Synchronization Library (WG21, 2019-07-19)"
  - id: p0323
    url: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0323r12.html
    title: "P0323R12, std::expected (WG21, 2022-01-07)"
  - id: draft-expected
    url: https://eel.is/c++draft/expected.object.obs
    title: C++ working draft, [expected.object.obs]
  - id: godot-cpp-rules
    url: https://contributing.godotengine.org/en/latest/development/engine/cpp_usage_guidelines.html
    title: Godot contributing guide, C++ rules and guidelines
  - id: godot-faq
    url: https://docs.godotengine.org/en/latest/about/faq.html
    title: Godot docs, Frequently asked questions (STL, exceptions)
  - id: godot-core-types
    url: https://docs.godotengine.org/en/latest/engine_details/architecture/core_types.html
    title: Godot docs, Core types (containers)
  - id: fog-cpp
    url: https://www.agner.org/optimize/
    title: Agner Fog, Optimizing software in C++ (last updated 2025-07-26)
  - id: fog-uarch
    url: https://www.agner.org/optimize/
    title: Agner Fog, The microarchitecture of Intel, AMD and VIA CPUs (last updated 2025-09-20)
  - id: fog-tables
    url: https://www.agner.org/optimize/
    title: Agner Fog, Instruction tables (last updated 2025-09-20)
---

# Scope

The question: which of Godot core's designs are dated the way copy-on-write
`std::string` became after C++11, can the fork change each one without
breaking compatibility, and is it worth a measured candidate. Research only:
nothing was built or measured for this entry.

The fork's rules decide "can it change": public container APIs,
GDExtension builtin sizes and script-visible behavior stay; C# reads the
CowData header and the leading fields of `ArrayPrivate` and
`DictionaryPrivate` ([container contracts](/project/container-contracts.md));
C++20, with C++23/26 only behind feature-test macros; exceptions off. The
milestone plan already rules out SSO or UTF-8 String storage, B-tree or flat
RBMap, dense HashMap or Dictionary storage, dropping COW or atomic
refcounts, third-party maps as backends and `std::format`. Those are not
argued again here; where an internal-only use could take the modern form,
that is said. Requirement IDs (ALLOC-, HASH-, COW-, VAR-, TREE-, CALL-) are
the milestone plan's.

"Worth a candidate" follows the measured shares ([family shares](/project/family-shares.md))
and the Phase 3 results on MSVC. Where a rule in Agner Fog's manuals and a
measurement disagree, the measurement decides. Merge cost follows
[CUSTOM_BUILD.md](../../CUSTOM_BUILD.md) "Diverged core files": every
upstream file the fork touches is reviewed at each release sync, so a change
in a file that already diverges costs little, and a change spread over
hundreds of upstream call sites costs a conflict at every sync.

Counts of uses below are greps over `core`, `scene`, `servers`, `editor`,
`main`, `modules`, `drivers` and `platform` (no `thirdparty`); they are
approximate.

# Summary

| # | Pattern | What Godot does | Current practice | Can the fork change it | Merge cost | Worth a candidate |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | CowData (Vector, String, Packed*Array) | Copy-on-write buffer behind one pointer; atomic refcount, capacity and size in a heap header; explicit write accessors | C++11 rules out COW `basic_string` (N2668), GCC 5 dropped it; Qt keeps implicit sharing | Public layout: no. Internal uses: yes (`LocalVector`) | Low inside `cowdata.h`; high for call-site changes | Already planned: COW-01..04. Fewer copies at call sites (CALL-07): shortlist 4 |
| 2 | String as UTF-32 | `CowData<char32_t>` | UTF-8 storage, small strings inline (Swift 5) | No | - | No (COW-03 covers conversions) |
| 3 | HashMap | Robin-hood index (hash array, node-pointer array) over separately allocated nodes in insertion order | Flat SIMD tables where elements may move; node variants (Abseil, Boost, F14) where pointers must stay | Storage: no. Internal uses: yes (`AHashMap`) | Low (`hash_map.h` already diverges) | Already tried: HASH-01..07. Two follow-ups: shortlist 2, 3 |
| 4 | `List<T>` | Doubly linked, one allocation per element | Contiguous by default (Stroustrup; upstream's own guide) | `List<T> *` APIs: no. About 360 local-only Lists: internal uses only | High (hundreds of upstream files) | No: trees/lists at most 1.3% (TREE-01 skip); CALL-06 |
| 5 | RBMap, RBSet | Red-black nodes, `Element*` handles | B-trees; `std::flat_map` (C++23, P0429) | Backend: no. Internal uses only | High (call sites) | No: at most 1.3% (TREE-01 skip) |
| 6 | Variant | Type tag plus 16-byte union, 24 bytes; switch dispatch; validated and ptrcall tables | `std::variant` (index plus storage); NaN-boxing (8 bytes) | No (GDExtension size, C# mirror) | - | No; VAR-01..04 cover the containers |
| 7 | Object model | GDCLASS identity and ancestry bits for `cast_to`; StringName-keyed ClassDB; virtual MethodBind; C++ RTTI compiled in | `dynamic_cast`; C++26 static reflection (P2996) | Public: no. RTTI off: possible, size only | Medium (24 `dynamic_cast` sites) | No: upstream already replaced `dynamic_cast` (#103708, #110763) |
| 8 | RefCounted, `Ref<T>` | Intrusive count, binding hooks at the 1-2 transitions; `unreference()` three locked operations since #118678 | Intrusive pointer for self-counted objects (P0468, Boost) | Already the current form | - | No (CALL-07 covers the traffic) |
| 9 | StringName | Interned table of 65,536 chains under one global mutex | No primary source checked | Internal: yes (table is private) | Medium (`string_name.cpp` not yet diverged) | Already planned: VAR-03, CALL-04; shortlist 5 |
| 10 | Memory | `malloc` plus a size header and two global atomics per call in `DEBUG_ENABLED` builds; alignment by over-allocation | Sized delete (N3778), aligned new (P0035), `std::pmr` (C++17) | Internal: yes | Low (`memory.cpp` already diverges) | Already tried: ALLOC-01/02, shortlist 1. Sized delete, aligned new, pmr: no |
| 11 | Threading | `Mutex` is recursive by default; atomics acquire/release/acq_rel; refcount copies by a CAS loop | Non-recursive by default; relaxed increment, release/acquire decrement; C++20 semaphores (P1135) | Internal: yes, lock by lock | High (about 140 `Mutex` declarations) | No (no lock share measured; the CAS loop costs what a locked add costs) |
| 12 | Error handling | `Error` returns, `ERR_FAIL_*` macros, no exceptions | `std::expected` (C++23, P0323) | Public: no. Internal: behind `__cpp_lib_expected` | High (about 21,000 macro sites) | No |
| 13 | Upstream's C++ rules | No STL containers, exceptions off, a "subset of C++17" | - | The fork is already C++20 | - | Explains rows 1-12 |

# 1. CowData: Vector, String, Packed*Array

**Godot.** `CowData<T>` is one pointer to element 0 of a heap block whose
header holds a `SafeNumeric<uint64_t>` refcount, the capacity and the size
(`core/templates/cowdata.h:76-93`); the header is 24 bytes with MSVC and 32
with GCC and Clang. A copy takes a conditional increment, a CAS loop
(`cowdata.h:531-547`, `core/templates/safe_refcount.h:137-147`). Writes
detach explicitly: `ptrw()` (`cowdata.h:182-185`), `Vector::write[]`
through `VectorWriteProxy` (`core/templates/vector.h:44-50`, `:66`), and the
non-const `String::operator[]` returns a `CharProxy` that detaches only
when assigned to (`core/string/ustring.h:128`, `:351`). C# repeats the
header formula (`modules/mono/glue/runtime_interop.cpp:1564-1576`).

**Practice.** N2668 made element access on `basic_string` safely
concurrent, which rules out COW: under the old rules, taking a non-const
reference could invalidate const references already handed out[^n2668].
libstdc++ kept a COW string until GCC 5.1 and changed its ABI to drop
it[^libstdcxx-abi]. Qt still shares its containers implicitly and detaches
before a write[^qt-sharing]. Godot's COW is Qt's kind: it avoids the trap
N2668 closed, because a plain `operator[]` never hands out a non-const
reference. What it still costs: one locked atomic operation per copy and
one per destruction, a refcount check on every write accessor (a `write[i]` loop
checks per element), `size()` read from the heap header (`cowdata.h:191`),
and no inline buffer, so every non-empty String allocates. These are
inside the CowData family: 23.2% of `script:script` template_release on
Clang, 20.6% on GCC, 8.0% on MSVC, with `_ref`/`_unref` among its
frames[^shares].

**Manuals.** A shared or smart pointer costs nothing on access but extra
on every create, copy and delete (Optimizing software in C++, 7.9)[^fog-cpp];
string classes allocate on every create or modify (9.8)[^fog-cpp]. On the
nearest P-core in the instruction tables (Ice Lake and Tiger Lake; the
manuals have no Raptor Lake entry, and Alder Lake's P-core timings are
"mostly the same" as Ice Lake's, microarchitecture 13[^fog-uarch]),
`LOCK XADD` has a reciprocal throughput of 21 cycles and `LOCK CMPXCHG` 22,
against 7 for an unlocked `XADD`; on Tremont, the Atom core before
Gracemont, both locked forms take about 16[^fog-tables].

**Verdict.** Public layout: no. Internal uses: yes, `LocalVector`
(`core/templates/local_vector.h:53`) is the non-COW form, and upstream's
docs say to use it where cheap copies are not needed[^godot-core-types];
`Vector<` appears 11,539 times, `LocalVector<` 2,663. Candidates: COW-01..04
are planned. Replacing the CAS loop in `_ref()` by a plain increment would
save a load and a branch, not a locked operation (the two cost the same in
the tables), so it is not worth a series on its own. Avoiding the copy is
(CALL-07, shortlist 4).

# 2. String as UTF-32

**Godot.** `CowData<char32_t> _cowdata` (`core/string/ustring.h:281`);
upstream chose UTF-32 for its fixed character size[^godot-core-types].
GDExtension sizes String as one pointer
(`core/extension/extension_api_dump.cpp:167`), and `string_operator_index`
returns a `char32_t *` into the buffer
(`core/extension/gdextension_interface.cpp:1021-1027`).

**Practice.** Swift 5 moved its String from UTF-16 to UTF-8, with up to 15
code units stored inline[^swift-utf8]. The manual's advice for fast strings
is C-style character arrays or a string pool (Optimizing software in C++,
9.8)[^fog-cpp].

**Verdict.** No: out of scope (one-pointer size, `char32_t *` contract).
Byte-oriented work already has `CharString` (UTF-8, `ustring.h:267`) and the
table byte hash; COW-03 (simdutf) covers conversions. No candidate.

# 3. HashMap

**Godot.** Robin-hood probing over two parallel arrays, `uint32_t *_hashes`
and `HashMapElement **_elements`, with prime capacities and `fastmod`
(`core/templates/hash_map.h:76-77`, `:294`); each element is its own
allocation, linked by `next` and `prev` in insertion order (`hash_map.h:46-48`,
`:78-79`). `AHashMap` (`core/templates/a_hash_map.h:47-62`, upstream #92554,
2024) is the dense form: elements in one array, a robin-hood index, no
pointer stability.

**Practice.** Swiss tables keep one control byte per slot (7 bits of the
hash) and compare 16 slots with one SIMD instruction[^swisstables]. Flat
tables invalidate pointers on rehash; Abseil's `node_hash_map` keeps "stable
addresses"[^absl-containers]; Boost's `unordered_node_map` gives pointer
stability under rehashing with open addressing, at some speed
cost[^boost-unordered]; F14NodeMap is "the only F14 variant that doesn't
move its elements"[^f14]. So HashMap is not dated: it is the node variant of
current open-addressing designs, plus an insertion-order list that none of
them has. Its prime-plus-`fastmod` capacity already avoids the division
the manual warns about (27-80 cycles, Optimizing software in C++,
14.5)[^fog-cpp]. Upstream describes HashMap as the defensive, slower map and
AHashMap as the default[^godot-core-types]; engine code still writes `HashMap<`
2,924 times and `AHashMap<` 104 times.

**Verdict.** Storage: no (pointer stability, order, `Variant *` held by
GDExtension, 744 `getptr(` sites). Internal uses: yes, AHashMap where order
and stability are not needed (CALL-06). Candidates: tried on MSVC. The
node pool, power-of-two capacity, SIMD tags and rapidhash lost
([node pool](/project/hashmap-node-pool.md), [power of two](/project/hash-pow2-mixer.md),
[SIMD tags](/project/hash-simd-tags.md), [rapidhash](/project/rapidhash-byte-hash.md));
the 2026-11-15 dense adapter was 1.3-2.6x slower and 50-57% bigger
([dropped](/project/core-template-storage.md)). The cheaper StringName and
pointer hash and the small map won their targets and failed on regressions
with known causes[^tkh][^small]: shortlist 2 and 3.

# 4. List

**Godot.** `List<T>` (`core/templates/list.h:48`) is doubly linked; each
element holds `next_ptr`, `prev_ptr` and the list's `_Data` (`list.h:52-59`)
and is allocated on insert (`list.h:295`).

**Practice.** "Use a contiguous representation unless there is a good
reason not to"[^stroustrup-lists]. The manual: linked lists are "very
inefficient", contiguous arrays "usually much faster", each element is its
own allocation and the elements are not contiguous (Optimizing software in
C++, 9.6, 9.7)[^fog-cpp]. Upstream agrees: List is generally slower and
should not be used in new code unless it avoids conversions[^godot-core-types].

**Uses.** 963 local `List<T> x;` declarations in `.cpp` files. About 604 of
them are passed by address within ten lines, mostly to `List<T> *` APIs
(`get_property_list()`, method and signal lists; 616 such `p_`/`r_`
parameters in headers), which modules call. About 360 are local only. The
most common element types are String (240), PropertyInfo (180) and
StringName (140).

**Verdict.** `List<T> *` APIs: no. Local-only Lists: yes, `LocalVector`,
where no List contract (O(1) erase through an `Element`, stable `Element *`)
is used; that is CALL-06, and it touches hundreds of upstream files, so it
is best sent upstream. Candidate: no. The manual's rule does not survive the
measurement here: trees/lists are at most 1.3% of any workload, a 0.4% max
gain against an 8.31% floor[^shares]. D013 already moved
`DependencyTracker::update_end()` off List
([decisions](../../docs/core-modernization/decisions.md)).

# 5. RBMap and RBSet

**Godot.** Red-black trees, one allocation per node with `left`, `right`,
`parent`, `_next`, `_prev` and a color (`core/templates/rb_map.h:52`, `:60-68`),
`Element *` handles (70 `::Element` uses).

**Practice.** Abseil's B-tree maps have lower memory overhead and are more
cache friendly than binary trees[^absl-containers]. `std::flat_map`
(C++23, P0429R9) is a container adaptor over sorted key and value
sequences; it does not meet the associative containers' iterator
invalidation rules, and a single insert or erase is linear[^p0429][^draft-flatmap].
The manual keeps binary trees for ordered data with range searches
(Optimizing software in C++, 9.7)[^fog-cpp]. Upstream says RBMap's
performance benefits are not established and prefers other
types[^godot-core-types].

**Verdict.** Backend: no (`Element *` stability). Internal uses only: 82
local declarations in `.cpp` files and 75 members; a build-once table could
be a sorted `LocalVector`. Candidate: no. 1.3% at most, and RBMap and RBSet
are within 0.9-1.5x of `std::map` and `gtl::btree` in the micro
tier[^shares].

# 6. Variant

**Godot.** A `Type` tag and a 16-byte union (`core/variant/variant.h:168`,
`:255-267`): 24 bytes in single precision, 40 in double
(`extension_api_dump.cpp:202`). Transform2D, AABB, Basis, Transform3D and
Projection live in pools (`core/variant/variant.cpp:1200-1234`). Generic
code switches on the type; hot paths use validated operator, method,
constructor and member tables (`variant.h:618-737`) and `VariantInternal`
(`core/variant/variant_internal.h:45`), which the GDScript VM's typed
opcodes call (`modules/gdscript/gdscript_function.h:205-208`).

**Practice.** `std::variant` is the same idea, an index plus storage, and
can be valueless by exception[^p0088]. NaN-boxing stores values in the
unused NaN space of a double, 8 bytes[^jsc-value]. The manual: a `switch`
over consecutive labels compiles to a jump table, and a mispredicted branch
costs about 15-25 cycles (Optimizing software in C++, 7.12)[^fog-cpp];
`Variant::Type` is a dense enum, so the switch is the cheap kind, and the
validated tables remove it from typed code.

**Verdict.** No. The size is pinned by GDExtension and by the C#
`godot_variant` (`modules/mono/glue/GodotSharp/GodotSharp/Core/NativeInterop/InteropStructs.cs:180-237`);
NaN-boxing cannot hold a Vector4 or a Quaternion inline; `std::variant`'s
layout is the library's, so it cannot match the C# mirror. Not dated.
VAR-01..04 cover Dictionary, its String keys, StringName and typed Array.

# 7. Object model

**Godot.** `GDCLASS` (`core/object/object.h:245`) gives each class a static
identity pointer and a virtual `is_class_ptr()` chain (`object.h:162-167`).
`cast_to` goes through `derives_from()` (`object.h:640-659`, `:864-880`): a
compile-time answer where it can, 15 ancestry bits for common bases
(`object.h:367-385`, `:435`), the virtual walk otherwise. Upstream replaced
`dynamic_cast` this way in 2025, reporting 8x its throughput[^pr103708], and
added the ancestry bits[^pr110763]. ClassDB keys classes by StringName
(`core/object/class_db.h:185`); MethodBind has virtual `call`,
`validated_call` and `ptrcall` (`core/object/method_bind.h:116-119`), and
GDScript binds to the validated call when it compiles.

C++ RTTI is compiled in: `/GR` on MSVC (`platform/windows/detect.py:389`)
and no `-fno-rtti` on GCC or Clang. It is used by 24 `dynamic_cast` sites
(Callable and script-instance checks, not hot loops) and 2 `typeid` names
in leak messages (`core/templates/rid_owner.h:473`,
`core/templates/paged_allocator.h:170`).

**Practice and manuals.** The manual says to turn RTTI off because it
"adds extra information to all class objects" (Optimizing software in C++,
7.23)[^fog-cpp]; in the Itanium C++ ABI the type_info pointer is a vtable
entry, not a per-object field[^itanium-abi], so here it costs binary size,
not speed. A virtual call costs a few cycles, 10-20 more when the target
changes (7.22)[^fog-cpp]: MethodBind's calls are virtual, and no profile
has named them. C++26 adds static reflection (P2996R13, adopted
2025-06)[^p2996], which could generate bindings at compile time.

**Verdict.** Public: no (class registration and binds are the extension
API). RTTI off: possible after removing 24 casts, for size only. Reflection:
not under the C++20 baseline; compiler support not checked. No candidate;
signal costs are CALL-02.

# 8. RefCounted and `Ref<T>`

**Godot.** An intrusive `SafeRefCount` in `RefCounted`
(`core/object/ref_counted.h:38-40`) and `Ref<T>` (`ref_counted.h:59`).
`reference()` and `unreference()` call the script instance, extension and
instance-binding hooks when the count crosses 1 and 2
(`core/object/ref_counted.cpp:74-108`); that is how C# and extensions
switch between strong and weak handles, and `reference()` must fail at zero
(`ref_counted.h:51`). Since #118678 (2026) `unreference()` also increments
and decrements a `dereference_count` and spins on it before destroying, to
fix a use-after-free race (`ref_counted.cpp:92-123`)[^pr118678]: three
locked operations per unreference.

**Practice.** The intrusive form is the one WG21 proposed for objects that
carry their own count (`retain_ptr`, P0468R1)[^p0468]; Boost's documented
count increments with a relaxed add and decrements with release, plus an
acquire fence before the delete[^boost-atomic]. The manual prices any
shared pointer by its copies, not its accesses (7.9)[^fog-cpp].

**Verdict.** Not dated, and the hooks make it the only form that works with
the bindings. No candidate. If a profile names `RefCounted::unreference`,
folding `dereference_count` into the count word would make it one locked
operation, and has to keep #118678's fix. Copy traffic is CALL-07.

# 9. StringName

**Godot.** One global table of 65,536 chains with one `BinaryMutex`
(`core/string/string_name.cpp:40-58`), taken for every lookup and insert
(`:231-262`) and for the last unreference (`:131-132`). The name is hashed
before locking. A found entry is taken with a conditional increment
(`:245`): a name whose count just reached zero stays findable until its
owner takes the lock, which is why `SafeRefCount` has a CAS loop at all.
`SNAME` caches each name in a function-local static
(`core/string/string_name.h:223`).

**Practice.** No primary source on interning tables was checked; lookups
without the global lock are the fork's own plan (VAR-03), not a cited
practice. The manual: threads writing to the same cache line "invalidate
each other's caches and cause large delays" (Optimizing software in C++,
10)[^fog-cpp]; the global mutex and the refcount of a popular name are such
lines. The measured workloads are mostly single-threaded, so this is not
measured.

**Verdict.** Internal: yes, the table is private and StringName stays one
pointer. Already planned: VAR-03 and CALL-04. Shortlist 5, after its share
is measured: the i9 profiles give StringName no family of its own (the
superseded [Ryzen profiles](/project/core-cpu-profiles.md) put it at
3.4-4.2%).

# 10. Memory

**Godot.** `memnew` is a placement new through `DefaultAllocator`, then
`postinitialize_handler`; `memdelete` runs `predelete_handler` first
(`core/os/memory.h:109-160`). In every `DEBUG_ENABLED` build (editor, debug
templates) `Memory::alloc_static` adds a 16-byte header holding the size
(`memory.h:53-62`) and updates two global atomic counters per allocation,
reallocation and free (`core/os/memory.cpp:99-100`, `:141-176`, `:256`);
release builds add the header only on request. Aligned allocations
over-allocate and store an offset (`memory.h:72-92`; 5 callers).
`PagedAllocator` serves 39 pools.

**Practice.** Sized deallocation (N3778, C++14) lets an allocator skip the
size lookup on free[^n3778]; aligned new (P0035R4, C++17)[^p0035]; memory
resources and `polymorphic_allocator` (C++17, from N3916)[^draft-memres].
The manual: allocation and deallocation take "much more time than other
kinds of storage", fragment the heap, and the remedy is pooling (Optimizing
software in C++, 9.6, 9.7)[^fog-cpp].

**Measurement against the manual.** Phase 3 measured the manual's remedy
against a better general allocator: the per-map HashMap node pool was
within noise on its target and failed on 82 regression rows, while
mimalloc behind `Memory::` made `script:script` template_release 23.1%
faster and `scene:instance:10000` 38.5% faster, and failed on +56-72%
gdbench peak memory and 30 lookup rows[^alloc].

**Verdict.** Internal: yes. The allocator is already a candidate (ALLOC-01/02,
shortlist 1). Sized delete: no gain with it, because mimalloc's
`mi_free_size` ignores the size in release builds
(`thirdparty/mimalloc/src/alloc/free.c:542-543`), and release builds carry
no Godot header to save. Aligned new: 5 callers, no. `std::pmr`: a virtual
call per allocation, outside the accounting, and `PagedAllocator` already
fills the role; no. The `DEBUG_ENABLED` counters are shared cache lines
(section 9), but the MSVC editor's allocation share is not above the
release share on `scene:instance` (18.8% against 21.1%)[^shares], so they do
not show single-threaded; no candidate until a threaded editor profile
names them.

# 11. Threading

**Godot.** `Mutex` is `std::recursive_mutex` "for general use" and
`BinaryMutex` is `std::mutex`, "handle with care" (`core/os/mutex.h:97-98`);
about 140 `Mutex` and 27 `BinaryMutex` declarations. `RWLock` is a
`shared_mutex` (`core/os/rw_lock.h:45`); `SpinLock` acquires with a CAS and
spins on relaxed loads (`core/os/spin_lock.h:94-114`); `Semaphore` is a
mutex and a condition variable (`core/os/semaphore.h:52-56`); the worker
pool has one queue under one `BinaryMutex`
(`core/object/worker_thread_pool.h:107-110`).

`SafeNumeric` and `SafeFlag` use acquire loads, release stores and acq_rel
read-modify-writes, never `seq_cst` (`core/templates/safe_refcount.h:63-150`).
On x86-64, acquire and release loads and stores are plain `MOV`s[^cpp-mappings],
and the cost is in the locked read-modify-writes (section 1). The one form
stronger than needed is `conditional_increment()`, a CAS loop
(`safe_refcount.h:137-147`), used for every CowData copy
(`cowdata.h:541`): Boost increments a count with a relaxed add, since "new
references to an object can only be formed from an existing
reference"[^boost-atomic]. The loop is needed where a count can reach zero
while still reachable (StringName, section 9; `RefCounted::reference()`); a
CowData being copied holds a reference, and the comment at `cowdata.h:536-537`
says the loop only guards misuse.

**Practice.** Non-recursive by default: `std::mutex`, and Abseil's Mutex
"is not re-entrant"[^absl-sync]; the POSIX threads view is that "a correct
and well understood design does not require recursive mutexes"[^butenhof].
C++20 added `counting_semaphore`, `latch`, `barrier` and atomic wait and
notify (P1135R6)[^p1135]. The manual says synchronization overhead is
"considerable" (Optimizing software in C++, 7.31) and that shared variables
must be `volatile`[^fog-cpp]; the second is pre-C++11 advice: the language
makes any unsynchronized non-atomic conflict undefined
behavior[^draft-races], and Godot correctly uses `std::atomic`.

**Verdict.** Internal: yes, lock by lock, each needing proof it is never
taken re-entrantly, across about 140 upstream declarations. No candidate:
no lock is a measured share on the i9 (the Ryzen lock cost was RID
`owns()`, made lock-free by D012), and replacing the CAS loop buys no
locked operation (section 1). Semaphores and the pool: no share, leave.

# 12. Error handling

**Godot.** The `Error` enum (`core/error/error_list.h:46`), about 21,000
`ERR_FAIL_*` uses, exceptions off (`SConstruct:279`, `:965-971`). Upstream
disables exceptions for performance, binary size and code
complexity[^godot-cpp-rules]; the FAQ says games should not crash, and
errors print and recover[^godot-faq]. The manual agrees: exception
bookkeeping can cost even when nothing throws, so turn it off if it is not
needed (Optimizing software in C++, 7.32)[^fog-cpp].

**Practice.** `std::expected<T, E>` (C++23, P0323R12)[^p0323]. Its
`value()` throws `bad_expected_access` when empty[^draft-expected], so under
`-fno-exceptions` only `operator*` and `has_value()` are usable.

**Verdict.** Script-visible `Error` stays. Internal use behind
`__cpp_lib_expected` is possible; no performance case. `Error` is not
`[[nodiscard]]` (some CowData functions are, `cowdata.h:159-168`); marking
it would be a correctness change, not a candidate.

# 13. Upstream's C++ rules

Upstream disallows the STL "as Godot provides its own data types", turns
exceptions off, limits `auto` and lambdas, and describes the language as "a
subset of C++17"[^godot-cpp-rules]. The FAQ allows "a few exceptions such
as threading primitives" and gives the reasons: large template symbols and
debug binaries, special containers (COW Vector, RID), built-in memory
tracking, "pooled memory" for large arrays, and String's
internationalization[^godot-faq]. Core includes `<atomic>`, `<mutex>`,
`<type_traits>`, `<initializer_list>` and `<bit>`, among others.

Two notes. The pooled-memory reason does not describe this tree: CowData
allocates through `Memory::alloc_static` (`cowdata.h:486`). And many
patterns above follow from these rules, not from age: COW and UTF-32 are
choices for Variant and for i18n, Godot's containers exist so allocations
are tracked. The fork already left "C++17" (`core/typedefs.h:41`); std
containers stay out of the API, but std facilities that do not cross it
are open (for example `std::to_chars`, FMT-02).

# Shortlist, ranked

1. **mimalloc with a peak-memory fix** (ALLOC-02's step 5, the purge-delay
   variant `FORK_BACKEND_ALLOCATOR=2`). Allocation is the largest family
   (34.0% of `scene:instance:10000`, GCC)[^shares]; on MSVC mimalloc gave
   -23.1% on `script:script` template_release, -38.5% on
   `scene:instance:10000` and -12.8% on level load, and failed on +56-72%
   gdbench peak memory and 30 hash-lookup rows[^alloc]. Fork-owned files and
   an already diverged `memory.cpp`: low merge cost. Planned as ALLOC-03
   in Phase 3.1, with the Linux series (ALLOC-04).
2. **StringName by pointer, pointer keys mixed** (the HASH-05 follow-up
   named in its entry). Targets -7.5% (`scene:reparent:10000` editor) and
   -3.8% (`scene:script:10000` template_release); the 1M AHashMap pointer
   rows +125-250% look like poorly mixed aligned pointers[^tkh]. Hash tables
   are 15.7% at most[^shares]. `hashfuncs.h` already diverges. Planned as
   HASH-08 in Phase 3.1.
3. **Small map without the threshold cliff** (a HASH-07 follow-up).
   `scene:script:10000` template_release -5.2%; Dictionary inserts at 16
   elements +144%, where the index is rebuilt from the list[^small]. A
   variant that builds the index cheaply at the threshold, or keeps
   Dictionary's map out of the small path, is the one to measure.
   `hash_map.h` already diverges. Planned as HASH-09 in Phase 3.1.
4. **Fewer refcount operations at hot call sites** (CALL-07, next
   milestone). The CowData family is 23.2% of `script:script` on Clang, with
   `_ref`/`_unref` among its frames[^shares]; each avoided copy saves two
   locked operations, about 21-22 cycles of throughput each on the nearest
   listed P-core[^fog-tables]. Call sites are upstream files: send what works
   upstream.
5. **StringName lookups without the global mutex** (VAR-03). Measure its
   share on the i9 first; it has no family of its own yet. One file pair,
   medium merge cost.

**Leave alone:** UTF-32 String and Variant (pinned, and not dated); the
object model and RTTI (upstream fixed `cast_to`); `Ref<T>`; `Error` and the
macros; List, RBMap and RBSet backends (at most 1.3%); HashMap storage
beyond items 2 and 3; sized delete, aligned new and `std::pmr`; recursive
`Mutex`, semaphores and the worker pool (no measured share); the CAS loop in
`_ref()` on its own.

[^shares]: [Container family shares and decisions](/project/family-shares.md), i9-14900K, MSVC, GCC 16.2.1, Clang 23.1.5.
[^alloc]: [ALLOC-02: mimalloc against the HashMap node pool](/project/alloc-vs-pool.md), MSVC results of 2026-11-22.
[^tkh]: [StringName and pointer table hashes](/project/table-key-hash.md), MSVC results (provisional).
[^small]: [Small-map HashMap without an index](/project/hashmap-small-map.md), MSVC results (provisional).
[^n2668]: N2668, "Concurrency Modifications to Basic String", WG21, 2008-06-12, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2008/n2668.htm
[^libstdcxx-abi]: libstdc++ manual, "Dual ABI": GCC 5.1's new ABI, because C++11 "forbids Copy-On-Write strings", https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html
[^qt-sharing]: Qt 6 documentation, "Implicit Sharing", https://doc.qt.io/qt-6/implicit-sharing.html
[^swift-utf8]: Swift blog, "UTF-8 String", 2019-03-20, https://www.swift.org/blog/utf8-string/
[^swisstables]: Abseil, "Swiss Tables Design Notes", https://abseil.io/about/design/swisstables
[^absl-containers]: Abseil, "Containers" guide, https://abseil.io/docs/cpp/guides/container
[^boost-unordered]: Boost.Unordered, "Introduction", https://www.boost.org/doc/libs/latest/libs/unordered/doc/html/unordered/intro.html
[^f14]: Folly, "F14 Hash Table", https://github.com/facebook/folly/blob/main/folly/container/F14.md
[^stroustrup-lists]: B. Stroustrup, "Are lists evil?", isocpp.org, 2014-06-06, https://isocpp.org/blog/2014/06/stroustrup-lists
[^p0429]: P0429R9, "A Standard flat_map", WG21, 2022-06-17, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0429r9.pdf
[^draft-flatmap]: C++ working draft, [flat.map.overview], https://eel.is/c++draft/flat.map.overview
[^p0088]: P0088R3, "Variant: a type-safe union for C++17 (v8)", WG21, 2016-06-23, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0088r3.html
[^jsc-value]: WebKit, `Source/JavaScriptCore/runtime/JSCJSValue.h`, "We use a NaN-encoded form for immediates", https://github.com/WebKit/WebKit/blob/main/Source/JavaScriptCore/runtime/JSCJSValue.h
[^pr103708]: godotengine/godot#103708, https://github.com/godotengine/godot/pull/103708
[^pr110763]: godotengine/godot#110763, https://github.com/godotengine/godot/pull/110763
[^itanium-abi]: Itanium C++ ABI, 2.5.2 "Virtual Table Components and Order" and 2.9.4, https://itanium-cxx-abi.github.io/cxx-abi/abi.html
[^p2996]: N5015, Editors' Report, applying P2996R13 "Reflection for C++26" (adopted 2025-06), https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/n5015.html
[^p0468]: P0468R1, "An Intrusive Smart Pointer", WG21, 2018-06-19, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0468r1.html
[^boost-atomic]: Boost.Atomic, "Usage examples", reference counting, https://www.boost.org/doc/libs/latest/libs/atomic/doc/html/atomic/usage_examples.html
[^pr118678]: godotengine/godot#118678, "Fix race in RefCounted::unreference()", https://github.com/godotengine/godot/pull/118678
[^n3778]: N3778, "C++ Sized Deallocation", WG21, 2013-09-27, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2013/n3778.html
[^p0035]: P0035R4, "Dynamic memory allocation for over-aligned data", WG21, 2016-06-21, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0035r4.html
[^draft-memres]: C++ working draft, [mem.res], https://eel.is/c++draft/mem.res; origin N3916, "Polymorphic Memory Resources - r2", WG21, 2014-02-14 (listed in SD-1, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2015/sd-1.htm)
[^butenhof]: D. Butenhof, "Re: recursive mutexes", comp.programming.threads, 2005-05-17, https://groups.google.com/d/topic/comp.programming.threads/tcrTKnfP8HI
[^absl-sync]: Abseil, "Synchronization" guide, https://abseil.io/docs/cpp/guides/synchronization
[^cpp-mappings]: "C/C++11 mappings to processors", University of Cambridge, x86 rows, https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
[^draft-races]: C++ working draft, [intro.races] paragraph 17, https://eel.is/c++draft/intro.races
[^p1135]: P1135R6, "The C++20 Synchronization Library", WG21, 2019-07-19, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1135r6.html
[^p0323]: P0323R12, "std::expected", WG21, 2022-01-07, https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p0323r12.html
[^draft-expected]: C++ working draft, [expected.object.obs], https://eel.is/c++draft/expected.object.obs
[^godot-cpp-rules]: Godot contributing guide, "C++ rules and guidelines", https://contributing.godotengine.org/en/latest/development/engine/cpp_usage_guidelines.html
[^godot-faq]: Godot documentation, FAQ, "Why does Godot not use STL?" and "Why does Godot not use exceptions?", https://docs.godotengine.org/en/latest/about/faq.html
[^godot-core-types]: Godot documentation, "Core types", containers table, https://docs.godotengine.org/en/latest/engine_details/architecture/core_types.html
[^fog-cpp]: Agner Fog, "Optimizing software in C++", last updated 2025-07-26, sections as cited, https://www.agner.org/optimize/
[^fog-uarch]: Agner Fog, "The microarchitecture of Intel, AMD and VIA CPUs", last updated 2025-09-20, chapter 13 (Alder Lake), https://www.agner.org/optimize/
[^fog-tables]: Agner Fog, "Instruction tables", last updated 2025-09-20, Ice Lake and Tiger Lake, and Tremont, synchronization instructions, https://www.agner.org/optimize/
