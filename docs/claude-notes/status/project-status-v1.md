# Project Status — v1

_Last updated: Full pipeline verified end-to-end on real hardware. ESP32 → MQTT → Bridge → InfluxDB confirmed. Known open issues: USB CDC silence under WiFi, firmware timestamps are uptime not epoch._

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

### Phase 2 — Firmware Logic ✅ Complete (except MPU-6050 calibration — hardware-blocked)

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
| Safety ISR (`IRAM_ATTR`) + event group | ✅ Done | GPIO10, falling edge; `safetyISR` → `g_safetyEvents` → `safetyTask` (priority 6); latching interlock; stamps `STATUS_INTERLOCK_OPEN` on all records post-trip |
| MPU-6050 calibration offsets in NVS | ❌ Not started | Low priority until hardware arrives — TODO at `main.cpp:621` |
| NVS boot_id persistence | ✅ Done | `initBootId()` in `main.cpp`; reads/increments/writes NVS key "boot_id" on every boot |

### Phase 3 — Hardware Integration ▶ In Progress

Hardware arrived. ESP32-S3 DevKitC-1 in hand. Step 5 smoke test underway.

**Bring-up phases:**

| Phase | Description | Status |
|-------|-------------|--------|
| A | Controller bring-up (serial output, USB CDC) | ✅ Done |
| B | I2C detection — MPU-6050 found at 0x68 | ✅ Done |
| C | Raw sensor validation — calibration loads, telemetry pipeline running | ✅ Done |
| D | NVS / boot_id persistence — monotonic counter verified across reboots | ✅ Done |
| E | Wi-Fi validation — connect to real AP; MQTT broker reachable | ✅ Done |
| F | Full integration — real IMU data flowing to broker at 2Hz; state machine, buffer, anomaly detection all confirmed | ✅ Done |

**Notes:**
- ESP32-S3 native USB CDC required `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` build flags to route `Serial` to USB port
- I2C on GPIO 8 (SDA) / GPIO 9 (SCL); MPU-6050 AD0 pulled low → address 0x68
- WiFi credentials injected via gitignored `secrets.ini` — copy `secrets.ini.template` and fill in values
- Serial `Serial` / `Serial.flush()` inside MQTT callbacks causes a crash on ESP32-S3 (USB CDC + WiFi interrupt contention). Rule: callbacks must only call `xEventGroupSetBits` — no Serial, no setState, no blocking calls. State transitions and logging are handled in `connectionTask`.
- USB CDC goes silent once WiFi is active (WiFi starves USB CDC DMA). Device is running; use `mosquitto_sub` for observability. See next steps for fix options.

**Phase C sketch** (`esp32-bringup/src/main.cpp`):
- No library dependencies — raw Wire calls only; single-file, single-loop
- `initMPU()` — clears sleep bit in `PWR_MGMT_1`; returns false if I2C ack fails; `setup()` halts with error message on failure
- `readAccel()` — reads 6 bytes from `ACCEL_XOUT_H`; reconstructs three signed 16-bit values; converts to m/s² using default ±2g scale factor (÷ 16384 × 9.81)
- `loop()` — prints ax/ay/az every 200ms; prints error line on read failure
- Gravity test guide printed to serial at boot: lay flat first to confirm which axis reads ~+9.81, then use that to anchor tilt and flip tests

### Phases 4 & 5 — HIL Testing, Pi Deployment, Docs ❌ Not Started

---

## Next Steps (Priority Order)

### Step 0 — MQTT + State Machine ✅ Done

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

### Step 1 — Integration Checkpoint (simulation only) ✅ Done

Verified full path: mock → broker → bridge → InfluxDB → MCP → dashboard.
Schema, topics, timestamps, flag bits, and health queries all confirmed correct.

### Step 2 — Buffered Replay / Reconnect Test ✅ Done

Simulated node outage (killed mock), confirmed OFFLINE detection after 30s,
restarted mock, confirmed immediate ONLINE recovery via MCP health query.
Note: full PSRAM buffer drain test requires real hardware (Step 5).

Simulate outage, queue buildup, reconnect burst, and verify latest-state
correctness under drain. Kill the mock mid-run, confirm bridge handles
reconnect cleanly, check InfluxDB has no gaps or duplicates.

### Step 3 — Safety ISR ✅ Done

