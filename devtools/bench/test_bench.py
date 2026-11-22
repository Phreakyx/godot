"""Self-checks of the runner's statistics and the Linux adapter's parsers: python devtools/bench/test_bench.py"""

import ast
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402
import bench_linux  # noqa: E402  (imports on every OS; Linux calls sit inside functions)

FLOOR = 0.02


class SignTest(unittest.TestCase):
    def test_wins_needed_table(self):
        self.assertEqual([bench.wins_needed(n) for n in range(5, 14)], [5, 6, 7, 7, 8, 9, 9, 10, 10])

    def test_wins_needed_below_five_is_none(self):
        self.assertEqual([bench.wins_needed(n) for n in range(1, 5)], [None] * 4)

    def test_tie_is_a_loss_for_the_candidate(self):
        # Four clear wins and one tie of five: k=5 needs every round, so the tie decides.
        self.assertEqual(bench.verdict([-0.1, -0.1, -0.1, -0.1, 0.0], FLOOR, True), "REJECT")


class Verdict(unittest.TestCase):
    def test_target_accept(self):
        self.assertEqual(bench.verdict([-0.05] * 5, FLOOR, True), "ACCEPT")

    def test_target_median_exactly_at_floor_is_within_noise(self):
        self.assertEqual(bench.verdict([-FLOOR] * 5, FLOOR, True), "WITHIN-NOISE")

    def test_target_beyond_floor_with_too_few_wins_is_reject(self):
        self.assertEqual(bench.verdict([-0.05, -0.05, -0.05, 0.01, -0.05], FLOOR, True), "REJECT")

    def test_other_regression(self):
        self.assertEqual(bench.verdict([0.05] * 5, FLOOR, False), "REGRESSION")
        self.assertEqual(bench.verdict([0.05] * 5, FLOOR, True), "REGRESSION")  # a slower target too (review CR-02)

    def test_other_equal_memory_is_not_a_regression(self):
        self.assertEqual(bench.verdict([0.0] * 5, FLOOR, False), "WITHIN-NOISE")

    def test_other_slower_with_too_few_losses_is_within_noise(self):
        self.assertEqual(bench.verdict([0.05, 0.05, 0.05, -0.01, 0.05], FLOOR, False), "WITHIN-NOISE")

    def test_fewer_than_five_pairs_is_incomplete(self):
        self.assertEqual(bench.verdict([-0.05] * 4, FLOOR, True), "INCOMPLETE")

    def test_no_floor(self):
        self.assertEqual(bench.verdict([-0.05] * 5, None, True), "NO-FLOOR")

    def test_failed_rounds_are_dropped_from_the_pairs(self):
        control = {1: 10.0, 2: 10.0, 3: 10.0}
        candidate = {1: 9.0, 3: 11.0}  # round 2 failed on the candidate side
        self.assertEqual(bench.paired_diffs(control, candidate), [-0.1, 0.1])


class CountVerdict(unittest.TestCase):
    def test_any_increase_is_flagged(self):
        self.assertEqual(bench.count_verdict({"a": 100, "b": 50}, {"a": 90, "b": 51}, True), "ALLOC-INCREASE")

    def test_no_increase(self):
        self.assertEqual(bench.count_verdict({"a": 100}, {"a": 100}, True), "NO-INCREASE")

    def test_nondeterministic_aa_counts(self):
        self.assertEqual(bench.count_verdict({"a": 100}, {"a": 101}, False), "COUNT-NONDETERMINISTIC")


class FloorAndSkip(unittest.TestCase):
    def test_floor_is_the_largest_absolute_difference(self):
        self.assertEqual(bench.floor_from_aa([0.01, -0.03, 0.02]), 0.03)

    def test_max_gain(self):
        self.assertAlmostEqual(bench.max_gain(0.035, 2.0), 0.0175, places=12)

    def test_skip_only_strictly_below_the_floor(self):
        self.assertEqual(bench.skip_decision(0.0099, 0.01), "skip")
        self.assertEqual(bench.skip_decision(0.01, 0.01), "build")
        self.assertEqual(bench.skip_decision(0.02, 0.01), "build")


class FloorsAsData(unittest.TestCase):
    META = {"compiler_version": "14.51.36231", "bios": "1.K0", "microcode": "0x12b", "os_build": "26100.9711"}

    def setUp(self):
        self.key = bench.floor_key("scene", "churn:10000", "frame_ms_p50", "editor", "msvc")
        self.floors = {self.key: dict(self.META, floor=0.03, n=5)}

    def test_floor_found(self):
        self.assertEqual(bench.lookup_floor(self.floors, self.key, self.META, 5), 0.03)

    def test_no_entry_is_no_floor(self):
        self.assertEqual(bench.lookup_floor({}, self.key, self.META, 5), "NO-FLOOR")

    def test_changed_machine_or_n_is_re_measure(self):
        for field, value in (("compiler_version", "14.52.1"), ("bios", "1.L0"), ("microcode", "0x12c"),
                             ("os_build", "26200.1")):
            with self.subTest(field=field):
                meta = dict(self.META, **{field: value})
                self.assertEqual(bench.lookup_floor(self.floors, self.key, meta, 5), "RE-MEASURE-AA")
        self.assertEqual(bench.lookup_floor(self.floors, self.key, self.META, 11), "RE-MEASURE-AA")

    def test_monthly_os_update_keeps_the_floor(self):
        # D-04: a major OS update invalidates the floor; the update build revision (UBR) alone does not.
        meta = dict(self.META, os_build="26100.9999")
        self.assertEqual(bench.lookup_floor(self.floors, self.key, meta, 5), 0.03)

    def test_key_separates_build_type_and_compiler(self):
        keys = {self.key, bench.floor_key("scene", "churn:10000", "frame_ms_p50", "template_release", "msvc"),
                bench.floor_key("scene", "churn:10000", "frame_ms_p50", "editor", "gcc")}
        self.assertEqual(len(keys), 3)


