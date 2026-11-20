"""Self-checks of the runner's statistics: python devtools/bench/test_bench.py"""

import sys
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


if __name__ == "__main__":
    unittest.main()
