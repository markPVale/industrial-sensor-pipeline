"""Bridge boundary tests: real Influx Point serialization, mocked network I/O.

Install requirements.txt, then run: python gateway/bridge/test_mqtt_to_influx.py
No broker or database required.
"""
import json
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

import mqtt_to_influx as bridge


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.writer = Mock()
        self.patcher = patch.object(bridge, "write_api", self.writer)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        self.client = Mock()
        self.client.publish.return_value.rc = 0
        self.payload = dict(boot=7, seq=42, ts=1710000000123, flags=0,
                            ax=0.0, ay=0.0, az=9.81, gx=0.0, gy=0.0, gz=0.0, wrms=9.81)

    def write(self):
        bridge.write_telemetry(self.client, "node01", self.payload)
        return self.writer.write.call_args.kwargs["record"].to_line_protocol()

    def test_valid_record_ack_follows_write(self):
        order = []
        self.writer.write.side_effect = lambda **kwargs: order.append("write")
        self.client.publish.side_effect = lambda *args, **kwargs: (order.append("ack") or SimpleNamespace(rc=0))
        line = self.write()
        self.assertEqual(order, ["write", "ack"])
        self.assertIn("window_rms=9.81", line)
        self.assertIn("data_complete=true", line)
        self.assertEqual(json.loads(self.client.publish.call_args.args[1]), {"boot": 7, "seq": 42})

    def test_mixed_null_values_are_omitted_not_zeroed(self):
        self.payload.update(ax=None, gy=None, wrms=None)
        line = self.write()
        for field in ("ax=", "gy=", "vibration_rms=", "window_rms="):
            self.assertNotIn(field, line)
        self.assertIn("az=9.81", line)
        self.assertIn('unavailable_fields="ax,gy,wrms"', line)
        self.assertIn("data_complete=false", line)

    def test_all_invalid_persists_identity_and_evidence_before_ack(self):
        for key in ("ax", "ay", "az", "gx", "gy", "gz", "wrms"):
            self.payload[key] = None
        line = self.write()
        self.assertTrue(line.startswith("vibration,"))
        self.assertIn('reason="invalid_telemetry"', line)
        self.assertIn("boot_id=7i", line)
        self.assertIn("sequence_id=42i", line)
        self.assertNotIn("vibration_rms=", line)
        self.client.publish.assert_called_once()

    def test_nonfinite_numbers_are_unavailable(self):
        self.payload.update(ax=float("nan"), ay=float("inf"), az=-float("inf"))
        line = self.write()
        self.assertNotIn("vibration_rms=", line)
        self.assertIn('unavailable_fields="ax,ay,az"', line)

    def test_extreme_finite_magnitude_does_not_overflow_intermediate_square(self):
        self.payload.update(ax=1e200, ay=1e200, az=1e200)
        line = self.write()
        self.assertIn("vibration_rms=1.732", line)

    def test_missing_legacy_wrms_falls_back_but_null_does_not(self):
        del self.payload["wrms"]
        self.assertIn("window_rms=9.81", self.write())
        self.payload["wrms"] = None
        self.assertNotIn("window_rms=", self.write())

    def test_fault_null_whoami_is_not_fabricated(self):
        self.payload.update(flags=0x20, ax=None, wrms=None)
        line = self.write()
        self.assertTrue(line.startswith("sensor_faults,"))
        self.assertNotIn("whoami_raw=", line)
        self.assertIn('unavailable_fields="ax"', line)

    def test_database_failure_never_acks_valid_or_invalid_records(self):
        self.writer.write.side_effect = RuntimeError("database offline")
        for invalid in (False, True):
            if invalid:
                for key in ("ax", "ay", "az", "gx", "gy", "gz", "wrms"):
                    self.payload[key] = None
            with self.assertRaises(RuntimeError):
                self.write()
            self.client.publish.assert_not_called()

    def test_wrong_measurement_type_or_identity_does_not_ack(self):
        for changes in ({"ax": "bad"}, {"boot": None}, {"seq": True}, {"seq": -1}):
            with self.subTest(changes=changes):
                payload = dict(self.payload, **changes)
                with self.assertRaises(ValueError):
                    bridge.write_telemetry(self.client, "node01", payload)
        self.writer.write.assert_not_called()
        self.client.publish.assert_not_called()

    def test_malformed_json_never_acks(self):
        bridge.on_message(self.client, None, SimpleNamespace(
            topic="sensor/node01/telemetry", payload=b'{"boot":7,"seq":42,"ax":nan}'))
        self.writer.write.assert_not_called()
        self.client.publish.assert_not_called()

    def test_ack_failure_does_not_erase_persisted_evidence(self):
        self.client.publish.return_value.rc = 1
        with self.assertRaises(RuntimeError):
            self.write()
        self.writer.write.assert_called_once()

    def test_invalid_retry_has_stable_persisted_identity_and_fields(self):
        for key in ("ax", "ay", "az", "gx", "gy", "gz", "wrms"):
            self.payload[key] = None
        self.assertEqual(self.write(), self.write())
        self.assertEqual(self.writer.write.call_count, 2)
        self.assertEqual(self.client.publish.call_count, 2)


if __name__ == "__main__":
    unittest.main()
