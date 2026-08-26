"""
test_integrity_check.py — unit tests for integrity_check.py pure helpers.

Runs without InfluxDB and without requirements.txt installed: integrity_check
defers its influxdb_client import into main(), so importing the module here
pulls in no third-party packages.

Usage:
    python3 gateway/bridge/test_integrity_check.py
"""

import contextlib
import io
import sys
from datetime import datetime, timedelta, timezone

from integrity_check import check_duplicates, dedupe

BASE = datetime(2026, 8, 26, 12, 0, 0, tzinfo=timezone.utc)


def rec(boot, seq, offset_ms=0, measurement="vibration"):
    """Minimal record shaped like query() output."""
    return {
        "time": BASE + timedelta(milliseconds=offset_ms),
        "measurement": measurement,
        "boot_id": boot,
        "seq_id": seq,
    }


def test_empty_input():
    unique, duplicates = dedupe([])
    assert unique == []
    assert duplicates == []


def test_clean_input_passes_through_in_order():
    records = [rec(1, 0, 0), rec(1, 1, 500), rec(1, 2, 1000)]
    unique, duplicates = dedupe(records)
    assert duplicates == []
    assert [r["seq_id"] for r in unique] == [0, 1, 2]
    assert unique == records


def test_duplicate_keeps_earliest_and_pairs_kept_with_duplicate():
    kept = rec(1, 1, 500)
    dup = rec(1, 1, 3500)  # same identity, later stored timestamp
    unique, duplicates = dedupe([rec(1, 0, 0), kept, dup, rec(1, 2, 4000)])

    assert [r["seq_id"] for r in unique] == [0, 1, 2]
    assert len(duplicates) == 1
    assert duplicates[0] == (kept, dup)
    # The kept row is the earlier one; the pair is ordered (kept, duplicate).
    assert duplicates[0][0]["time"] < duplicates[0][1]["time"]


def test_same_seq_under_different_boot_is_not_a_duplicate():
    # Boot reset: sequence restarts at 0. A seq_id-only key would wrongly
    # collapse these into one record and hide a whole boot cycle.
    records = [rec(1, 0, 0), rec(1, 1, 500), rec(2, 0, 1000), rec(2, 1, 1500)]
    unique, duplicates = dedupe(records)
    assert duplicates == []
    assert len(unique) == 4


def test_same_identity_across_measurements_is_flagged():
    # Telemetry and fault records share one boot_id + sequence_id stream, so
    # the same identity in both measurements is a defect, not two valid rows.
    vib = rec(1, 7, 0, measurement="vibration")
    fault = rec(1, 7, 250, measurement="sensor_faults")
    unique, duplicates = dedupe([vib, fault])

    assert len(unique) == 1
    assert len(duplicates) == 1
    assert duplicates[0] == (vib, fault)


def test_multiple_duplicates_of_one_identity_all_reported():
    kept = rec(1, 4, 0)
    dup_a = rec(1, 4, 100)
    dup_b = rec(1, 4, 200)
    unique, duplicates = dedupe([kept, dup_a, dup_b])

    assert len(unique) == 1
    assert duplicates == [(kept, dup_a), (kept, dup_b)]


def test_check_duplicates_passes_when_none():
    ok, count = check_duplicates([])
    assert ok is True
    assert count == 0


def test_check_duplicates_warns_on_same_measurement_retry():
    # At-least-once delivery permits retries — must not fail the run.
    kept = rec(1, 1, 0)
    dup = rec(1, 1, 3000)
    ok, count = check_duplicates([(kept, dup)])
    assert ok is True, "a same-measurement retry must not fail validation"
    assert count == 1


def test_check_duplicates_fails_on_cross_measurement_collision():
    # The bridge routes on payload flags, so a retry cannot change measurement.
    # The same identity in both means a reused sequence or corrupted payload.
    kept = rec(1, 7, 0, measurement="vibration")
    dup = rec(1, 7, 0, measurement="sensor_faults")  # same timestamp, differing measurement
    ok, count = check_duplicates([(kept, dup)])
    assert ok is False, "a cross-measurement identity collision must fail validation"
    assert count == 1


def test_check_duplicates_fails_if_any_collision_among_retries():
    retry = (rec(1, 1, 0), rec(1, 1, 3000))
    collision = (rec(1, 7, 0, measurement="vibration"),
                 rec(1, 7, 0, measurement="sensor_faults"))
    ok, count = check_duplicates([retry, collision])
    assert ok is False
    assert count == 2


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failures = 0
    for test in tests:
        try:
            # check_duplicates prints its report; swallow it so the pass/fail
            # lines below stay readable in CI output.
            with contextlib.redirect_stdout(io.StringIO()):
                test()
            print(f"  ok   {test.__name__}")
        except AssertionError as exc:
            failures += 1
            # Bare asserts carry no message; report the failing line instead.
            tb = exc.__traceback__
            while tb.tb_next:
                tb = tb.tb_next
            detail = str(exc) or "assertion failed"
            print(f"  FAIL {test.__name__} (line {tb.tb_lineno}): {detail}")
    print(f"\n{len(tests) - failures}/{len(tests)} passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
