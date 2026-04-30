# Firmware State Model

State inventory for the ESP32-S3 sensor node. Covers all explicit state held
across tasks, structs, and globals. Updated as the firmware evolves.

---

## NodeState
Connectivity state machine. All tasks key their behavior off this single enum.

- `NORMAL` — MQTT connected; telemetryTask pops and publishes live at 2Hz
- `BUFFERING` — MQTT disconnected; records accumulate in PSRAM, nothing publishes
- `SYNCING` — MQTT reconnected; syncTask burst-drains buffer, telemetryTask resumes

Transitions:
- `NORMAL → BUFFERING` on MQTT disconnect
- `BUFFERING → SYNCING` on MQTT reconnect
- `SYNCING → NORMAL` when buffer is empty

---

## TelemetryRecord
Output of the filter pipeline. 44 bytes. Persisted in PSRAM, serialised to MQTT JSON.

- `boot_id` — increments each reboot (persisted in NVS)
- `sequence_id` — monotonic counter, resets to 0 each boot
- `timestamp_ms` — UTC epoch ms (NTP-anchored) or millis() before sync
- `accel_x/y/z` — Kalman-filtered acceleration, m/s²
- `gyro_x/y/z` — Kalman-filtered angular velocity, rad/s
- `status_flags` — bitmask (see Status Flags below)

---

## RawSample
Internal struct. Lives only on the FreeRTOS queue between sensorTask and filterTask.
Never persisted or serialised.

- `timestamp_ms`
- `accel_x/y/z` — calibrated raw accel, m/s²
- `gyro_x/y/z` — raw gyro, rad/s
- `status_flags`

---

## Status Flags
Bitmask on TelemetryRecord (and RawSample). Bits OR'd together across the 50-sample window.

| Flag | Value | Set when |
|------|-------|----------|
| `STATUS_OK` | `0x00` | Normal sample |
| `STATUS_ACCEL_CLIPPED` | `0x01` | Accel hit hardware range limit |
| `STATUS_GYRO_CLIPPED` | `0x02` | Gyro hit hardware range limit |
| `STATUS_INTERLOCK_OPEN` | `0x04` | Safety interlock was open at sample time |
| `STATUS_ANOMALY` | `0x08` | RMS exceeded anomaly threshold |
| `STATUS_SENSOR_FAULT` | `0x10` | I2C dropout — consecutive zero reads (planned) |

---

## BufferManager State
PSRAM ring buffer. State is implicit — no enum.

- `_buffer` — pointer to PSRAM allocation (nullptr = uninitialised)
- `_head` — next write slot index
- `_tail` — next read slot index
- `_count` — records currently held
- `_dropped` — cumulative evictions due to overflow since last clear()
- `_capacity` — total slots (immutable after begin())

Overflow policy: evicts oldest record, increments `_dropped`. Preserves most-recent data.

---

## Safety State

- `g_interlockActive` — `std::atomic<bool>`; set by `safetyISR` via `exchange(true)`;
  latches until reboot. filterTask stamps `STATUS_INTERLOCK_OPEN` on all records while set.

---

## NTP State

- `g_ntpSynced` — `std::atomic<bool>`; set once when first valid epoch received
- `g_ntpEpochMs` — `std::atomic<uint64_t>`; epoch ms at sync moment
- `g_millisAtSync` — `std::atomic<uint32_t>`; millis() value at sync moment

Epoch derivation: `g_ntpEpochMs + (millis() - g_millisAtSync)`

---

## MQTT / Event State

- `g_mqttEvents` — FreeRTOS EventGroup
  - `kBitConnected` — MQTT connected
  - `kBitDisconnected` — MQTT disconnected
  - `kBitReconnect` — reconnected after a disconnect (triggers syncTask drain)
- `g_publishQueue` — FreeRTOS queue of `MqttMessage`; connectionTask is sole drainer

---

## Sensor Health State (planned)

No sensor-level state currently exists. Planned addition for I2C dropout detection:

- `consecutiveZeroReads` — local counter in sensorTask; resets on any non-zero read
- Threshold: 5 consecutive zeros (50ms at 100Hz) triggers a fault record
- Fault record: `TelemetryRecord` with `STATUS_SENSOR_FAULT` pushed directly to
  `g_buffer`, bypassing filterTask. Zero sensor values, valid boot_id/sequence_id/timestamp.
- Bridge routes `STATUS_SENSOR_FAULT` records to `sensor_faults` InfluxDB measurement.
