# Project Status — v1

_Last updated: Phase 1 complete, Phase 2 in progress (BufferManager + types done)._

---

## What We Have

### Repository Structure

```
industrial-sensor-pipeline/
├── firmware/                           # ESP32-S3 PlatformIO project
│   ├── src/main.cpp                    # Boot sequence (PSRAM, MPU-6050) — FreeRTOS tasks still stubs
│   ├── include/
│   │   ├── config.h                    # Pin assignments, sample rate, buffer capacity, MQTT port
│   │   └── types.h                     # TelemetryRecord (44 bytes, 6-axis IMU + flags), BufferStats
│   ├── lib/
│   │   └── BufferManager/
│   │       ├── BufferManager.h         # PSRAM ring buffer — mutex-protected, overflow eviction
│   │       └── BufferManager.cpp
│   └── platformio.ini                  # 16MB flash, OPI PSRAM (qio_opi), USB CDC, lib_deps
├── gateway/
│   ├── docker-compose.yml              # Mosquitto 2, InfluxDB 2.7, Grafana — running
│   ├── config/mosquitto.conf           # TCP :1883 + WebSocket :9001 listeners
│   └── bridge/
│       ├── mock_esp32.py               # Simulated sensor node (NORMAL → ANOMALY → ESTOP loop)
│       ├── mqtt_to_influx.py           # MQTT subscriber → InfluxDB writer
│       └── requirements.txt
├── dashboard/
│   ├── app/page.tsx                    # Root page, renders TelemetryDisplay for node01
│   ├── components/
│   │   ├── TelemetryDisplay.tsx        # Live RMS, flags, status badge
│   │   └── HeartbeatIndicator.tsx      # Connection heartbeat
│   ├── hooks/useMqttTelemetry.ts       # MQTT WebSocket hook, 100-record rolling history
│   └── package.json
├── mcp-server/
│   ├── src/index.ts                    # MCP server (stdio), three tools backed by Flux queries
│   ├── dist/index.js                   # Built and ready
│   └── package.json
├── .mcp.json                           # Claude Code MCP config — points to dist/index.js
└── docs/
    ├── project-context.md              # Source-of-truth architecture doc
    └── claude-notes/
        ├── platformio-init.md
        ├── gateway-stack.md
        ├── dashboard-architecture.md
        ├── mcp-server-architecture.md
        ├── implementation-schedule.md
        └── status/
            └── project-status-v1.md   ← this file
```

---

## Completion by Phase

### Phase 1 — Environment & Simulation ✅ Complete

| Component | File(s) | Notes |
|-----------|---------|-------|
| Git repo + structure | — | Initial commit |
| Docker gateway stack | `gateway/docker-compose.yml` | Mosquitto, InfluxDB v2, Grafana — containers up |
| Mosquitto config | `gateway/config/mosquitto.conf` | TCP + WebSocket listeners |
| MQTT → InfluxDB bridge | `gateway/bridge/mqtt_to_influx.py` | Handles `telemetry` and `estop` topics |
| Mock ESP32 publisher | `gateway/bridge/mock_esp32.py` | State machine with realistic synthetic signal |
| Next.js dashboard | `dashboard/` | Live MQTT WebSocket display, rolling history |
| MCP server | `mcp-server/src/index.ts` | Built; `get_latest_telemetry`, `get_sensor_health`, `get_recent_anomalies` |
| `.mcp.json` | `.mcp.json` | Claude Code auto-discovers MCP server at repo root |

**Remaining blockers before Phase 1 can be smoke-tested end-to-end:**
- Python deps not installed: `cd gateway/bridge && pip3 install paho-mqtt influxdb-client`
- Dashboard deps not installed: `cd dashboard && npm install`

### Phase 2 — Firmware Logic 🔄 In Progress

