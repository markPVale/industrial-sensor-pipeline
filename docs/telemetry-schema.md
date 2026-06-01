# Telemetry Payload Schema — v1

This document is the **single source of truth** for the telemetry data contract.
The firmware, mock script, MQTT bridge, MCP server, and dashboard must all agree
with this specification. When a field changes here, every layer must be updated
before the change is merged.

---

## Telemetry Message

**MQTT topic:** `sensor/<node_id>/telemetry`
**QoS:** 0 (PubSubClient default — not end-to-end guaranteed delivery)
**Format:** UTF-8 JSON, single object per message

### Fields

| Field | Type | Units | Source | Description |
|-------|------|-------|--------|-------------|
| `boot` | uint32 | — | Firmware | Boot cycle counter. Loaded from NVS, incremented each reboot. Identifies which boot session produced this record. |
| `seq` | uint32 | — | Firmware | Per-boot sequence counter. Resets to 0 on each boot. Monotonically increasing within a session. Together with `boot`, uniquely identifies a record. |
| `ts` | uint64 | epoch ms | Firmware | Edge-side timestamp. UTC epoch ms after NTP sync; monotonic millis fallback before sync. Used for InfluxDB record time when it looks like a real epoch. |
| `ax` | float32 | m/s² | Firmware | Kalman-filtered accelerometer X axis. Gravity-inclusive — approximately 0 on a flat horizontal surface (az carries ~9.81 m/s²). |
| `ay` | float32 | m/s² | Firmware | Kalman-filtered accelerometer Y axis. |
| `az` | float32 | m/s² | Firmware | Kalman-filtered accelerometer Z axis. ~9.81 m/s² on a stationary flat surface. |
| `gx` | float32 | rad/s | Firmware | Kalman-filtered gyroscope X axis. |
| `gy` | float32 | rad/s | Firmware | Kalman-filtered gyroscope Y axis. |
| `gz` | float32 | rad/s | Firmware | Kalman-filtered gyroscope Z axis. |
| `wrms` | float32 | m/s² | Firmware | Rolling RMS over `FILTER_WINDOW_SIZE` samples. This is the anomaly metric. |
| `flags` | uint8 | — | Firmware | Status bitmask. See flag definitions below. |

### Example

```json
{
  "boot": 1,
  "seq": 42,
  "ts": 1710000000123,
  "ax": -0.1234,
  "ay":  0.0987,
  "az":  9.8312,
  "gx":  0.0012,
  "gy": -0.0034,
  "gz":  0.0007,
  "wrms": 9.8123,
  "flags": 8
}
```

---

## Status Flags

Defined in `firmware/include/types.h`. All layers must use these exact bit positions.

| Constant | Hex | Bit | Meaning |
|----------|-----|-----|---------|
| `STATUS_OK` | `0x00` | — | Normal sample, no issues |
| `STATUS_ACCEL_CLIPPED` | `0x01` | 0 | One or more accel axes hit the ±8g hardware range limit |
| `STATUS_GYRO_CLIPPED` | `0x02` | 1 | One or more gyro axes hit the ±500 deg/s range limit |
| `STATUS_INTERLOCK_OPEN` | `0x04` | 2 | Safety interlock was open at sample time (E-Stop condition) |
| `STATUS_ANOMALY` | `0x08` | 3 | filterTask RMS exceeded anomaly threshold |
| `STATUS_SENSOR_FAULT` | `0x10` | 4 | I2C dropout — WHO_AM_I probe failed (legacy; use 0x20/0x40) |
| `STATUS_DEGRADED_REBOOT_REQUIRED` | `0x20` | 5 | I2C fault; auto-reboot pending (reboots remaining) |
| `STATUS_SENSOR_UNAVAILABLE` | `0x40` | 6 | I2C fault; max reboots exhausted — power-cycle required |

Multiple flags can be set simultaneously (bitmask).

Records with flags `0x10`, `0x20`, or `0x40` are routed by the bridge to the
`sensor_faults` InfluxDB measurement instead of `vibration`. The `ax` field in
these records encodes the raw WHO_AM_I register byte (`0xFF` = bus not responding).

---

## E-Stop Event Message

Published once when the safety ISR fires, in addition to setting `STATUS_INTERLOCK_OPEN`
in the ongoing telemetry stream.

**MQTT topic:** `sensor/<node_id>/estop`
**QoS:** 0

```json
{
  "ts": 1710000000456,
  "triggered": 1,
  "reason": "optical_interlock"
}
```

E-Stop events use `ts`, matching telemetry payload timestamps.

---

## InfluxDB Schema

**Measurement:** `vibration`
**Tag:** `node_id`

### Stored fields (written by `mqtt_to_influx.py`)

| InfluxDB field | Source | Notes |
|----------------|--------|-------|
| `ax` | payload | m/s² |
| `ay` | payload | m/s² |
| `az` | payload | m/s² |
| `gx` | payload | rad/s |
| `gy` | payload | rad/s |
| `gz` | payload | rad/s |
| `flags` | payload | integer bitmask |
| `boot_id` | payload `boot` | renamed for clarity |
| `sequence_id` | payload `seq` | renamed for clarity |
| `vibration_rms` | **derived** | `sqrt(ax² + ay² + az²)` in m/s², computed by bridge |
| `window_rms` | payload `wrms` | firmware rolling RMS in m/s², anomaly metric |

`window_rms` is the operational anomaly signal. `vibration_rms` is retained as a
derived accel vector magnitude for compatibility with older dashboards and data.

### Fault records (`sensor_faults` measurement)

Records with fault flags (`0x10 | 0x20 | 0x40`) are routed here instead of `vibration`.

| InfluxDB field | Source | Notes |
|----------------|--------|-------|
| `boot_id` | payload `boot` | |
| `sequence_id` | payload `seq` | |
| `flags` | payload | original bitmask — distinguishes 0x10/0x20/0x40 |
| `whoami_raw` | payload `ax` | WHO_AM_I register byte encoded by firmware (0xFF = bus not responding) |
| `reason` | bridge | always `"i2c_dropout"` |

### Record time

Set from `payload["ts"]` with `WritePrecision.MS`. Falls back to broker-arrival
time if `ts` is absent.

---

## Layer Compliance

| Layer | File | Must implement |
|-------|------|----------------|
| Firmware | `firmware/src/main.cpp` — `buildPayload()` | Emit all fields per this spec |
| Mock | `gateway/bridge/mock_esp32.py` | Match firmware payload shape exactly |
| Bridge | `gateway/bridge/mqtt_to_influx.py` | Parse all fields; compute `vibration_rms`; store `window_rms` |
| MCP server | `mcp-server/src/index.ts` | Use correct flag bit masks; report units as m/s² |
| Dashboard hook | `dashboard/hooks/useMqttTelemetry.ts` | Match type shape; display firmware `wrms` with vector fallback |

---

## Versioning

This schema is currently **unversioned** (no `schema_version` field in payloads).

Rules until a versioning mechanism is added:
- All field changes are **breaking** — update every layer atomically.
- Do not add optional fields without updating this document first.
- The firmware struct (`TelemetryRecord` in `types.h`) is the ground truth for field names, types, and units. This document must reflect it exactly.
- When hardware arrives and calibration changes the expected value ranges, update the "Notes" column here rather than the field definitions.
