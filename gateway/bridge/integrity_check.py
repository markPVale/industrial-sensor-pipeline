"""
integrity_check.py — End-to-End Pipeline Integrity Check (Phase 4, Step 3)

Validates three properties of data stored in InfluxDB:

  1. Sequence integrity  — seq_id strictly increases with no gaps or duplicates
                           within each boot_id. Resets to 0 only on boot change.
  2. Timestamp monotonicity — _time values are strictly increasing and spaced
                              ~500ms apart (FILTER_WINDOW_SIZE=50 at 100Hz).
  3. Data fidelity — vibration_rms matches recomputed sqrt(ax²+ay²+az²) to
                     within floating-point tolerance, confirming values are
                     unchanged through the pipeline.

Usage (on Pi):
    source .venv/bin/activate
    python3 integrity_check.py

Reads the last 10 minutes by default. Pass --minutes N to change the window.
"""

import argparse
import math
import os
import sys
from influxdb_client import InfluxDBClient

INFLUX_URL    = os.getenv("INFLUX_URL",   "http://localhost:8086")
INFLUX_TOKEN  = os.getenv("INFLUX_TOKEN", "dev-token-change-in-production")
INFLUX_ORG    = os.getenv("INFLUX_ORG",   "industrial")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET","sensors")

EXPECTED_INTERVAL_MS = 500
INTERVAL_TOLERANCE_MS = 110  # ±110ms — NTP+WiFi+ESP32 crystal jitter; worst observed ~±106ms
PAIR_TOLERANCE_MS = 50       # compensating-pair check: two consecutive intervals must sum to 2×500ms ±this
RMS_TOLERANCE = 0.01         # m/s² floating-point rounding tolerance


def query(client, minutes):
    flux = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -{minutes}m)
  |> filter(fn: (r) => r._measurement == "vibration")
  |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
  |> sort(columns: ["_time"])
"""
    tables = client.query_api().query(flux, org=INFLUX_ORG)
    records = []
    for table in tables:
        for row in table.records:
            records.append({
                "time":     row.get_time(),
                "boot_id":  int(row.values.get("boot_id", 0)),
                "seq_id":   int(row.values.get("sequence_id", 0)),
                "ax":       float(row.values.get("ax", 0.0)),
                "ay":       float(row.values.get("ay", 0.0)),
                "az":       float(row.values.get("az", 0.0)),
                "rms":      float(row.values.get("vibration_rms", 0.0)),
                "flags":    int(row.values.get("flags", 0)),
            })
    return sorted(records, key=lambda r: r["time"])


def check_sequence(records):
    print("\n--- 1. Sequence Integrity ---")
    errors = 0
    prev = None
    for r in records:
        if prev is None:
            prev = r
            continue
        same_boot = r["boot_id"] == prev["boot_id"]
        if same_boot:
            expected = prev["seq_id"] + 1
            if r["seq_id"] != expected:
                print(f"  GAP/DUP  boot={r['boot_id']}  "
                      f"seq {prev['seq_id']} -> {r['seq_id']} (expected {expected})")
                errors += 1
        else:
            if r["seq_id"] != 0:
                print(f"  BOOT RESET  boot {prev['boot_id']}->{r['boot_id']}  "
                      f"seq did not reset to 0 (got {r['seq_id']})")
                errors += 1
        prev = r
    if errors == 0:
        print(f"  PASS — {len(records)} records, no gaps or duplicates")
    else:
        print(f"  FAIL — {errors} violation(s)")
    return errors == 0


def check_timestamps(records):
    print("\n--- 2. Timestamp Monotonicity ---")
    errors = 0
    jitter_warnings = 0

    deltas = []
    prev = None
    for r in records:
        if prev is None or r["boot_id"] != prev["boot_id"]:
            prev = r
            deltas.append((r, None))
            continue
        delta_ms = (r["time"] - prev["time"]).total_seconds() * 1000
        deltas.append((r, delta_ms))
        prev = r

    i = 0
    while i < len(deltas):
        r, delta_ms = deltas[i]
        if delta_ms is None:
            i += 1
            continue
        if delta_ms <= 0:
            print(f"  NON-MONOTONIC  seq={r['seq_id']}  delta={delta_ms:.1f}ms")
            errors += 1
            i += 1
            continue
        if abs(delta_ms - EXPECTED_INTERVAL_MS) > INTERVAL_TOLERANCE_MS:
            forgiven = False
            if i + 1 < len(deltas):
                next_r, next_delta = deltas[i + 1]
                if next_delta is not None and next_r["boot_id"] == r["boot_id"]:
                    if abs(delta_ms + next_delta - 2 * EXPECTED_INTERVAL_MS) <= PAIR_TOLERANCE_MS:
                        forgiven = True
                        jitter_warnings += 1
                        i += 2
                        continue
            if not forgiven:
                print(f"  JITTER  seq={r['seq_id']}  delta={delta_ms:.1f}ms "
                      f"(expected {EXPECTED_INTERVAL_MS}±{INTERVAL_TOLERANCE_MS}ms, uncompensated)")
                errors += 1
        i += 1

    if errors == 0:
        note = f", {jitter_warnings} compensating pair(s) forgiven" if jitter_warnings else ""
        print(f"  PASS — all intervals within {EXPECTED_INTERVAL_MS}±{INTERVAL_TOLERANCE_MS}ms{note}")
    else:
        print(f"  FAIL — {errors} hard violation(s) ({jitter_warnings} compensating pair(s) forgiven)")
    return errors == 0


def check_fidelity(records):
    print("\n--- 3. Data Fidelity ---")
    errors = 0
    for r in records:
        expected_rms = math.sqrt(r["ax"]**2 + r["ay"]**2 + r["az"]**2)
        diff = abs(r["rms"] - expected_rms)
        if diff > RMS_TOLERANCE:
            print(f"  RMS MISMATCH  seq={r['seq_id']}  "
                  f"stored={r['rms']:.6f}  computed={expected_rms:.6f}  diff={diff:.6f}")
            errors += 1
    if errors == 0:
        print(f"  PASS — {len(records)} records, vibration_rms matches recomputed values")
    else:
        print(f"  FAIL — {errors} violation(s)")
    return errors == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=int, default=10)
    args = parser.parse_args()

    print(f"Integrity check — last {args.minutes} minutes from {INFLUX_URL}")

    client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    records = query(client, args.minutes)

    if not records:
        print("No records found — is the pipeline running?")
        sys.exit(1)

    boots = sorted(set(r["boot_id"] for r in records))
    print(f"Found {len(records)} records across boot_id(s): {boots}")

    seq_ok  = check_sequence(records)
    time_ok = check_timestamps(records)
    fid_ok  = check_fidelity(records)

    print("\n--- Summary ---")
    print(f"  Sequence integrity:     {'PASS' if seq_ok  else 'FAIL'}")
    print(f"  Timestamp monotonicity: {'PASS' if time_ok else 'FAIL'}")
    print(f"  Data fidelity:          {'PASS' if fid_ok  else 'FAIL'}")

    if seq_ok and time_ok and fid_ok:
        print("\nAll checks passed.")
        sys.exit(0)
    else:
        print("\nOne or more checks failed.")
        sys.exit(1)


if __name__ == "__main__":
    main()