| Task | Status | Notes |
|------|--------|-------|
| `include/types.h` — `TelemetryRecord` + `BufferStats` | ✅ Done | 44-byte record: 6-axis IMU, boot_id, sequence_id, status_flags |
| `lib/BufferManager` — PSRAM ring buffer | ✅ Done | Mutex-protected; push/pop/peek/getStats; overflow evicts oldest |
| `lib/KalmanFilter` — 1D scalar filter | ❌ Not started | |
| FreeRTOS task skeletons (all four tasks) | ❌ Not started | Stubs in `main.cpp` |
| Safety ISR (`IRAM_ATTR`) + event group | ❌ Not started | |
| MQTT connection manager + reconnect backoff | ❌ Not started | |
| `NORMAL → BUFFERING → SYNCING` state machine | ❌ Not started | |
| MPU-6050 calibration offsets in NVS | ❌ Not started | Low priority until hardware arrives |

### Phase 3 — Hardware Integration ⏳ Blocked

Hardware not yet arrived. Can begin once ESP32-S3 N16R8 is in hand.

### Phases 4 & 5 — HIL Testing, Pi Deployment, Docs ❌ Not Started

---

## Next Steps (Priority Order)

### 1. `firmware/lib/KalmanFilter` ← do this next

A prerequisite for `filterTask`. One instance per IMU axis (6 total).

- Class with configurable `Q` (process noise) and `R` (measurement noise)
- Single `update(float measurement) → float` method
- Stateless between instances — no global state
- Starting defaults: Q = 0.01, R = 0.1 (tune against real hardware later)

### 2. FreeRTOS Tasks in `main.cpp`

Uncomment and implement the four tasks once KalmanFilter exists:

| Task | Core | Priority | Responsibility |
|------|------|----------|----------------|
| `sensorTask` | 1 | 5 | Sample MPU-6050 @ 100 Hz via `xQueueSend` |
| `filterTask` | 1 | 5 | Dequeue raw samples, apply Kalman per axis, compute RMS/peak, push to `BufferManager` |
| `telemetryTask` | 0 | 3 | Pop from buffer (NORMAL state) and publish via MQTT QoS 1 |
| `syncTask` | 0 | 3 | On reconnect: burst-flush buffer in rate-limited batches |

Inter-task communication: a single `QueueHandle_t` between `sensorTask` → `filterTask`. `BufferManager` shared between `filterTask` (producer) and `telemetryTask`/`syncTask` (consumers).

### 3. Safety ISR + Event Group

- `IRAM_ATTR` ISR on `PIN_SAFETY_INTERLOCK` (GPIO 10), falling edge
- Sets a bit in a `FreeRTOS EventGroup`; a lightweight safety task blocks on it
- On trigger: set `STATUS_INTERLOCK_OPEN` flag, publish to `sensor/<node>/estop`, halt `telemetryTask`

### 4. MQTT Manager + State Machine

- WiFi connection with credential loading from NVS (not hardcoded)
- PubSubClient reconnect loop with exponential backoff
- State machine: `NORMAL` → `BUFFERING` (on disconnect) → `SYNCING` (on reconnect) → `NORMAL`
- `SYNCING` rate-limits historical burst to avoid overwhelming the broker

### 5. End-to-End Validation

- Run full simulation stack (docker, bridge, mock script)
- Flash firmware, confirm telemetry reaches InfluxDB
- Unplug network, verify PSRAM buffering, reconnect, verify burst sync
- Ask Claude Code "Is node01 healthy?" to validate MCP path

---

## Key Design Decisions Recorded

| Decision | Choice | File |
|----------|--------|------|
| PSRAM mode | `qio_opi` (OPI, not QSPI) | `platformio-init.md` |
| TelemetryRecord layout | 44 bytes, 6-axis IMU + boot_id/sequence_id | `types.h` |
| Buffer overflow policy | Evict oldest (keep newest data) | `BufferManager.h` |
| Buffer thread-safety | FreeRTOS mutex; NOT ISR-safe by design | `BufferManager.h` |
| MCP transport | stdio (laptop); swap to SSE for Pi remote access | `mcp-server-architecture.md` |

---

## Port Reference

| Service | Host Port |
|---------|-----------|
| Mosquitto MQTT | 1883 |
| Mosquitto WebSocket | 9001 |
| InfluxDB | 8086 |
| Grafana | 3001 |
| Next.js | 3000 |
| MCP Server | 3002 (Pi deployment only) |