class BudgetPlan(unittest.TestCase):
    def test_cheap_processes_get_eleven_rounds(self):
        self.assertEqual(bench.plan_rounds(2.9), 11)
        self.assertEqual(bench.plan_rounds(3.0), 5)

    def test_budget_minutes(self):
        # Two workloads (2 s x 11 rounds, 6 s x 5 rounds), three binaries, two build types.
        self.assertAlmostEqual(bench.budget_minutes([(2.0, 11), (6.0, 5)], 3, 2), (22 + 30) * 6 / 60)

    def test_suite_names_known_workloads(self):
        self.assertEqual({bench.parse_workload(spec)[0] for spec in bench.SUITE}, set(bench.WORKLOADS))


def _result(line, returncode=0):
    return {"output": line + "\n", "timed_out": False, "returncode": returncode, "peak_ws_bytes": 1 << 20,
            "wall_s": 1.5}


def _counts(line, names):
    fields, reason = bench.process_fields("scene:churn:10", "scene", _result(line), {})["scene:churn:10"]
    return bench.count_values(fields, names)


class Counts(unittest.TestCase):
    def test_counts_are_not_time_or_memory_metrics(self):
        for name, w in bench.WORKLOADS.items():
            self.assertFalse(set(w["counts"]) & set(w["metrics"]), name)
        self.assertEqual(bench.WORKLOADS["scene"]["counts"], ("allocs", "reallocs"))

    def test_counts_are_read_from_the_bench_line_zero_included(self):
        counts = _counts("BENCH: frame_ms_p50=1.0 allocs=1200 reallocs=0", ("allocs", "reallocs"))
        self.assertEqual(counts, {"allocs": 1200, "reallocs": 0})

    def test_missing_field_is_not_available_not_zero(self):
        counts = _counts("BENCH: frame_ms_p50=1.0", ("allocs", "reallocs"))
        self.assertEqual(counts, {"allocs": None, "reallocs": None})

    def test_failed_sample_has_no_counts(self):
        counts = _counts("BENCH_FAIL: boom", ("allocs",))
        self.assertEqual(counts, {"allocs": None})

    def test_aa_identical(self):
        self.assertEqual(bench.aa_counts({1: 50, 2: 50}, {1: 50, 2: 50}), "identical")

    def test_aa_any_difference_is_nondeterministic_with_the_spread(self):
        self.assertEqual(bench.aa_counts({1: 50, 2: 52}, {1: 49, 2: 50}), "COUNT-NONDETERMINISTIC (49..52)")

    def test_aa_binary_without_counts_is_not_available(self):
        self.assertEqual(bench.aa_counts({1: 50}, {}), "n/a (aa)")

    def test_compare_uses_the_count_rule(self):
        self.assertEqual(bench.compare_counts({1: 50, 2: 50}, {1: 51, 2: 51}), "ALLOC-INCREASE")
        self.assertEqual(bench.compare_counts({1: 50, 2: 50}, {1: 50, 2: 50}), "NO-INCREASE")
        self.assertEqual(bench.compare_counts({1: 50, 2: 50}, {1: 40, 2: 40}), "NO-INCREASE")

    def test_compare_nondeterministic_or_missing(self):
        self.assertEqual(bench.compare_counts({1: 50, 2: 51}, {1: 50, 2: 50}), "COUNT-NONDETERMINISTIC")
        self.assertEqual(bench.compare_counts({1: 50}, {}), "n/a")


class Clocks(unittest.TestCase):
    def test_rounds_more_than_five_percent_off_the_median_are_flagged(self):
        flags = bench.clock_flags({0: 5000.0, 1: 5100.0, 2: 4700.0, 3: 5000.0, 4: 5300.0})
        self.assertEqual(flags, {0: 0, 1: 0, 2: 1, 3: 0, 4: 1})

    def test_unavailable_clock_stays_unavailable(self):
        self.assertEqual(bench.clock_flags({1: "unavailable", 2: 5000.0}), {1: "unavailable", 2: 0})


class SeriesMetadata(unittest.TestCase):
    def test_series_in_the_same_second_get_their_own_directory(self):
        with tempfile.TemporaryDirectory() as base:
            names = [bench.series_dir(Path(base), "20261120-120000-aa").name for _ in range(3)]
        self.assertEqual(names, ["20261120-120000-aa", "20261120-120000-aa-2", "20261120-120000-aa-3"])

    def test_unreadable_probe_is_unavailable(self):
        self.assertEqual(bench.probe(lambda: 1 / 0), "unavailable")
        self.assertEqual(bench.probe(lambda: ""), "unavailable")
        self.assertEqual(bench.probe(lambda: 0), 0)  # Game Mode off is a value, not a missing one


GD = "gdbench"


