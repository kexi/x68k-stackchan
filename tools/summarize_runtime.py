#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Summarize CoreS3 slice profiles and separately bounded PCM observations as JSON."""

import argparse
import json
import re
import sys
from itertools import pairwise
from pathlib import Path

TAG = re.compile(r"\[(run-profile|run-profile-pc|audio-continuity)\](.*)")
STAMP = re.compile(r"\bI\s*\((\d+)\)")
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
REQUIRED = {
    "run-profile": (
        "from_ms",
        "to_ms",
        "render_us",
        "clock_waits",
        "queue_waits",
        "other_samples",
        "other_us",
        "other_cycles",
        "overflow_samples",
    ),
    "run-profile-pc": ("to_ms", "samples", "us", "cycles"),
    "audio-continuity": ("source_frames", "missing_frames"),
}


def issue(code, line, detail):
    return {"code": code, "line": line, "detail": detail}


def parse_record(line, number, problems):
    clean = ANSI.sub("", line)
    match = TAG.search(clean)
    has_record = match is not None
    if not has_record:
        return None
    tag, payload = match.groups()
    stamp = STAMP.search(clean[: match.start()])
    try:
        has_stamp = stamp is not None
        if not has_stamp:
            raise ValueError("missing ESP I(t) timestamp")
        fields = {}
        for token in payload.split():
            key, value = token.split("=", 1)
            duplicate = key in fields
            if duplicate:
                raise ValueError(f"duplicate field: {key}")
            fields[key] = value
        numeric = REQUIRED[tag] + tuple(key for key in ("pc_rows", "muted_frames") if key in fields)
        for key in numeric:
            fields[key] = int(fields[key], 10)
            negative = fields[key] < 0
            if negative:
                raise ValueError(f"negative field: {key}")
        is_pc = tag == "run-profile-pc"
        if is_pc:
            page = int(fields["page"], 16)
            valid_page = 0 <= page <= 0xFFFF00 and page % 256 == 0
            if not valid_page:
                raise ValueError("page is not a 24-bit 256-byte page")
            fields["page"] = f"{page:06X}"
            has_first_pc = "first_pc" in fields
            if has_first_pc:
                pc = int(fields["first_pc"], 16)
                same_page = 0 <= pc <= 0xFFFFFF and pc & 0xFFFF00 == page
                if not same_page:
                    raise ValueError("first_pc does not belong to page")
                fields["first_pc"] = f"{pc:06X}"
        return {"tag": tag, "line": number, "at_ms": int(stamp[1]), **fields}
    except (ValueError, KeyError) as error:
        problems.append(issue("malformed_record", number, f"{tag}: {error}"))
        return {"tag": tag, "line": number, "malformed": True}


def pcm_window(samples, start, end, max_gap_ms):
    result = {
        "selection": "nearest_sample_to_each_ESP_boundary_ties_earlier",
        "exact_profile_bounds": False,
        "issues": [],
    }
    valid_bounds = start is not None and end is not None
    if not valid_bounds:
        result["issues"].append("invalid_profile_bounds")
        return result
    bad_timeline = any(b["at_ms"] <= a["at_ms"] for a, b in pairwise(samples))
    if bad_timeline:
        result["issues"].append("non_increasing_audio_time")
        return result
    has_points = bool(samples)
    if not has_points:
        result["issues"].append("missing_boundary_sample")
        return result
    left = min(range(len(samples)), key=lambda index: (abs(samples[index]["at_ms"] - start), index))
    right = min(range(len(samples)), key=lambda index: (abs(samples[index]["at_ms"] - end), index))
    a, b = samples[left], samples[right]
    result.update(
        {
            "from": a,
            "to": b,
            "duration_ms": b["at_ms"] - a["at_ms"],
            "start_offset_ms": a["at_ms"] - start,
            "end_offset_ms": b["at_ms"] - end,
            "profile_overlap_ms": max(0, min(b["at_ms"], end) - max(a["at_ms"], start)),
            "exact_profile_bounds": a["at_ms"] == start and b["at_ms"] == end,
            "source_frames_delta": b["source_frames"] - a["source_frames"],
            "missing_frames_delta": b["missing_frames"] - a["missing_frames"],
        }
    )
    enough_points = right > left and b["at_ms"] > a["at_ms"]
    if not enough_points:
        result["issues"].append("no_distinct_ordered_samples")
    observed = samples[left : right + 1]
    for before, after in pairwise(observed):
        gap = after["at_ms"] - before["at_ms"]
        bad_time = gap <= 0
        if bad_time:
            result["issues"].append("non_increasing_audio_time")
        missing_sample = gap > max_gap_ms
        if missing_sample:
            result["issues"].append("audio_sample_gap")
        backwards = any(after[key] < before[key] for key in ("source_frames", "missing_frames"))
        if backwards:
            result["issues"].append("audio_counter_decreased")
    stale_boundary = abs(start - a["at_ms"]) > max_gap_ms or abs(end - b["at_ms"]) > max_gap_ms
    if stale_boundary:
        result["issues"].append("stale_boundary_sample")
    frames = result["source_frames_delta"] + result["missing_frames_delta"]
    ratio_valid = not result["issues"] and frames > 0
    result["missing_fraction"] = result["missing_frames_delta"] / frames if ratio_valid else None
    result["issues"] = sorted(set(result["issues"]))
    return result


