# Project Status — v1

_Last updated: Phase 2 in progress — MQTT + state machine implemented. Ready for intermediate integration checkpoint._

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
| `lib/KalmanFilter` — 1D scalar filter | ✅ Done | Q=0.01, R=0.1 defaults; spike rejection; NaN/Inf guard; 7/7 validation tests pass |
| FreeRTOS tasks — `sensorTask` + `filterTask` | ✅ Done | `sensorTask` @ 100Hz via `vTaskDelayUntil`; `filterTask` with 6 Kalman instances, rolling RMS window, anomaly detection, pushes to BufferManager |
| FreeRTOS tasks — `telemetryTask` + `syncTask` | ✅ Done | peek/enqueue/pop pattern; state-gated; syncTask drains on kBitReconnect EventGroup |
| MQTT connection manager (`lib/MqttManager`) | ✅ Done | WiFi + PubSubClient; exponential backoff 1s→60s; onConnect/onDisconnect callbacks |
| `NORMAL → BUFFERING → SYNCING` state machine | ✅ Done | `NodeState` enum in types.h; `std::atomic<NodeState>`; transitions in MQTT callbacks |
| `connectionTask` — MQTT socket owner | ✅ Done | Sole caller of MqttManager::loop() and publish(); drains g_publishQueue each tick |
| Safety ISR (`IRAM_ATTR`) + event group | ❌ Not started | GPIO10, falling edge; posts to FreeRTOS EventGroup |
| MPU-6050 calibration offsets in NVS | ❌ Not started | Low priority until hardware arrives |
| NVS boot_id persistence | ❌ Not started | Currently hardcoded to 1 in `main.cpp` |

### Phase 3 — Hardware Integration ⏳ Blocked

Hardware not yet arrived. Can begin once ESP32-S3 N16R8 is in hand.

### Phases 4 & 5 — HIL Testing, Pi Deployment, Docs ❌ Not Started

---

## Next Steps (Priority Order)

### Step 1 — MQTT + State Machine ✅ Done

These belong together: the connection manager emits events; the state machine
consumes them immediately. Implementing them separately would require wiring
them together anyway.

**MQTT connection manager:**
- WiFi init with credentials from NVS (not hardcoded)
- PubSubClient connect + exponential reconnect backoff
- On disconnect callback: post event to state machine
- On reconnect callback: post event to state machine + set FreeRTOS EventGroup bit
- MQTT topics: `sensor/<node>/telemetry` (QoS 1), `sensor/<node>/estop`

**State machine (keyed from a single enum, not scattered conditionals):**

| State | `telemetryTask` | `syncTask` |
|-------|----------------|-----------|
| `NORMAL` | Pop buffer, publish live | Idle |
| `BUFFERING` | Skip pop — records accumulate in PSRAM | Idle |
| `SYNCING` | Resume live publish | Burst-flush at `SYNC_BATCH_SIZE` / `SYNC_BATCH_DELAY_MS` |

Transition rules:
- `NORMAL → BUFFERING` on MQTT disconnect
- `BUFFERING → SYNCING` on MQTT reconnect
- `SYNCING → NORMAL` when `g_buffer.isEmpty()`

### Step 2 — Intermediate Integration Checkpoint ← do this next

Before adding more firmware features, prove the core loop works end-to-end:

1. Cold boot → one filtered sample publishes live → appears in InfluxDB
2. Pull network → records accumulate in PSRAM buffer
3. Restore network → controlled drain, no broker flood
4. Ask Claude Code "Is node01 healthy?" → MCP path returns real data

If this works, everything else is mechanical. If it doesn't, find out here.

### Step 3 — Safety ISR + Event Group

Independent of MQTT; can be developed in parallel but validated after Step 2.

- `IRAM_ATTR` ISR on `PIN_SAFETY_INTERLOCK` (GPIO 10), falling edge
- Sets a bit in a FreeRTOS EventGroup
- Safety consumer task: blocks on EventGroup bit, sets `STATUS_INTERLOCK_OPEN`,
  publishes to `sensor/<node>/estop`, halts `telemetryTask`
- Must survive: safety interrupt during normal run, during SYNCING, during reboot mid-sync

### Step 4 — NVS boot_id

- Load persistent boot counter from NVS on startup
- Increment and write back each boot
- Include in all telemetry and heartbeat payloads (currently hardcoded to `1`)

### Step 5 — Full Smoke Test

Validate the complete failure matrix:

| Scenario | Expected outcome |
|----------|-----------------|
| Cold boot | Connects, publishes within 5s |
| Disconnect during run | Transitions to BUFFERING, no data loss |
| Prolonged buffering | PSRAM fills gracefully, oldest records evicted |
| Reconnect | Controlled drain, transitions back to NORMAL |
| Safety interrupt during SYNCING | E-Stop published immediately, sync resumes/halts correctly |
| Reboot mid-sync | `boot_id` increments; `sequence_id` resets; no duplicate confusion |

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