class MultiCase(unittest.TestCase):
    LINES = ("BENCH: workload=gdbench case=a.benchmark_x time_ms=2.0\n"
             "BENCH: workload=gdbench case=b.benchmark_x time_ms=3.0")

    def test_same_method_in_two_files_is_two_cases(self):
        fields = bench.process_fields(GD, GD, _result(self.LINES), {})
        self.assertEqual(sorted(fields), [GD, "gdbench/a.benchmark_x", "gdbench/b.benchmark_x"])
        self.assertEqual(fields["gdbench/b.benchmark_x"][0]["time_ms"], "3.0")
        self.assertEqual(bench.item_def(GD, GD, [], "gdbench/a.benchmark_x")["case"], "a.benchmark_x")

    def test_expected_case_missing_is_failed_not_dropped(self):
        expected = {"gdbench/a.benchmark_x", "gdbench/c.benchmark_y"}
        fields = bench.process_fields(GD, GD, _result(self.LINES), {}, expected)
        self.assertIsNone(fields["gdbench/c.benchmark_y"][0])
        self.assertIn("missing", fields["gdbench/c.benchmark_y"][1])

    def test_bench_fail_fails_every_expected_case(self):
        result = _result("BENCH_FAIL: empty.gd has no benchmark_ methods", returncode=1)
        fields = bench.process_fields(GD, GD, result, {}, {"gdbench/a.benchmark_x"})
        self.assertEqual({reason for _, reason in fields.values()}, {"BENCH_FAIL: empty.gd has no benchmark_ methods"})
        self.assertEqual(bench.metric_values(*fields["gdbench/a.benchmark_x"], ("time_ms",)),
                         {"time_ms": (None, "BENCH_FAIL: empty.gd has no benchmark_ methods")})

    def test_order_is_workload_then_case(self):
        items = {}
        for spec, label in (("scene:instance:10", "scene:instance:10"), (GD, "gdbench/b.x"), (GD, "gdbench/a.x"),
                            (GD, GD), ("scene:churn:10", "scene:churn:10")):
            name, params = bench.parse_workload(spec)
            items[label] = (spec, name, params, bench.item_def(spec, name, params, label))
        self.assertEqual([label for label, _ in bench.sorted_items(items)],
                         [GD, "gdbench/a.x", "gdbench/b.x", "scene:churn:10", "scene:instance:10"])

    def test_editor_startup_is_editor_only(self):
        self.assertTrue(bench.runs_on("editor_startup", "editor"))
        self.assertFalse(bench.runs_on("editor_startup", "template_release"))
        self.assertTrue(bench.runs_on("gdbench", "template_release"))
        self.assertEqual(bench.only_on("editor_startup"), "n/a (editor only)")


class GdbenchCopy(unittest.TestCase):
    def test_only_extends_and_iterations_change_line_endings_kept(self):
        text = "extends Benchmark\r\n\r\nconst ITERATIONS = 1_000_000\r\nfunc benchmark_a() -> void:\r\n\tpass\r\n"
        copy, changed = bench.gdbench_copy(text, 1000)
        self.assertEqual(changed, 2)
        self.assertEqual(copy, text.replace("extends Benchmark", "extends RefCounted").replace("1_000_000", "1000"))
        self.assertEqual(bench.gdbench_copy(text, None)[1], 1)

    def test_no_benchmark_base_is_refused(self):
        with self.assertRaises(RuntimeError):
            bench.gdbench_copy("extends Node\n", None)


def micro_row(case, operations="100", checksum="7", live_bytes="64", allocs="2"):
    return {"case": case, "ns_per_op": "1.5", "operations": operations, "live_bytes": live_bytes, "allocs": allocs,
            "checksum": checksum}


