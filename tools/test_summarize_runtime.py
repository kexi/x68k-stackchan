#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Guarantees for clock separation, observed sums, and incomplete runtime logs."""

import io
import json
import unittest
from unittest.mock import patch

from summarize_runtime import main, summarize


def profile(at=2940, start=1000, end=2000, **changes):
    fields = dict(
        from_ms=start,
        to_ms=end,
        render_us=100000,
        clock_waits=10,
        queue_waits=20,
        other_samples=0,
        other_us=0,
        other_cycles=0,
        overflow_samples=0,
    )
    fields.update(changes)
    return f"I ({at}) x68k: [run-profile] " + " ".join(f"{k}={v}" for k, v in fields.items())


def pc(page="035700", *, at=2941, end=2000, samples=5, us=200000, cycles=300000, **changes):
    fields = dict(to_ms=end, page=page, samples=samples, us=us, cycles=cycles)
    fields.update(changes)
    return f"I ({at}) x68k: [run-profile-pc] " + " ".join(f"{k}={v}" for k, v in fields.items())


def audio(at, source, missing):
    return (
        f"I ({at}) x68k: [audio-continuity]"
        f" source_frames={source} missing_frames={missing} muted_frames=0"
    )


class RuntimeSummaryTests(unittest.TestCase):
    def window(self, *records, **options):
        return summarize(records, **options)["profiles"][0]

    def test_two_clocks_remain_separate_and_pcm_uses_actual_sample_bounds(self):
        result = self.window(
            audio(900, 0, 0),
            audio(1900, 1000, 50),
            profile(),
            pc(),
            audio(2900, 2000, 100),
        )
        self.assertEqual((result["wall_from_ms"], result["wall_to_ms"]), (1940, 2940))
        self.assertEqual(result["esp_minus_tick_ms"], 940)
        self.assertEqual(result["run_us"], 200000)
        self.assertEqual(result["unaccounted_us"], 700000)
        observed = result["pcm"]
        self.assertEqual((observed["from"]["at_ms"], observed["to"]["at_ms"]), (1900, 2900))
        self.assertEqual(observed["source_frames_delta"], 1000)
        self.assertEqual(observed["missing_frames_delta"], 50)
        self.assertEqual((observed["start_offset_ms"], observed["end_offset_ms"]), (-40, -40))
        self.assertEqual(observed["profile_overlap_ms"], 960)
        self.assertFalse(observed["exact_profile_bounds"])
        self.assertAlmostEqual(observed["missing_fraction"], 50 / 1050)

    def test_top_four_and_other_include_overflow_exactly_once(self):
        result = self.window(
            profile(other_samples=10, other_us=50000, other_cycles=100, overflow_samples=3),
            *(pc(f"{page:06X}", us=100000) for page in range(0x100, 0x500, 0x100)),
        )
        self.assertEqual(result["run_us"], 450000)
        self.assertEqual(result["run_samples"], 30)
        self.assertEqual(result["run_cycles"], 1200100)
        self.assertEqual(result["unaccounted_us"], 450000)
        self.assertEqual(result["overflow"], {"samples": 3, "included_in_other": True})

    def test_interleaved_audio_does_not_break_pc_group(self):
        result = self.window(profile(), pc(), audio(2942, 1000, 0), pc("038600"))
        self.assertEqual(len(result["pc"]), 2)
        self.assertEqual(result["run_us"], 400000)

    def test_optional_code_remains_report_time_evidence(self):
        result = self.window(profile(), pc(first_pc="0357a2", code_at_report="123456"))
        self.assertEqual(result["pc"][0]["first_pc"], "0357A2")
        self.assertEqual(result["pc"][0]["code_at_report"], "123456")
        self.assertEqual(result["pc_completeness"], "unverifiable_without_pc_rows")

    def test_reported_pc_count_detects_missing_last_row(self):
        result = self.window(profile(pc_rows=2), pc())
        self.assertIn("pc_row_count_mismatch", result["issues"])

    def test_unreported_count_with_other_proves_missing_top_rows(self):
        result = self.window(profile(other_samples=1, other_us=10), pc())
        self.assertIn("missing_top_pc_rows", result["issues"])

    def test_no_pc_rows_are_not_silently_treated_as_zero_run(self):
        self.assertIn("no_pc_rows", self.window(profile())["issues"])
        known_empty = self.window(profile(pc_rows=0))
        self.assertEqual(known_empty["run_us"], 0)
        self.assertEqual(known_empty["issues"], [])

    def test_duplicate_pc_rows_are_flagged_and_not_double_counted(self):
        result = self.window(profile(), pc(), pc())
        self.assertIn("duplicate_pc_page", result["issues"])
        self.assertEqual(result["run_us"], 200000)
        self.assertEqual(len(result["pc"]), 2)

    def test_excess_pc_rows_and_inconsistent_overflow_are_flagged(self):
        result = self.window(
            profile(overflow_samples=1),
            *(pc(f"{page:06X}") for page in range(0x100, 0x600, 0x100)),
        )
        self.assertIn("too_many_pc_rows", result["issues"])
        self.assertIn("overflow_exceeds_other_samples", result["issues"])
        self.assertIn("negative_unaccounted_us", result["issues"])

    def test_missing_and_duplicate_profile_windows_are_visible(self):
        result = summarize(
            [
                profile(),
                pc(),
                profile(at=4940, start=3000, end=4000),
                pc(end=4000),
                profile(at=4940, start=3000, end=4000),
                pc(end=4000),
            ]
        )
        second = result["profiles"][1]
        self.assertIn("profile_gap_or_restart", second["issues"])
        self.assertEqual(second["tick_gap_from_previous_ms"], 1000)
        self.assertIn("duplicate_profile_end", second["issues"])
        self.assertIn("duplicate_profile_end", result["profiles"][2]["issues"])
        self.assertTrue(result["has_issues"])

    def test_invalid_duration_does_not_produce_a_plausible_wall_window(self):
        result = self.window(profile(start=3000), pc())
        self.assertIsNone(result["wall_from_ms"])
        self.assertIsNone(result["unaccounted_us"])
        self.assertIn("non_positive_profile_duration", result["issues"])

    def test_orphan_and_malformed_rows_are_flagged(self):
        result = summarize(
            [
                pc(),
                profile(),
                pc(end=4000),
                pc(samples=-1),
                "I (3000) x68k: [audio-continuity] source_frames=1",
                "[run-profile] from_ms=1",
                profile() + " to_ms=2000",
            ]
        )
        codes = [entry["code"] for entry in result["issues"]]
        self.assertEqual(codes.count("orphan_pc_row"), 2)
        self.assertEqual(codes.count("malformed_record"), 4)
        self.assertIn("malformed_pc_row", result["profiles"][0]["issues"])

    def test_invalid_page_or_first_pc_cannot_become_a_valid_bucket(self):
        for row in (pc("035701"), pc("1000000"), pc(first_pc="036700")):
            with self.subTest(row=row):
                result = summarize([profile(), row])
                self.assertTrue(result["has_issues"])
                self.assertEqual(result["profiles"][0]["pc"], [])

    def test_negative_cumulative_deltas_disable_missing_fraction(self):
        result = self.window(audio(1900, 100, 50), profile(), pc(), audio(2900, 50, 20))
        self.assertEqual(result["pcm"]["source_frames_delta"], -50)
        self.assertEqual(result["pcm"]["missing_frames_delta"], -30)
        self.assertIsNone(result["pcm"]["missing_fraction"])
        self.assertIn("audio_counter_decreased", result["pcm"]["issues"])

    def test_interior_counter_reset_is_detected_even_when_endpoint_delta_is_positive(self):
        result = self.window(
            audio(1900, 100, 50),
            profile(),
            pc(),
            audio(2200, 0, 0),
            audio(2900, 1000, 100),
        )
        self.assertEqual(result["pcm"]["source_frames_delta"], 900)
        self.assertIsNone(result["pcm"]["missing_fraction"])

    def test_reboot_or_duplicate_timestamp_is_not_joined_to_another_session(self):
        for at in (100, 1900):
            with self.subTest(at=at):
                result = self.window(audio(1900, 100, 50), profile(), pc(), audio(at, 0, 0))
                self.assertIn("non_increasing_audio_time", result["pcm"]["issues"])
                self.assertNotIn("missing_fraction", result["pcm"])

    def test_pcm_gaps_and_stale_boundaries_are_visible(self):
        result = self.window(
            audio(100, 10, 0),
            profile(at=7000, start=1000, end=6000),
            pc(end=6000),
            audio(5000, 100, 10),
            max_audio_gap_ms=1500,
        )
        self.assertIn("audio_sample_gap", result["pcm"]["issues"])
        self.assertIn("stale_boundary_sample", result["pcm"]["issues"])
        self.assertIsNone(result["pcm"]["missing_fraction"])

    def test_missing_or_single_pcm_boundary_cannot_claim_zero_shortage(self):
        no_audio = self.window(profile(), pc())
        self.assertIn("missing_boundary_sample", no_audio["pcm"]["issues"])
        one_point = self.window(audio(1900, 100, 0), profile(), pc())
        self.assertIn("no_distinct_ordered_samples", one_point["pcm"]["issues"])
        self.assertIsNone(one_point["pcm"]["missing_fraction"])

    def test_nearest_samples_avoid_attributing_almost_an_entire_previous_window(self):
        result = self.window(
            audio(900, 0, 0),
            audio(1941, 100, 5),
            profile(),
            pc(),
            audio(2941, 200, 10),
        )
        observed = result["pcm"]
        self.assertEqual((observed["from"]["at_ms"], observed["to"]["at_ms"]), (1941, 2941))
        self.assertEqual((observed["start_offset_ms"], observed["end_offset_ms"]), (1, 1))
        self.assertEqual(observed["profile_overlap_ms"], 999)
        self.assertFalse(observed["exact_profile_bounds"])

    def test_equally_near_samples_choose_earlier_timestamp(self):
        result = self.window(
            audio(1840, 0, 0),
            audio(2040, 100, 0),
            profile(),
            pc(),
            audio(2840, 200, 0),
            audio(3040, 300, 0),
        )
        self.assertEqual(result["pcm"]["from"]["at_ms"], 1840)
        self.assertEqual(result["pcm"]["to"]["at_ms"], 2840)

    def test_ansi_logs_and_exact_pcm_boundaries_are_supported(self):
        result = self.window(
            audio(1940, 0, 0),
            "\x1b[0;32m" + profile() + "\x1b[0m",
            pc(),
            audio(2940, 100, 0),
        )
        self.assertTrue(result["pcm"]["exact_profile_bounds"])
        self.assertEqual(result["pcm"]["missing_fraction"], 0)

    def test_empty_log_and_invalid_gap_threshold_do_not_pass_silently(self):
        result = summarize(["unrelated output"])
        self.assertTrue(result["has_issues"])
        self.assertEqual(
            {entry["code"] for entry in result["issues"]}, {"no_profiles", "no_audio_samples"}
        )
        with self.assertRaises(ValueError):
            summarize([], max_audio_gap_ms=0)

    def test_strict_cli_still_emits_json_before_failing(self):
        with (
            patch("pathlib.Path.open", return_value=io.StringIO("")),
            patch("sys.stdout", new_callable=io.StringIO) as output,
        ):
            status = main(["capture.log", "--strict"])
            result = json.loads(output.getvalue())
        self.assertEqual(status, 1)
        self.assertTrue(result["has_issues"])


if __name__ == "__main__":
    unittest.main()
