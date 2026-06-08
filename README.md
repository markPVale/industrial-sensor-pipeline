# Industrial Sensor Pipeline

An end-to-end industrial vibration monitoring system built on an ESP32-S3 microcontroller, Raspberry Pi gateway, and InfluxDB time-series database. Real IMU data flows from hardware to a live Grafana dashboard and is queryable by Claude Code via an MCP server.

## Architecture

See [`docs/architecture.md`](docs/architecture.md) for full system, store-and-forward state machine, and I2C fault escalation diagrams.

```
ESP32-S3 N16R8
  └── MPU-6050 (I2C, SDA=8, SCL=9)       100Hz sampling, 6-axis IMU
  └── Photoresistor (GPIO 10)              Safety interlock ISR
        │
        │ MQTT over WiFi (QoS 0)
        ▼
Mosquitto Broker (Docker, :1883 / :9001 WS)
        │
        ▼
Python Bridge (paho-mqtt → influxdb-client)
        │
        ▼
InfluxDB 2.7 (Docker, :8086)  ──▶  Grafana (:3001)
                               ──▶  Next.js Dashboard (:3000)
                               ──▶  MCP Server (:3002)
```

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3-DevKitC-1 N16R8 (16MB flash, 8MB OPI PSRAM) |
| IMU | MPU-6050 at I2C address 0x68 |
| Safety interlock | Photoresistor on GPIO 10, falling-edge ISR |
| Gateway | Raspberry Pi (any model with WiFi/Ethernet) |

## Repository Structure

```
industrial-sensor-pipeline/
├── firmware/                   # ESP32-S3 PlatformIO project
│   ├── src/main.cpp            # FreeRTOS tasks, state machine, MQTT
│   ├── include/
│   │   ├── config.h            # Pin assignments, sample rate, buffer capacity
│   │   └── types.h             # TelemetryRecord, NodeState, status flags
│   └── lib/
│       ├── BufferManager/      # PSRAM ring buffer (50,000 records, mutex-protected)
│       ├── KalmanFilter/       # 1D scalar Kalman filter (6 instances per node)
│       └── MqttManager/        # WiFi + PubSubClient, exponential reconnect backoff
├── gateway/
│   ├── docker-compose.yml      # Mosquitto, InfluxDB 2.7, Grafana
│   ├── config/mosquitto.conf
│   └── bridge/
│       ├── mqtt_to_influx.py   # MQTT → InfluxDB bridge
│       ├── integrity_check.py  # End-to-end data integrity validator
│       └── mock_esp32.py       # Simulated sensor node for local dev
├── dashboard/                  # Next.js live dashboard (MQTT WebSocket)
├── mcp-server/                 # MCP server — exposes sensor data to Claude Code
└── docs/
    ├── telemetry-schema.md
    └── claude-notes/           # Architecture notes, status, state inventory
```

## Firmware Design

The firmware runs six FreeRTOS tasks across two cores:

| Task | Core | Priority | Role |
|------|------|----------|------|
| `safetyTask` | 0 | 6 | ISR handler — latches interlock, publishes estop |
| `sensorTask` | 1 | 5 | 100Hz MPU-6050 sampling via `vTaskDelayUntil` |
| `filterTask` | 1 | 5 | 6× Kalman filters, rolling RMS, anomaly detection |
| `connectionTask` | 0 | 4 | Sole MQTT socket owner, drains publish queue |
| `telemetryTask` | 0 | 3 | Enqueues oldest buffered telemetry at 2Hz |
| `syncTask` | 0 | 3 | Drains PSRAM buffer on reconnect |

**Store-and-forward:** Records accumulate in PSRAM (up to 50,000 × 48 bytes ≈ 2.4MB) during WiFi outages. The current shortcut commits buffered records after `connectionTask` successfully calls `publish()`, not when records are merely enqueued. MQTT still uses QoS 0 style publish behavior, so this is improved local correctness but not end-to-end guaranteed delivery. See [`docs/store-and-forward-status.md`](docs/store-and-forward-status.md).

**State machine:** `NodeState` (NORMAL → BUFFERING → SYNCING) is a single `std::atomic<NodeState>` — no scattered boolean flags.

**I2C fault recovery:** `sensorTask` probes the MPU-6050 via WHO_AM_I on every sample. After 5 consecutive failures it enters a fault state: periodic 9-pulse SCL bus recovery attempts every 3s, escalating to a full software reboot after 30s. Auto-reboots are capped at 3 per power-on session (tracked in NVS); after the third reboot the node transitions to `STATUS_SENSOR_UNAVAILABLE` and holds that state until SDA is reconnected or the node is power-cycled.

## Getting Started

### Gateway (Raspberry Pi)

```bash
# Start the Docker stack
cd gateway && docker compose up -d

# Start the MQTT → InfluxDB bridge
cd gateway/bridge
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 mqtt_to_influx.py
```

### Firmware (ESP32-S3)

```bash
# Copy secrets template and fill in WiFi credentials + broker IP
cp firmware/secrets.ini.template firmware/secrets.ini

# Flash via PlatformIO
cd firmware && pio run --target upload
```

### MCP Server (Pi, network mode)

```bash
cd mcp-server && npm install && npm run build
TRANSPORT=sse MCP_PORT=3002 INFLUX_URL=http://localhost:8086 \
  INFLUX_TOKEN=dev-token-change-in-production \
  INFLUX_ORG=industrial INFLUX_BUCKET=sensors \
  nohup node dist/index.js > ~/mcp-server.log 2>&1 &
```

Verify: `curl -s http://sensor-gateway.local:3002/sse` — should return an SSE endpoint event.

### Integrity Check

```bash
cd gateway/bridge && source .venv/bin/activate
python3 integrity_check.py --minutes 10
```

## Telemetry Schema

Each record published to `sensor/<node_id>/telemetry`:

```json
{
  "boot": 12,
  "seq": 4821,
  "ts": 1714000000000,
  "ax": -0.88, "ay": 0.17, "az": 10.19,
  "gx": -0.07, "gy": -0.15, "gz": -0.05,
  "flags": 0
}
```

`wrms` is computed in firmware over the 50-sample filter window and written to InfluxDB as `window_rms`. `vibration_rms` remains a bridge-derived accel vector magnitude for compatibility. See `docs/telemetry-schema.md` for flag bit definitions.

## Status Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `STATUS_OK` | `0x00` | Normal sample |
| `STATUS_ACCEL_CLIPPED` | `0x01` | Accelerometer hit ±8g range limit |
| `STATUS_GYRO_CLIPPED` | `0x02` | Gyro hit ±500°/s range limit |
| `STATUS_INTERLOCK_OPEN` | `0x04` | Safety interlock was open at sample time |
| `STATUS_ANOMALY` | `0x08` | Vibration RMS exceeded threshold |
| `STATUS_SENSOR_FAULT` | `0x10` | I2C dropout — WHO_AM_I probe failed (legacy) |
| `STATUS_DEGRADED_REBOOT_REQUIRED` | `0x20` | I2C fault; auto-reboot pending (reboots remaining) |
| `STATUS_SENSOR_UNAVAILABLE` | `0x40` | I2C fault; max reboots exhausted, power-cycle required |

## Port Reference

| Service | Port |
|---------|------|
| Mosquitto MQTT | 1883 |
| Mosquitto WebSocket | 9001 |
| InfluxDB | 8086 |
| Grafana | 3001 |
| Next.js dashboard | 3000 |
| MCP server (Pi) | 3002 |