class Micro(unittest.TestCase):
    def test_a_checksum_change_between_samples_of_one_side_is_non_repeatable(self):
        refs = {"control": [micro_row("hashmap_i64_hit_16")]}
        with self.assertRaisesRegex(RuntimeError, "Non-repeatable.*checksum"):
            bench.micro_check(refs, "control", [micro_row("hashmap_i64_hit_16", checksum="8")], "control",
                              bench.MICRO_PARITY)

    def test_an_operations_change_between_samples_of_one_side_is_non_repeatable(self):
        refs = {"control": [micro_row("list_i64_erase_4096")]}
        with self.assertRaisesRegex(RuntimeError, "Non-repeatable.*operations"):
            bench.micro_check(refs, "control", [micro_row("list_i64_erase_4096", operations="99")], "control",
                              bench.MICRO_PARITY)

    def test_identical_samples_pass_and_the_first_becomes_the_reference(self):
        refs = {}
        bench.micro_check(refs, "control", [micro_row("rbset_str_miss_0")], "control", bench.MICRO_PARITY)
        bench.micro_check(refs, "control", [micro_row("rbset_str_miss_0")], "control", bench.MICRO_PARITY)
        self.assertEqual(list(refs), ["control"])

    def test_compare_sides_may_differ_in_memory_but_not_in_work(self):
        refs = {"control": [micro_row("hashset_i64_insert_256")]}
        cross = ("operations", "checksum")
        bench.micro_check(refs, "candidate", [micro_row("hashset_i64_insert_256", live_bytes="32")], "control", cross)
        with self.assertRaisesRegex(RuntimeError, "different workloads"):
            bench.micro_check(refs, "previous", [micro_row("hashset_i64_insert_256", checksum="1")], "control", cross)

    def test_hash_byte_checksums_are_not_compared_across_sides(self):
        refs = {"control": [micro_row("hash_bytes_8")]}
        bench.micro_check(refs, "candidate", [micro_row("hash_bytes_8", checksum="9")], "control", ("checksum",))

    def test_case_names_parse_into_four_parts(self):
        self.assertEqual(bench.parse_case("hashmap_ptr_churn_1048576"), ("hashmap", "ptr", "churn", 1048576))
        self.assertEqual(bench.parse_case("ahashmap_i64_miss_0"), ("ahashmap", "i64", "miss", 0))
        self.assertEqual(bench.parse_case("ctl_unordered_dense_i64_hit_32768"),
                         ("ctl_unordered_dense", "i64", "hit", 32768))
        self.assertEqual(bench.parse_case("vector_str_copy_32768"), ("vector", "str", "copy", 32768))

    def test_non_matrix_cases_do_not_parse(self):
        for name in ("string_copy_detach_16", "hash_bytes_8", "hashmap_int_hit_16", "hashmap_i64_sort_16"):
            self.assertIsNone(bench.parse_case(name), name)

    def test_ratio_to_the_fastest_matching_control(self):
        medians = {"hashmap_i64_hit_256": 10.0, "ctl_std_map_i64_hit_256": 2.0, "ctl_boost_flat_i64_hit_256": 4.0,
                   "ctl_std_umap_i64_hit_256": 5.0, "ctl_boost_flat_i64_hit_4096": 1.0, "rbmap_i64_hit_256": 3.0,
                   "rbmap_ptr_hit_256": 3.0, "list_i64_push_256": 2.0}
        self.assertEqual(bench.fastest_controls(medians), {
            "hashmap_i64_hit_256": (2.5, "ctl_boost_flat_i64_hit_256"),  # std::map is ordered: not a hash control
            "rbmap_i64_hit_256": (1.5, "ctl_std_map_i64_hit_256"),
        })

    def test_layout_sentinel_counts_control_rows_beyond_their_floor(self):
        summary = [
            {"workload": "micro/ctl_std_map_i64_hit_256", "n": 11, "median_diff": 0.05, "floor": 0.02},
            {"workload": "micro/ctl_std_map_i64_hit_4096", "n": 11, "median_diff": -0.01, "floor": 0.02},
            {"workload": "micro/ctl_std_map_i64_miss_4096", "n": 11, "median_diff": 0.5, "floor": ""},
            {"workload": "micro/hashmap_i64_hit_256", "n": 11, "median_diff": 0.5, "floor": 0.02},
        ]
        self.assertEqual(bench.layout_sentinel(summary), (1, 2))

    def test_harness_csv_parses_and_a_failed_process_raises(self):
        output = "case," + ",".join(bench.MICRO_FIELDS) + "\nhashmap_i64_hit_16,2.5,1600,640,3,12\n"
        result = {"output": output, "returncode": 0, "timed_out": False}
        self.assertEqual(bench.micro_rows(result)[0]["allocs"], "3")
        with self.assertRaisesRegex(RuntimeError, "BENCH_FAIL"):
            bench.micro_rows({**result, "output": output + "BENCH_FAIL: correctness check\n", "returncode": 2})


class MicroEngine(unittest.TestCase):
    LINE = "BENCH: workload=micro_engine case=dict_sname_hit_16 ns_per_op=19.5 operations=131072 allocs=0 checksum=99"

    def test_cases_are_items_with_ns_per_op_and_allocs(self):
        result = {"output": self.LINE + "\n", "returncode": 0, "timed_out": False, "peak_ws_bytes": 1 << 20,
                  "wall_s": 1}
        fields = bench.process_fields("micro_engine", "micro_engine", result, {})
        self.assertEqual(fields["micro_engine/dict_sname_hit_16"][0]["ns_per_op"], "19.5")
        d = bench.item_def("micro_engine", "micro_engine", [], "micro_engine/dict_sname_hit_16")
        self.assertEqual((d["metrics"], d["counts"]), (("ns_per_op",), ("allocs",)))

    def test_parity_stops_a_series_whose_work_changed(self):
        refs = {}
        fields = {"operations": "131072", "checksum": "99", "ns_per_op": "19.5"}
        bench.engine_parity(refs, "micro_engine/x", fields, bench.MICRO_ENGINE_PARITY)
        bench.engine_parity(refs, "micro_engine/x", {**fields, "ns_per_op": "20.1"}, bench.MICRO_ENGINE_PARITY)
        with self.assertRaisesRegex(RuntimeError, "different work"):
            bench.engine_parity(refs, "micro_engine/x", {**fields, "checksum": "98"}, bench.MICRO_ENGINE_PARITY)

    def test_runs_the_skipped_cases_of_the_tests_build(self):
        self.assertIn("--no-skip", bench.WORKLOADS["micro_engine"]["command"]("x.exe", [], {}))
        self.assertTrue(bench.is_tests("micro_engine"))
        self.assertFalse(bench.is_tests("scene"))

    @unittest.skipUnless(bench.osb is not None and bench.os.name == "nt", "Windows adapter")
    def test_tests_build_arguments(self):
        self.assertIn("-Tests", bench.osb.build_args("editor", "aa", None, None, True))
        args = bench.osb.scons_args("editor", "aa", None, True)
        self.assertIn("extra_suffix=bench.aa.tests", args)
        self.assertIn("tests=yes", args)
        self.assertNotIn("tests=yes", bench.osb.scons_args("editor", "aa"))


