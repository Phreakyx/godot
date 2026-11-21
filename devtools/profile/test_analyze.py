#!/usr/bin/env python3
"""Fixture tests of analyze.py's family attribution and of perf_to_tsv.py: python devtools/profile/test_analyze.py"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402
import perf_to_tsv  # noqa: E402

ROOT = "E:/src/godot/"  # sampler TSVs carry absolute paths; relative() cuts them at the engine's top directories


def frame(function, file="", line=1):
    return f"{function}@{ROOT}{file}:{line}" if file else function


def family(function, file="", module="godot"):
    return analyze.family(module, frame(function, file))


class Families(unittest.TestCase):
    def test_hash_tables(self):
        self.assertEqual(family("HashMap<int,int>::_lookup_pos", "core/templates/hash_map.h"), "hash tables")
        self.assertEqual(family("HashSet<int>::_lookup_key_idx", "core/templates/hash_set.h"), "hash tables")
        self.assertEqual(family("hash_murmur3_one_32", "core/templates/hashfuncs.h"), "hash tables")

    def test_allocation(self):
        self.assertEqual(family("Memory::alloc_static", "core/os/memory.cpp"), "allocation")
        self.assertEqual(family("RtlAllocateHeap", module="ntdll"), "allocation")
        self.assertEqual(family("_malloc_base", "minkernel/crts/ucrt/src/heap/malloc_base.cpp", "ucrtbase"),
                         "allocation")
        self.assertEqual(family("free", module="libc.so.6"), "allocation")
        self.assertEqual(family("_int_malloc", module="libc.so.6"), "allocation")
        self.assertEqual(family("__GI___libc_realloc", module="libc.so.6"), "allocation")
        self.assertEqual(family("RenderingServerDefault::free", "servers/rendering/rendering_server_default.h"),
                         "other")

    def test_formatting_by_function_before_cowdata(self):
        for function in ("String::sprintf", "vformat<int>", "String::num_int64", "String::num_real"):
            self.assertEqual(family(function, "core/string/ustring.cpp"), "formatting", function)
        self.assertEqual(family("grisu2::Grisu2", "thirdparty/grisu2/grisu2.h"), "formatting")

    def test_cowdata(self):
        self.assertEqual(family("String::operator+=", "core/string/ustring.cpp"), "CowData")
        self.assertEqual(family("CowData<int>::_copy_on_write", "core/templates/cowdata.h"), "CowData")
        self.assertEqual(family("Vector<int>::push_back", "core/templates/vector.h"), "CowData")

    def test_variant_containers(self):
        self.assertEqual(family("Dictionary::operator[]", "core/variant/dictionary.cpp"), "Variant containers")
        self.assertEqual(family("Array::push_back", "core/variant/array.cpp"), "Variant containers")
        self.assertEqual(family("StringName::StringName", "core/string/string_name.cpp"), "Variant containers")
        self.assertEqual(family("StringName::operator==", "core/string/string_name.h"), "Variant containers")
        self.assertEqual(family("Variant::hash", "core/variant/variant.cpp"), "Variant containers")

    def test_trees_lists(self):
        self.assertEqual(family("RBMap<int,int>::find", "core/templates/rb_map.h"), "trees/lists")
        self.assertEqual(family("RBSet<int>::insert", "core/templates/rb_set.h"), "trees/lists")
        self.assertEqual(family("List<int>::push_back", "core/templates/list.h"), "trees/lists")

    def test_bench_counter_first(self):
        self.assertEqual(family("fork_alloc_count_add", "core/os/fork_alloc_counters.cpp"), "bench counter")
        self.assertEqual(family("Node::_propagate", "scene/main/node.cpp"), "other")

    def test_waits_are_idle_on_both_systems(self):
        for module, function in (("ntdll", "NtWaitForSingleObject"), ("win32u", "NtUserMsgWaitForMultipleObjectsEx"),
                                 ("libc.so.6", "__futex_abstimed_wait_common"), ("libc.so.6", "epoll_wait"),
                                 ("libc.so.6", "__GI___clock_nanosleep"), ("libc.so.6", "__GI___poll"),
                                 ("libc.so.6", "__pthread_cond_wait"), ("[kernel.kallsyms]", "futex_wait_queue"),
                                 ("libpthread.so.0", "pthread_cond_wait")):
            self.assertTrue(analyze.is_idle(module, [[function]]), (module, function))
        self.assertFalse(analyze.is_idle("libc.so.6", [["malloc"]]))
        self.assertFalse(analyze.is_idle("godot", [["Thread::wait_to_finish"]]))

    def test_shares_sum_and_busiest_thread(self):
        rows = [
            (6, "1", "godot", [[frame("HashMap<int,int>::_lookup_pos", "core/templates/hash_map.h")]]),
            (2, "1", "godot", [[frame("fork_alloc_count_add", "core/os/fork_alloc_counters.cpp")]]),
            (2, "1", "godot", [[frame("Node::_propagate", "scene/main/node.cpp")]]),
            (5, "2", "ntdll", [["NtWaitForSingleObject"]]),
            (4, "2", "godot", [[frame("Memory::alloc_static", "core/os/memory.cpp")]]),
        ]
        shares = analyze.family_shares({"1": "main", "2": "worker"}, rows)
        self.assertEqual(shares["busy_samples"], 14)
        self.assertEqual(shares["busiest_thread"]["name"], "main")
        self.assertEqual(set(analyze.FAMILY_NAMES) | {"other"}, set(shares["families"]))
        self.assertAlmostEqual(sum(f["all_pct"] for f in shares["families"].values()), 100.0)
        self.assertAlmostEqual(sum(f["busiest_thread_pct"] for f in shares["families"].values()), 100.0)
        self.assertAlmostEqual(shares["families"]["hash tables"]["busiest_thread_pct"], 60.0)
        self.assertAlmostEqual(shares["families"]["allocation"]["all_pct"], 100.0 * 4 / 14)
        self.assertAlmostEqual(shares["families"]["allocation"]["share_pct"], 100.0 * 4 / 14)  # the larger


# Two samples of `perf script -F tid,comm,ip,sym,dso,srcline --inline --full-source-path` with a call graph
# (perf record -g): the leaf HashMap lookup inlined into Node::get_node, and a libc malloc.
PERF = """\
godot.linuxbsd. 4242
\t    55d0c0001234 HashMap<StringName, Node*>::_lookup_pos
  /home/u/godot/core/templates/hash_map.h:105 (inlined)