- `IRAM_ATTR safetyISR` on GPIO 10, falling edge → `xEventGroupSetBitsFromISR`
- `safetyTask` priority 6 (highest): latches `g_interlockActive`, enqueues estop publish
- `filterTask` stamps `STATUS_INTERLOCK_OPEN` on all records while latch is set
- Interlock latches until reboot — intentional; `FALLING` vs `RISING` to be verified on hardware

### Step 4 — NVS boot_id ✅ Done

- Read/increment/write boot counter from NVS on startup
- Replaced hardcoded `kBootId = 1` — see `initBootId()` in `main.cpp`
- Verify sequence semantics across reboot (seq resets, boot increments) — needs real hardware

### Step 5 — Hardware Smoke Test ▶ In Progress

| Check | What to validate | Status |
|-------|-----------------|--------|
| Controller bring-up | Serial output, USB CDC | ✅ Done |
| Physical sensor | MPU-6050 detected at 0x68 | ✅ Done |
| Raw sensor validation | Calibration loads from NVS defaults; telemetry pipeline running | ✅ Done |
| NVS boot_id | Monotonic counter increments correctly across reboots | ✅ Done |
| Wi-Fi + MQTT | Connect to real AP; broker reachable | ✅ Done |
| Full integration | Real IMU data at 2Hz; state machine, anomaly detection confirmed via `mosquitto_sub` | ✅ Done |
| Bridge → InfluxDB | Data landing in `sensors` bucket; confirmed via bridge DEBUG logs | ✅ Done |
| ISR trigger | Photoresistor interrupt fires correctly; flags 8→12 on trigger, latch holds, clears on reboot | ✅ Done |
| Timing/jitter | 100 Hz sample rate holds under load | ⏳ Pending |
| USB CDC serial silence | Silent after WiFi connects; observability via MQTT only | ⚠️ Known issue |

### Step 6 — USB CDC Serial Silence ⚠️ Open

On ESP32-S3, the native USB CDC shares interrupt/DMA resources with WiFi. Once
WiFi is active, heavy WiFi interrupt activity starves the USB CDC transmit path
and serial output goes silent. The device continues running normally — use
`mosquitto_sub` for observability in the meantime.

**Options (pick one):**

1. **Switch to UART0** — wire a USB-UART adapter to GPIO 43 (TX) / GPIO 44 (RX)
   and change `monitor_port` in `platformio.ini` to the adapter's port. UART is
   independent of WiFi and will not be affected. Recommended for ongoing
   development.

2. **Redirect ESP console to UART** — add `-DARDUINO_USB_CDC_ON_BOOT=0` and
   `-DARDUINO_UART_ON_BOOT=1` build flags to route `Serial` through UART0
   without hardware changes, but requires the USB-UART adapter.

3. **Accept silence** — if MQTT + MCP tools are sufficient for observability,
   serial logging is optional. Low priority if moving to Pi deployment soon.

---

## Key Design Decisions Recorded

| Decision | Choice | File |
|----------|--------|------|
| PSRAM mode | `qio_opi` (OPI, not QSPI) | `platformio-init.md` |
| TelemetryRecord layout | 44 bytes, 6-axis IMU + boot_id/sequence_id | `types.h` |
| Buffer overflow policy | Evict oldest (keep newest data) | `BufferManager.h` |
| Buffer thread-safety | FreeRTOS mutex; NOT ISR-safe by design | `BufferManager.h` |
| MCP transport | stdio (laptop); swap to SSE for Pi remote access | `mcp-server-architecture.md` |
| MQTT callbacks | Set event bits only — no Serial, no setState, no blocking calls. Serial.flush() inside a callback causes a crash on ESP32-S3 (USB CDC + WiFi interrupt contention). State transitions handled in connectionTask. | `main.cpp` |
| InfluxDB timestamps | Bridge uses broker-arrival time. Firmware `ts` field is `millis()` since boot (not Unix epoch) — not suitable as a DB timestamp until NTP is added. | `mqtt_to_influx.py` |
| ISR debounce | safetyISR checks `g_interlockActive` at entry and returns immediately if already latched. LDR signal is noisy on transition; without this guard, a single cover event produced 15+ estop publishes. | `main.cpp` |

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

---

## Development Environment

| Tool | Path |
|------|------|
| PlatformIO default projects | `/Users/markvale/Documents/PlatformIO/Projects` |
| Python venv (bridge) | `gateway/bridge/.venv` (Python 3.12) |
| MCP server dist | `mcp-server/dist/index.js` (rebuild with `npm run build` after editing `src/index.ts`) |
