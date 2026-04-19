# Implementation Schedule — Updated

_Last updated to reflect actual project state as of Phase 1 completion._

---

## Structural Changes vs. Original Plan

The original four-directory mono-repo has expanded to five top-level modules:

```
industrial-sensor-pipeline/
├── firmware/       # ESP32-S3 PlatformIO project
├── gateway/        # Docker stack + Python bridge + mock script
├── dashboard/      # Next.js digital twin
├── mcp-server/     # Node.js MCP server (AI integration — NEW)
└── docs/
    └── claude-notes/   # Per-decision architecture notes
```

The **MCP Server** was added to the architecture after the initial plan. It runs alongside the gateway and exposes InfluxDB sensor data to LLM clients (Claude Code) via the Model Context Protocol.

---

## Phase 1: Environment & Simulation

| Task | Status | Notes |
|------|--------|-------|
| Initialize Git repository and project structure | ✅ Done | Initial commit |
| `docker-compose.yml` — Mosquitto, InfluxDB v2, Grafana | ✅ Done | `gateway/docker-compose.yml` |
| `gateway/config/mosquitto.conf` | ✅ Done | Required by docker-compose volume mount |
| Python MQTT → InfluxDB bridge (`mqtt_to_influx.py`) | ✅ Done | Subscribes `sensor/+/telemetry` and `sensor/+/estop` |
| Python Mock ESP32 (`mock_esp32.py`) | ✅ Done | State machine: NORMAL → ANOMALY → ESTOP → NORMAL |
| Next.js Dashboard boilerplate — WebSocket MQTT listener | ✅ Done | `useMqttTelemetry` hook + `TelemetryDisplay` + `HeartbeatIndicator` |
| MCP Server — `get_latest_telemetry`, `get_sensor_health`, `get_recent_anomalies` | ✅ Done | `mcp-server/src/index.ts` (stdio transport, InfluxDB Flux queries) |

**Phase 1 is complete.** The full simulation stack can be run today without hardware.

---

## Phase 2: Firmware Logic & Store-and-Forward

| Task | Status | Notes |
|------|--------|-------|
| FreeRTOS task skeletons (`sensorTask`, `filterTask`, `telemetryTask`, `syncTask`) | ❌ Not started | Stubs commented in `firmware/src/main.cpp` |
| `lib/BufferManager` — circular buffer over `ps_malloc()` | ❌ Not started | `PSRAM_BUFFER_CAPACITY = 50,000` records defined in `config.h` |
| `lib/KalmanFilter` — 1D Kalman for vibration axis | ❌ Not started | |
| Safety ISR (`IRAM_ATTR`) + FreeRTOS event group for E-Stop | ❌ Not started | `PIN_SAFETY_INTERLOCK = GPIO10` placeholder set |
| MQTT connection manager with reconnect backoff | ❌ Not started | |
| `NORMAL → BUFFERING → SYNCING` state machine | ❌ Not started | Schema defined; mirrors mock script's simulation |
| MPU-6050 calibration offsets stored in NVS | ❌ Not started | |
| Validate sync logic against `mock_esp32.py` as receiver | ❌ Not started | |

**Recommended order:** BufferManager → KalmanFilter → FreeRTOS tasks → Safety ISR → State machine → MQTT manager.

---

## Phase 3: Hardware Integration _(blocked — hardware not yet arrived)_

| Task | Status |
|------|--------|
| Wire MPU-6050 and Photoresistor to ESP32-S3 | ⏳ Blocked |
| Calibrate sensor thresholds (safe vs. anomaly RMS) | ⏳ Blocked |
| Replace mock data with real sensor readings | ⏳ Blocked |
| Hardware interrupt for optical interlock | ⏳ Blocked |

Hardware verification checklist: see `docs/claude-notes/platformio-init.md`.

---

## Phase 4: Pi Deployment & Stress-Testing

| # | Task | Status |
|---|------|--------|
| 1 | Deploy gateway stack to Raspberry Pi 5 | ❌ Not started |
| 2 | NTP sync — Pi + firmware time alignment | ❌ Not started |
| 3 | End-to-End Integrity Check | ❌ Not started |
| 4 | Grafana dashboard validation (real timestamps) | ❌ Not started |
| 5 | MCP Server deployed to Pi, Claude Code connected over network | ❌ Not started |
| 6 | Record "Unplug Demo": disconnect → buffer → reconnect → burst-sync | ❌ Not started |
| 7 | Chaos Injection Panel (stretch) | ❌ Not started |

### Step 3 — End-to-End Integrity Check

Acceptance gate before Grafana validation. Proves the pipeline preserves ordering, time, and meaning from firmware → DB.

**Sequence integrity:** `seq_id` is strictly increasing with no gaps or duplicates. Resets to 0 only when `boot_id` increments.

**Timestamp correctness:** timestamps are monotonic and real-time aligned after NTP sync. No drift, no backward jumps.

**Data fidelity:** values and flags are unchanged end-to-end. Records sent == records stored, including anomaly-flagged records.

---

For Pi deployment: `docker compose up -d` on the Pi uses the same `docker-compose.yml`. MCP server transport swap: `StdioServerTransport` → `SSEServerTransport` — see `docs/claude-notes/mcp-server-architecture.md`.

---

## Phase 5: Documentation & Portfolio

| Task | Status |
|------|--------|
| High-quality READMEs for each sub-directory | ❌ Not started (firmware has one) |
| System diagram (Mermaid.js or Excalidraw) | ❌ Not started |
| Project summary for LinkedIn/Resume | ❌ Not started |

---

## Immediate Next Action

**Start Phase 2 — `lib/BufferManager`.**

All simulation infrastructure is in place. The mock script produces the correct `TelemetryRecord`-shaped payloads; the bridge persists them; the dashboard displays them. The firmware is the only layer that doesn't yet have working logic.
