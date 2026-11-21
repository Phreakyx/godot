"""Self-checks of the runner's statistics: python devtools/bench/test_bench.py"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

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


if __name__ == "__main__":
    unittest.main()
