# Project Status — v1

_Snapshot: Phase 1 complete, Phase 2 not started._

---

## What We Have

### Repository Structure

```
industrial-sensor-pipeline/
├── firmware/                       # ESP32-S3 PlatformIO project
│   ├── src/main.cpp                # Boot sequence (PSRAM check, MPU-6050 init) — FreeRTOS stubs present
│   ├── include/config.h            # Pin assignments, sample rate, buffer capacity, MQTT port
│   └── platformio.ini              # 16MB flash, OPI PSRAM (qio_opi), USB CDC, lib_deps
├── gateway/
│   ├── docker-compose.yml          # Mosquitto 2, InfluxDB 2.7, Grafana — all configured
│   ├── config/mosquitto.conf       # TCP :1883 + WebSocket :9001 listeners
│   └── bridge/
│       ├── mock_esp32.py           # Simulated sensor node (NORMAL → ANOMALY → ESTOP loop)
│       ├── mqtt_to_influx.py       # MQTT subscriber → InfluxDB writer
│       └── requirements.txt
├── dashboard/
│   ├── app/page.tsx                # Root page, renders TelemetryDisplay for node01
│   ├── app/layout.tsx + globals.css
│   ├── components/
│   │   ├── TelemetryDisplay.tsx    # Live RMS, flags, status badge
│   │   └── HeartbeatIndicator.tsx  # Connection heartbeat
│   ├── hooks/useMqttTelemetry.ts   # MQTT WebSocket hook, 100-record rolling history
│   ├── next.config.ts
│   ├── package.json
│   └── tsconfig.json
├── mcp-server/
│   ├── src/index.ts                # MCP server (stdio), three tools backed by Flux queries
│   ├── package.json
│   └── tsconfig.json
└── docs/
    ├── project-context.md          # Source-of-truth architecture doc
    └── claude-notes/
        ├── platformio-init.md      # PlatformIO setup decisions
        ├── gateway-stack.md        # Docker stack decisions
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
| Docker gateway stack | `gateway/docker-compose.yml` | Mosquitto, InfluxDB v2, Grafana |
| Mosquitto config | `gateway/config/mosquitto.conf` | TCP + WebSocket listeners |
| MQTT → InfluxDB bridge | `gateway/bridge/mqtt_to_influx.py` | Handles `telemetry` and `estop` topics |
| Mock ESP32 publisher | `gateway/bridge/mock_esp32.py` | State machine with realistic synthetic signal |
| Next.js dashboard | `dashboard/` | Live MQTT WebSocket display, rolling history |
| MCP server | `mcp-server/src/index.ts` | `get_latest_telemetry`, `get_sensor_health`, `get_recent_anomalies` |

### Phase 2 — Firmware Logic ❌ Not Started

All firmware application logic is absent. `main.cpp` contains a working boot sequence and commented task stubs only.

### Phase 3 — Hardware Integration ⏳ Blocked

Hardware not yet arrived.

### Phases 4 & 5 — HIL Testing, Pi Deployment, Docs ❌ Not Started

---

## Next Steps (Priority Order)

### 0. MCP Server — Verify Phase 1 end-to-end ← do this first

These are quick verification tasks before Phase 2 begins. They confirm the full simulation stack works together.

- [ ] `cd mcp-server && npm install && npm run build` — fix any TypeScript errors before adding more tools
- [ ] Add `.mcp.json` to the repo root so Claude Code finds the server without manual config:
  ```json
  {
    "mcpServers": {
      "sensor": {
        "command": "node",
        "args": ["./mcp-server/dist/index.js"],
        "env": {
          "INFLUX_URL": "http://localhost:8086",
          "INFLUX_TOKEN": "dev-token-change-in-production"
        }
      }
    }
  }
  ```
- [ ] End-to-end smoke test: run `docker compose up -d` (gateway), `mock_esp32.py`, `mqtt_to_influx.py`, then ask Claude Code _"Is node01 healthy?"_ — confirms the full path works
- [ ] Switch to `SSEServerTransport` (HTTP) when the Pi is ready and remote access is needed without SSH — see `docs/claude-notes/mcp-server-architecture.md`

---

### 1. `firmware/lib/BufferManager` ← start here
- Circular buffer over `ps_malloc()` in PSRAM
- Capacity: `PSRAM_BUFFER_CAPACITY` (50,000 × `TelemetryRecord` ≈ 650 KB)
- Operations: `push()`, `pop()`, `isFull()`, `isEmpty()`, `count()`
- Must be thread-safe (accessed from multiple FreeRTOS tasks)

### 2. `firmware/lib/KalmanFilter`
- 1D scalar Kalman for a single accelerometer axis
- Parameters: process noise Q, measurement noise R
- Used by `filterTask` to clean MPU-6050 readings before RMS computation

### 3. FreeRTOS task skeletons in `main.cpp`
Uncomment and implement the four tasks:

| Task | Core | Priority | Responsibility |
|------|------|----------|----------------|
| `sensorTask` | 1 | 5 | Sample MPU-6050 @ 100 Hz, push raw data to queue |
| `filterTask` | 1 | 5 | Consume raw queue, apply Kalman, compute RMS/peak |
| `telemetryTask` | 0 | 3 | Publish filtered records via MQTT QoS 1 |
| `syncTask` | 0 | 3 | Flush PSRAM buffer to gateway on reconnect |

### 4. Safety ISR + FreeRTOS Event Group
- `IRAM_ATTR` ISR on `PIN_SAFETY_INTERLOCK` (GPIO 10), falling edge
- Posts to an `EventGroup` bit; a dedicated safety task reads it and triggers E-Stop
- E-Stop must publish to `sensor/<node>/estop` and block `telemetryTask` output

### 5. NORMAL → BUFFERING → SYNCING State Machine
- `NORMAL`: MQTT connected, publish in real time
- `BUFFERING`: MQTT disconnected, write to PSRAM circular buffer
- `SYNCING`: MQTT reconnected, resume real-time stream + rate-limited burst of buffered records

### 6. Validate Against Mock Script
- Run `mock_esp32.py` as a receiver (subscribe mode) to confirm firmware output matches expected schema
- Confirm `mqtt_to_influx.py` ingests firmware messages correctly

---

## Port Reference

| Service | Host Port |
|---------|-----------|
| Mosquitto MQTT | 1883 |
| Mosquitto WebSocket | 9001 |
| InfluxDB | 8086 |
| Grafana | 3001 |
| Next.js | 3000 |
| MCP Server | 3002 (Pi deployment) |