@unittest.skipUnless(bench.osb is not None and bench.os.name == "nt", "Windows adapter")
class TypeperfParsing(unittest.TestCase):
    def test_only_sample_values_are_kept(self):
        text = (
            '\n"(PDH-CSV 4.0)","\\\\HOST\\Processor Information(0,2)\\% Processor Performance",'
            '"\\\\HOST\\Processor Information(_Total)\\Processor Frequency"\n'
            '"11/20/2026 23:24:25.393"," ","3200.000000"\n'
            '"11/20/2026 23:24:26.399","150.0","3200.000000"\n'
            "Exiting, please wait...\nThe command completed successfully.\n"
        )
        self.assertEqual(bench.osb.typeperf_values(text, 2), [[150.0, 3200.0]])


HERE = Path(__file__).resolve().parent
P_SIBLINGS = {cpu: f"{cpu & ~1}-{cpu | 1}" for cpu in range(16)}  # 14900K: P-cores 0-15 as HT pairs


class LinuxParsers(unittest.TestCase):
    def test_cpu_lists(self):
        self.assertEqual(bench_linux.parse_cpu_list("0-15"), list(range(16)))
        self.assertEqual(bench_linux.parse_cpu_list("0-3,8,10-11\n"), [0, 1, 2, 3, 8, 10, 11])
        self.assertEqual(bench_linux.cpu_list_text([2, 3, 4, 8, 10, 11]), "2-4,8,10-11")

    def test_bench_cpus_drop_the_core_of_the_lowest_cpu(self):
        self.assertEqual(bench_linux.bench_cpus("0-15", P_SIBLINGS), list(range(2, 16)))
        siblings = {cpu: f"{cpu},{cpu + 8}" if cpu < 8 else f"{cpu - 8},{cpu}" for cpu in range(16)}  # other numbering
        self.assertEqual(bench_linux.bench_cpus("0-15", siblings), [1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15])
        self.assertEqual(bench_linux.cpus_mask(bench_linux.bench_cpus("0-15", P_SIBLINGS)), 0xFFFC)
        with self.assertRaises(RuntimeError):
            bench_linux.bench_cpus("0-1", P_SIBLINGS)

    def test_ignorepkg(self):
        conf = "[options]\n#IgnorePkg   =\nIgnorePkg = gcc gcc-libs clang llvm llvm-libs lld\n"
        self.assertTrue(bench_linux.ignorepkg_held(conf))
        self.assertEqual(bench_linux.ignorepkg_missing("[options]\nIgnorePkg = gcc gcc-libs clang llvm\n"),
                         ["llvm-libs", "lld"])
        self.assertEqual(bench_linux.ignorepkg_missing("#IgnorePkg = gcc gcc-libs clang llvm llvm-libs lld\n"),
                         list(bench_linux.HELD_PACKAGES))
        self.assertTrue(bench_linux.ignorepkg_held("IgnorePkg = gcc*\nIgnorePkg = clang llvm* lld\n"))

    def test_cpu_load_of_the_listed_cpus(self):
        before = "cpu  0 0 0 0 0 0 0 0 0 0\ncpu2 100 0 100 800 0 0 0 0 0 0\ncpu3 0 0 0 1000 0 0 0 0 0 0\n"
        after = "cpu  0 0 0 0 0 0 0 0 0 0\ncpu2 150 0 150 900 0 0 0 0 0 0\ncpu3 0 0 0 1200 0 0 0 0 0 0\nintr 5\n"
        self.assertAlmostEqual(bench_linux.cpu_load(before, after, [2]), 0.5)
        self.assertAlmostEqual(bench_linux.cpu_load(before, after, [2, 3]), 100 / 400)

    def test_os_release_and_cpuinfo(self):
        self.assertEqual(bench_linux.os_release('NAME="CachyOS Linux"\nPRETTY_NAME="CachyOS"\nID=cachyos\n'),
                         "CachyOS")
        cpuinfo = "processor\t: 0\nmodel name\t: Intel(R) Core(TM) i9-14900K\nmicrocode\t: 0x12b\n"
        self.assertEqual(bench_linux.cpuinfo_field(cpuinfo, "model name"), "Intel(R) Core(TM) i9-14900K")
        self.assertEqual(bench_linux.cpuinfo_field(cpuinfo, "microcode"), "0x12b")

    def test_idle_compilers(self):
        procs = [("bash", ""), ("cc1plus", ""), ("ld.lld", ""), ("python3", "python3 /usr/bin/scons -j32"),
                 ("python3", "python3 devtools/bench/bench.py aa"), ("clang-21", ""), ("godot", "")]
        self.assertEqual(bench_linux.busy_compilers(procs), ["cc1plus", "clang-21", "ld.lld", "python3"])

    def test_child_peak_must_exceed_the_runners(self):
        self.assertEqual(bench_linux.child_peak_bytes(90_000, 40_000), 90_000 * 1024)
        with self.assertRaises(RuntimeError):  # 02-12: every workload below the runner's 603 MiB read exactly that
            bench_linux.child_peak_bytes(617_868, 617_868)

    def test_binary_names_put_llvm_before_the_suffix(self):
        self.assertEqual(bench_linux.linux_binary("editor", "bench.aa", clang=True).as_posix(),
                         "bin/godot.linuxbsd.editor.x86_64.llvm.bench.aa")
        self.assertEqual(bench_linux.linux_binary("template_release", "bench.tests", clang=False).as_posix(),
                         "bin/godot.linuxbsd.template_release.x86_64.bench.tests")

    def test_scons_args_per_compiler(self):
        old = bench_linux.COMPILER
        try:
            bench_linux.COMPILER = "gcc"
            gcc = bench_linux.scons_args("editor", "aa", "FORK_BACKEND_X=1", True)
            bench_linux.COMPILER = "clang"
            clang = bench_linux.scons_args("editor", "aa", "FORK_BACKEND_X=1", True)
        finally:
            bench_linux.COMPILER = old
        self.assertEqual([a for a in clang if a not in gcc], ["use_llvm=yes", "linker=lld"])
        for arg in ("platform=linuxbsd", "production=yes", "tests=yes", "object_prefix=t_", "debug_symbols=yes",
                    "disable_path_overrides=no", "extra_suffix=bench.aa.tests",
                    "cppdefines=FORK_ALLOC_COUNTERS=1 FORK_BACKEND_X=1"):
            self.assertIn(arg, gcc)