def summarize(lines, *, max_audio_gap_ms=2000):
    positive_gap = max_audio_gap_ms > 0
    if not positive_gap:
        raise ValueError("max_audio_gap_ms must be positive")
    problems, profiles, samples = [], [], []
    current = None
    for number, line in enumerate(lines, 1):
        record = parse_record(line, number, problems)
        ignored = record is None
        if ignored:
            continue
        malformed = record.get("malformed", False)
        if malformed:
            has_profile = current is not None
            if has_profile and record["tag"] == "run-profile-pc":
                current["issues"].append("malformed_pc_row")
            starts_profile = record["tag"] == "run-profile"
            if starts_profile:
                current = None
            continue
        tag = record.pop("tag")
        is_profile = tag == "run-profile"
        if is_profile:
            current = {**record, "pc": [], "issues": []}
            profiles.append(current)
            continue
        is_audio = tag == "audio-continuity"
        if is_audio:
            samples.append(record)
            continue
        matching_profile = current is not None and current["to_ms"] == record["to_ms"]
        if not matching_profile:
            problems.append(issue("orphan_pc_row", number, "no preceding matching profile"))
            continue
        current["pc"].append(record)

    for previous, sample in pairwise(samples):
        non_increasing = sample["at_ms"] <= previous["at_ms"]
        if non_increasing:
            problems.append(
                issue(
                    "non_increasing_audio_time",
                    sample["line"],
                    "possible duplicate, reset, or reordered log",
                )
            )
        decreased = any(sample[key] < previous[key] for key in ("source_frames", "missing_frames"))
        if decreased:
            problems.append(
                issue(
                    "audio_counter_decreased", sample["line"], "possible reset; not a valid delta"
                )
            )
        gap = sample["at_ms"] - previous["at_ms"]
        missing_sample = gap > max_audio_gap_ms
        if missing_sample:
            problems.append(
                issue(
                    "audio_sample_gap",
                    sample["line"],
                    f"{gap} ms exceeds {max_audio_gap_ms} ms threshold",
                )
            )

    previous = None
    for profile in profiles:
        flags = profile["issues"]
        duration = profile["to_ms"] - profile["from_ms"]
        valid_duration = duration > 0
        if not valid_duration:
            flags.append("non_positive_profile_duration")
        has_previous = previous is not None
        if has_previous:
            discontinuity = profile["from_ms"] - previous["to_ms"]
            profile["tick_gap_from_previous_ms"] = discontinuity
            has_gap = discontinuity != 0
            if has_gap:
                flags.append(
                    "profile_gap_or_restart" if discontinuity > 0 else "profile_overlap_or_reset"
                )
            duplicate = profile["to_ms"] == previous["to_ms"]
            if duplicate:
                flags.append("duplicate_profile_end")
                previous["issues"].append("duplicate_profile_end")
        previous = profile
        unique = {}
        for pc in profile["pc"]:
            duplicate = pc["page"] in unique
            if duplicate:
                flags.append("duplicate_pc_page")
                continue
            unique[pc["page"]] = pc
        count = len(profile["pc"])
        too_many = count > 4
        if too_many:
            flags.append("too_many_pc_rows")
        reported_count = profile.get("pc_rows")
        knows_count = reported_count is not None
        profile["pc_completeness"] = (
            "reported_count" if knows_count else "unverifiable_without_pc_rows"
        )
        mismatched_count = knows_count and (reported_count > 4 or count != reported_count)
        if mismatched_count:
            flags.append("pc_row_count_mismatch")
        missing_top = profile["other_samples"] > 0 and len(unique) < 4
        if missing_top:
            flags.append("missing_top_pc_rows")
        no_pc = not unique and (not knows_count or reported_count != 0)
        if no_pc:
            flags.append("no_pc_rows")
        invalid_overflow = profile["overflow_samples"] > profile["other_samples"]
        if invalid_overflow:
            flags.append("overflow_exceeds_other_samples")
        run_us = profile["other_us"] + sum(pc["us"] for pc in unique.values())
        profile.update(
            {
                "duration_ms": duration,
                "wall_from_ms": profile["at_ms"] - duration if valid_duration else None,
                "wall_to_ms": profile["at_ms"],
                "esp_minus_tick_ms": profile["at_ms"] - profile["to_ms"],
                "run_us": run_us,
                "run_cycles": profile["other_cycles"] + sum(pc["cycles"] for pc in unique.values()),
                "run_samples": profile["other_samples"]
                + sum(pc["samples"] for pc in unique.values()),
                "unaccounted_us": duration * 1000 - run_us - profile["render_us"]
                if valid_duration
                else None,
                "overflow": {"samples": profile["overflow_samples"], "included_in_other": True},
            }
        )
        negative_unaccounted = valid_duration and profile["unaccounted_us"] < 0
        if negative_unaccounted:
            flags.append("negative_unaccounted_us")
        profile["pcm"] = pcm_window(
            samples, profile["wall_from_ms"], profile["wall_to_ms"], max_audio_gap_ms
        )
        profile["issues"] = sorted(set(flags))

    no_profiles = not profiles
    if no_profiles:
        problems.append(issue("no_profiles", None, "no valid run-profile records"))
    no_audio = not samples
    if no_audio:
        problems.append(issue("no_audio_samples", None, "no valid audio-continuity records"))
    has_issues = bool(problems) or any(
        profile["issues"] or profile["pcm"]["issues"] for profile in profiles
    )
    return {
        "schema_version": 1,
        "has_issues": has_issues,
        "limitations": [
            "Profile wall end is ESP I(t); start subtracts the tick-clock duration."
            " Tick and ESP absolute timestamps are not interchangeable.",
            "run_us is the observed sum of unique PC rows plus other_us;"
            " row completeness is unverifiable unless pc_rows is logged.",
            "PC buckets identify slice entry pages, not time spent executing that page."
            " code_at_report is not a code snapshot taken at slice entry.",
            "other includes overflow already. unaccounted_us includes waits, scheduling,"
            " logging, and unmeasured or missing work; it is not idle time.",
            "clock_waits and queue_waits count iterations, not microseconds; they can overlap.",
            "PCM deltas use their reported two sample timestamps without interpolation;"
            " they do not establish PCM shortage in the exact profile window.",
            "Nearest PCM observations may reuse a boundary sample across profiles;"
            " inspect their timestamps before combining deltas.",
            "Timestamp resets, reordered records, malformed lines, and gaps require"
            " inspection; no boot/session offset correction is attempted.",
        ],
        "max_audio_gap_ms": max_audio_gap_ms,
        "issues": problems,
        "profiles": profiles,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--max-audio-gap-ms", type=int, default=2000)
    parser.add_argument(
        "--strict", action="store_true", help="exit 1 after JSON output when detected issues exist"
    )
    args = parser.parse_args(argv)
    try:
        with args.log.open(encoding="utf-8", errors="replace") as source:
            result = summarize(source, max_audio_gap_ms=args.max_audio_gap_ms)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return int(args.strict and result["has_issues"])


if __name__ == "__main__":
    raise SystemExit(main())