\t    55d0c0001234 Node::get_node+0x34 (/home/u/godot/bin/godot.linuxbsd.editor.x86_64.bench)
  /home/u/godot/scene/main/node.cpp:2001
\t    55d0c0005678 SceneTree::process+0x10 (/home/u/godot/bin/godot.linuxbsd.editor.x86_64.bench)
  /home/u/godot/scene/main/scene_tree.cpp:600

WorkerThread 3 4243
\t    7f0000001000 _int_malloc+0x1a (/usr/lib/libc.so.6)
  ??:0
\t    7f0000002000 malloc@@GLIBC_2.2.5+0x5 (/usr/lib/libc.so.6)

godot.linuxbsd. 4242
\t    55d0c0001234 HashMap<StringName, Node*>::_lookup_pos
  /home/u/godot/core/templates/hash_map.h:105 (inlined)
\t    55d0c0001234 Node::get_node+0x34 (/home/u/godot/bin/godot.linuxbsd.editor.x86_64.bench)
  /home/u/godot/scene/main/node.cpp:2001
\t    55d0c0005678 SceneTree::process+0x10 (/home/u/godot/bin/godot.linuxbsd.editor.x86_64.bench)
  /home/u/godot/scene/main/scene_tree.cpp:600
"""


class PerfToTsv(unittest.TestCase):
    def load(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "x.tsv")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("\n".join(perf_to_tsv.convert(text.splitlines())) + "\n")
            return analyze.load(path)

    def test_call_graph_samples(self):
        names, rows = self.load(PERF)
        self.assertEqual(names, {"4242": "godot.linuxbsd.", "4243": "WorkerThread 3"})
        self.assertEqual(len(rows), 2)  # the two identical stacks merged
        by_tid = {tid: (count, module, segments) for count, tid, module, segments in rows}
        count, module, segments = by_tid["4242"]
        self.assertEqual((count, module), (2, "godot.linuxbsd.editor.x86_64.bench"))
        self.assertEqual(len(segments), 2)  # leaf inline frame + its function, then the caller
        function, file = analyze.split_frame(segments[0][0])
        self.assertEqual(function, "HashMap<StringName, Node*>::_lookup_pos")
        self.assertEqual(analyze.relative(file), "core/templates/hash_map.h")
        self.assertEqual(analyze.split_frame(segments[0][1])[0], "Node::get_node")
        self.assertEqual(analyze.family(module, segments[0][0]), "hash tables")
        count, module, segments = by_tid["4243"]
        self.assertEqual((count, module, segments[0]), (1, "libc.so.6", ["_int_malloc"]))
        self.assertEqual(segments[1], ["malloc"])
        self.assertEqual(analyze.family(module, segments[0][0]), "allocation")

    def test_samples_without_call_graph(self):
        text = ("godot.linuxbsd. 4242 55d0c0001234 Node::get_node+0x34 (/home/u/godot/bin/godot.bench)\n"
                "  /home/u/godot/scene/main/node.cpp:2001\n")
        names, rows = self.load(text)
        self.assertEqual(rows[0][:3], (1, "4242", "godot.bench"))
        self.assertEqual(analyze.split_frame(rows[0][3][0][0]), ("Node::get_node", "/home/u/godot/scene/main/node.cpp"))


if __name__ == "__main__":
    unittest.main()