GOOD_STATE = {"governor": "performance", "epp": "performance", "no_turbo": "0", "sched_ext": "disabled",
              "sched_ext_ops": "unavailable", "compiler_hold": "held", "perf_event_paranoid": "2"}


class LinuxChecks(unittest.TestCase):
    def test_good_state_passes(self):
        self.assertEqual(bench_linux.state_problems(GOOD_STATE, [2, 3], perf=True), [])

    def test_each_wrong_setting_refuses_with_its_fix(self):
        cases = {
            "governor": ("powersave", "sudo cpupower -c 2-3 frequency-set -g performance"),
            "epp": ("balance_performance",
                    "echo performance | sudo tee /sys/devices/system/cpu/cpu{2,3}/cpufreq/energy_performance_preference"),
            "no_turbo": ("1", "echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo"),
            "sched_ext": ("enabled", "sudo systemctl stop scx_loader.service"),
            "compiler_hold": ("lld", "IgnorePkg = gcc gcc-libs clang llvm llvm-libs lld"),
            "perf_event_paranoid": ("4", "sudo sysctl kernel.perf_event_paranoid=2"),
        }
        for key, (value, fix) in cases.items():
            with self.subTest(key=key):
                problems = bench_linux.state_problems(dict(GOOD_STATE, **{key: value}), [2, 3], perf=True)
                self.assertEqual(len(problems), 1)
                self.assertIn(fix, problems[0])

    def test_paranoid_matters_only_for_counters_and_profiles(self):
        self.assertEqual(bench_linux.state_problems(dict(GOOD_STATE, perf_event_paranoid="4"), [2], perf=False), [])

    def test_missing_sysfs_files_are_recorded_not_refused(self):
        state = dict.fromkeys(GOOD_STATE, "unavailable")
        self.assertEqual(bench_linux.state_problems(state, [2], perf=True), [])

    def test_no_sudo_run_and_no_system_file_written(self):
        """T-02-26: fixes exist only as printed strings; nothing writes /sys, /proc/sys or /etc."""
        tree = ast.parse((HERE / "bench_linux.py").read_text(encoding="utf-8"))
        names = {t.id: n.value.value for n in tree.body if isinstance(n, ast.Assign) and isinstance(n.value, ast.Constant)
                 and isinstance(n.value.value, str) for t in n.targets if isinstance(t, ast.Name)}

        def strings(node):
            for n in ast.walk(node):
                if isinstance(n, ast.Constant) and isinstance(n.value, str):
                    yield n.value
                elif isinstance(n, ast.Name) and n.id in names:
                    yield names[n.id]

        system = ("/sys", "/proc/sys", "/etc")
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, "id", "")
            if func in ("run", "Popen", "call", "check_call", "check_output", "system", "execv", "execvp"):
                self.assertFalse(any("sudo" in s for a in node.args for s in strings(a)), ast.unparse(node))
            if func == "open" and node.args:
                mode = node.args[1] if len(node.args) > 1 else next((k.value for k in node.keywords if k.arg == "mode"),
                                                                    None)
                mode = mode.value if isinstance(mode, ast.Constant) else "r"
                if set(mode) & set("wax+"):
                    self.assertFalse(any(s.startswith(system) for s in strings(node.args[0])), ast.unparse(node))
            if func in ("write_text", "write_bytes"):
                self.assertFalse(any(s.startswith(system) for s in strings(node.func)), ast.unparse(node))


