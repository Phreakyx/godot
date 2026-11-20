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


class Counts(unittest.TestCase):
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
