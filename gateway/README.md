# Gateway

Raspberry Pi 5 gateway stack for the industrial sensor pipeline. Runs as a set
of Docker services plus a Python bridge process.

## Services

| Service | Port | Purpose |
|---------|------|---------|
| Mosquitto | 1883 | MQTT broker — firmware, bridge, mock script |
| Mosquitto WS | 9001 | MQTT over WebSocket — Next.js dashboard |
| InfluxDB 2.7 | 8086 | Time-series storage + UI |
| Grafana | 3001 | Operational dashboards |

## Setup

```bash
cd gateway
docker compose up -d
```

InfluxDB is auto-initialised on first boot (org: `industrial`, bucket: `sensors`,
token: `dev-token-change-in-production`). Data persists in named Docker volumes
across restarts. Use `docker compose down -v` only when a clean slate is needed.

Grafana provisions its InfluxDB datasource and dashboard JSON from
`gateway/grafana/` when the container starts. After pulling dashboard changes,
recreate Grafana with the current Compose config:

```bash
cd gateway
docker compose up -d grafana
```

## Bridge

The Python bridge subscribes to MQTT and writes records to InfluxDB.

```bash
cd gateway/bridge
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 mqtt_to_influx.py
```

### Topic routing

| Topic | InfluxDB measurement |
|-------|----------------------|
| `sensor/+/telemetry` without `0x10/0x20/0x40` | `vibration` |
| `sensor/+/telemetry` with `0x10/0x20/0x40` | `sensor_faults` |
| `sensor/+/estop` | `estop_event` |

Fault records (`STATUS_SENSOR_FAULT`, `STATUS_DEGRADED_REBOOT_REQUIRED`,
`STATUS_SENSOR_UNAVAILABLE`) are routed to `sensor_faults` to keep them
out of the normal vibration stream.

`window_rms` is parsed from firmware `wrms` and is the metric used for anomaly
detection. `vibration_rms` is still derived as `sqrt(ax² + ay² + az²)` for
legacy panels and compatibility.

### Timestamps

The bridge uses the firmware `ts` field (UTC epoch ms, NTP-anchored) when
`ts > 1_000_000_000_000`. Before NTP sync (first ~2s after boot), it falls
back to broker-arrival time.

### Observability

Bridge logs are intentionally quiet at steady state — telemetry writes use
`DEBUG` (silent by default at 2 records/second). E-Stop events log at `WARNING`.

To verify InfluxDB is receiving data without enabling debug logging:

```bash
bash gateway/bridge/query_influx.sh
```

Non-empty CSV output confirms the pipeline is writing correctly.

## Mock ESP32

For local development without hardware:

```bash
cd gateway/bridge && source .venv/bin/activate
python3 mock_esp32.py
```

Publishes synthetic telemetry in a loop: `NORMAL → ANOMALY → ESTOP → NORMAL`.
Produces the same JSON payload shape as the real firmware.

## Integrity Check

Validates end-to-end data integrity from firmware to InfluxDB:

```bash
cd gateway/bridge && source .venv/bin/activate
python3 integrity_check.py --minutes 10
```

Checks:
1. **Sequence integrity** — `seq` is strictly increasing with no gaps or duplicates per `boot_id`
2. **Timestamp monotonicity** — timestamps are monotonic and real-time aligned after NTP sync
3. **Data fidelity** — values and flags are unchanged end-to-end
4. **Fault classification** — fault records are correctly routed to `sensor_faults`

## Credentials (dev only)

| Service | Credential |
|---------|-----------|
| InfluxDB token | `dev-token-change-in-production` |
| Grafana | `admin` / `admin` |
| Mosquitto | anonymous (no auth) |

**These are not suitable for production.** For a hardened deployment: add
Mosquitto `password_file`, rotate the InfluxDB token, and inject secrets via
environment variables.