class LinuxPerf(unittest.TestCase):
    STAT = ("# started on Sat Nov 21 10:00:00 2026\n\n"
            "12345678901,,cpu_core/instructions/u,2003456789,100.00,2.70,insn per cycle\n"
            "4567890123,,cpu_core/cycles/u,2003456789,100.00,,\n"
            "<not counted>,,cpu_core/cache-misses/u,0,0.00,,\n"
            "<not supported>,,cpu_core/cache-references/u,0,0.00,,\n")

    def test_parse_perf_stat_missing_is_none_not_zero(self):
        self.assertEqual(bench_linux.parse_perf_stat(self.STAT), {
            "instructions": 12345678901, "cycles": 4567890123, "cache_misses": None, "cache_references": None})
        counted = self.STAT.replace("<not counted>", "98765").replace("<not supported>", "1234567")
        self.assertEqual(bench_linux.parse_perf_stat(counted)["cache_misses"], 98765)
        self.assertEqual(bench_linux.parse_perf_stat(counted)["cache_references"], 1234567)

    def test_counter_command(self):
        self.assertEqual(bench_linux.counter_command("c.txt", ["bin/godot", "--headless"]), [
            "perf", "stat", "-x,", "-o", "c.txt", "-e",
            "cpu_core/instructions/u,cpu_core/cycles/u,cpu_core/cache-misses/u,cpu_core/cache-references/u",
            "--", "bin/godot", "--headless"])

    def test_micro_command_differs_only_in_the_compiler(self):
        args = (Path("out/bench"), [Path("a.cpp")], [Path(".")], ["BENCH_CONTROLS"])
        gcc, clang = (bench_linux.micro_command(c, *args) for c in ("gcc", "clang"))
        self.assertEqual((gcc[0], clang[0]), ("g++", "clang++"))
        self.assertEqual(gcc[1:], clang[1:])
        for flag in ("-O2", "-std=gnu++20", "-fno-rtti", "-DDEBUG_ENABLED", "-DTOOLS_ENABLED", "-DLINUXBSD_ENABLED",
                     "-DUNIX_ENABLED", "-DFORK_ALLOC_COUNTERS=1", "-DBENCH_CONTROLS"):
            self.assertIn(flag, gcc)

    def test_micro_wrapper_compiles_as_c(self):
        # g++ and clang++ treat .c as C++: the mimalloc wrapper gets a C compile with the side's defines.
        args = (Path("out/fork_mimalloc.o"), Path("core/os/fork_mimalloc.c"), [Path(".")], ["FORK_BACKEND_ALLOCATOR=1"])
        for compiler, cc in (("gcc", "gcc"), ("clang", "clang")):
            command = bench_linux.micro_c_command(compiler, *args)
            self.assertEqual(command[0], cc)
            for flag in ("-std=gnu17", "-DFORK_ALLOC_COUNTERS=1", "-DFORK_BACKEND_ALLOCATOR=1", "-c"):
                self.assertIn(flag, command)
            self.assertNotIn("-std=gnu++20", command)
            self.assertIn(str(Path("core/os/fork_mimalloc.c")), command)
        self.assertIn("core/os/fork_mimalloc.c", bench.MICRO_TREE_INPUTS)

    def test_micro_target_names_one_case(self):
        # Review CR-01: micro items share the spec "micro"; --target micro/<case> must reach the accept rule.
        meta = {"msvc_toolset": "v", "compiler_version": "v", "bios": "b", "microcode": "m", "os_build": "1.1"}
        d = {"case": "x", "metrics": ("ns_per_op",), "counts": (), "info": (), "time": "ns_per_op"}
        items = [("micro/x", ("micro", "micro", [], d)), ("micro/y", ("micro", "micro", [], {**d, "case": "y"}))]
        with tempfile.TemporaryDirectory() as tmp:
            floors = Path(tmp) / "floors.json"
            floors.write_text(bench.json.dumps({bench.floor_key("micro", c, "ns_per_op", bench.MICRO_BUILD, bench.COMPILER): {
                "floor": 0.01, "n": 5, "compiler_version": "v", "bios": "b", "microcode": "m", "os_build": "1.1"}
                for c in ("x", "y")}))
            args = type("Args", (), {"floors": str(floors), "target": "micro/x"})()
            values = {(item, "ns_per_op", bench.MICRO_BUILD, label): {r: 10.0 * (0.5 if label == "candidate" else 1.0)
                                                                     for r in range(1, 6)}
                      for item, _ in items for label in ("previous", "candidate", "control")}
            rows = bench.summarize_compare(args, items, [bench.MICRO_BUILD], values, {"micro": 5}, meta)
        self.assertEqual([row["verdict"] for row in rows], ["ACCEPT", "WITHIN-NOISE"])

    def test_negative_counts_are_values(self):
        self.assertEqual(bench.count_values({"allocs": "-3", "reallocs": "x"}, ("allocs", "reallocs")),
                         {"allocs": -3, "reallocs": None})  # review WR-02

    def test_counter_columns_never_change_a_verdict(self):
        item, metric, build = "scene:churn:10", "frame_ms_p50", "editor"
        name, params = bench.parse_workload(item)
        meta = {"msvc_toolset": "v", "compiler_version": "v", "bios": "b", "microcode": "m", "os_build": "1.1"}
        with tempfile.TemporaryDirectory() as tmp:
            floors = Path(tmp) / "floors.json"
            floors.write_text(bench.json.dumps({bench.floor_key(name, "churn:10", metric, build, bench.COMPILER): {
                "floor": 0.01, "n": 5, "compiler_version": "v", "bios": "b", "microcode": "m", "os_build": "1.1"}}))
            args = type("Args", (), {"floors": str(floors), "target": item})()
            items = [(item, (item, name, params, bench.item_def(item, name, params, item)))]
            values = {(item, m, build, label): {r: base * (0.9 if label == "candidate" else 1.0) for r in range(1, 6)}
                      for m, base in (("frame_ms_p50", 10.0), ("peak_ws_mb", 100.0))
                      for label in ("previous", "candidate", "control")}
            plain = bench.summarize_compare(args, items, [build], values, {item: 5}, meta)
            counted = [dict(row) for row in plain]
            found = {(item, build, label): dict(zip(bench.COUNTER_COLUMNS, (100, 50, None, 10)))
                     for label in ("previous", "candidate", "control")}
            bench.add_counter_columns(counted, found, ("previous", "candidate", "control"))
        self.assertEqual([row["verdict"] for row in plain], ["ACCEPT", "NO-FLOOR"])
        for before, after in zip(plain, counted):
            self.assertEqual({k: after[k] for k in before}, before)
            self.assertEqual(after["cache_misses"], "previous=n/a candidate=n/a control=n/a")
            self.assertEqual(after["instructions"], "previous=100 candidate=100 control=100")


class LinuxAdapterShape(unittest.TestCase):
    # Windows-only calls in bench.py, each inside an `os.name == "nt"` branch (no Linux equivalent).
    WINDOWS_ONLY = {"check_power_plan", "game_mode", "msvc_toolset", "vs_version"}

    def test_linux_has_every_function_bench_py_calls(self):
        called = set(re.findall(r"\bosb\.(\w+)", (HERE / "bench.py").read_text(encoding="utf-8")))
        windows = {n.name for n in ast.parse((HERE / "bench_windows.py").read_text(encoding="utf-8")).body
                   if isinstance(n, (ast.FunctionDef, ast.ClassDef))}
        windows |= set(re.findall(r"^([A-Z_]+) =", (HERE / "bench_windows.py").read_text(encoding="utf-8"), re.M))
        self.assertLessEqual(self.WINDOWS_ONLY, windows)
        self.assertEqual(sorted(n for n in called & windows - self.WINDOWS_ONLY if not hasattr(bench_linux, n)), [])

    def test_kernel_release_floors(self):
        self.assertEqual(bench._major_os("6.17.8-2-cachyos"), "6.17")
        self.assertEqual(bench._major_os("26100.9711"), "26100")
        key = bench.floor_key("scene", "churn:10000", "frame_ms_p50", "editor", "gcc")
        meta = {"compiler_version": "g++ (GCC) 15.2.1", "bios": "1.K0", "microcode": "0x12b",
                "os_build": "6.17.8-2-cachyos"}
        floors = {key: dict(meta, floor=0.03, n=5)}
        self.assertEqual(bench.lookup_floor(floors, key, dict(meta, os_build="6.17.9-1-cachyos"), 5), 0.03)
        self.assertEqual(bench.lookup_floor(floors, key, dict(meta, os_build="6.18.1-1-cachyos"), 5), "RE-MEASURE-AA")


class SeriesVerdict(unittest.TestCase):
    """bench.py verdict: the mechanical reading of a finished series (summary.csv and counts.csv)."""

    HEADER = "workload,metric,build,rule,verdict\n"

    def series(self, summary, counts=None):
        tmp = Path(tempfile.mkdtemp())
        (tmp / "summary.csv").write_text(self.HEADER + "".join(f"{r}\n" for r in summary), encoding="utf-8")
        if counts is not None:
            (tmp / "counts.csv").write_text("workload,metric,build,verdict\n" + "".join(f"{r}\n" for r in counts),
                                            encoding="utf-8")
        return tmp

    TARGET = "script:script,frame_ms_p50,template_release,accept,ACCEPT"

    def test_accept_and_no_regression_passes(self):
        tmp = self.series([self.TARGET, "scene:instance:10000,peak_ws_mb,editor,regression,WITHIN-NOISE"])
        code, lines = bench.series_verdict(tmp, "script:script@template_release")
        self.assertEqual(code, 0)
        self.assertTrue(lines[0].startswith("PASS "))

    def test_a_regression_anywhere_fails_naming_it(self):
        tmp = self.series([self.TARGET, "scene:instance:10000,peak_ws_mb,editor,regression,REGRESSION"])
        code, lines = bench.series_verdict(tmp, "script:script@template_release")
        self.assertEqual(code, 1)
        self.assertTrue(lines[0].startswith("FAIL ") and "scene:instance:10000 peak_ws_mb editor" in lines[0])

    def test_target_not_accepted_fails_naming_it(self):
        for token in ("WITHIN-NOISE", "REJECT", "NO-FLOOR", "INCOMPLETE"):
            tmp = self.series([f"script:script,frame_ms_p50,template_release,accept,{token}"])
            code, lines = bench.series_verdict(tmp, "script:script@template_release")
            self.assertEqual(code, 1, token)
            self.assertIn("script:script frame_ms_p50 template_release", lines[0])
            self.assertIn(token, lines[0])

    def test_micro_series_follows_the_same_rule(self):
        tmp = self.series(["micro/hashmap_i64_insert_4096,ns_per_op,standalone,accept,ACCEPT",
                           "micro/hashmap_i64_hit_4096,ns_per_op,standalone,regression,WITHIN-NOISE"],
                          ["micro/hashmap_tiny_ptr_build_2,live_bytes,standalone,INCREASE"])
        code, lines = bench.series_verdict(tmp, "micro/hashmap_i64_insert_4096@standalone")
        self.assertEqual(code, 0)
        self.assertTrue(any("micro/hashmap_tiny_ptr_build_2 live_bytes" in line for line in lines[1:]))
        tmp = self.series(["micro/hashmap_i64_insert_4096,ns_per_op,standalone,accept,ACCEPT",
                           "micro/hashmap_i64_hit_4096,ns_per_op,standalone,regression,REGRESSION"])
        self.assertEqual(bench.series_verdict(tmp, "micro/hashmap_i64_insert_4096@standalone")[0], 1)

    def test_no_summary_is_exit_two(self):
        self.assertEqual(bench.series_verdict(Path(tempfile.mkdtemp()), "script:script@template_release")[0], 2)

    def test_base_variant_ignores_macro_order(self):
        self.assertEqual(bench.base_variant("A=1 B=1"), bench.base_variant("B=1 A=1"))
        self.assertNotEqual(bench.base_variant("A=1 B=1"), bench.base_variant("A=1"))
        self.assertRegex(bench.base_variant("A=1"), r"^b[0-9a-f]{6}$")


if __name__ == "__main__":
    unittest.main()
