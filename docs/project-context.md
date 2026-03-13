# Project Context: High-Reliability Industrial Sensor Node & HIL Harness

## Executive Summary (The "Why")
This project addresses three major operational risks in mission-critical industrial environments:
1. **Unplanned Downtime:** Early detection of harmonic vibration anomalies in rotating machinery (motors, pumps) to enable predictive maintenance.
2. **Safety Liability:** Real-time enforcement of hardware interlocks via optical safety loops to prevent workplace injury.
3. **Data Integrity & Compliance:** Guaranteeing audit-ready telemetry logs through network partitions using a "Store-and-Forward" architecture.

In addition, the system exposes physical sensor telemetry to LLM clients (Claude Code, etc.) via a Model Context Protocol (MCP) server, enabling AI-assisted diagnostics directly from a developer's terminal.

---

## System Architecture

### 1. Industrial Sensor Node (The "Product")
- **Hardware:** ESP32-S3 (N16R8) with 16MB Flash and 8MB Octal PSRAM.
- **Sensors:** MPU-6050 (Vibration DAQ) and Photoresistor (Safety Interlock).
- **Edge Intelligence:**
  - Kalman filtering for noise reduction.
  - RMS and Peak amplitude tracking for anomaly scoring.
- **Safety Interlock:** GPIO interrupt tied to a photoresistor loop for near-instantaneous (<10ms) "E-Stop" logic.

### 2. Edge Gateway & HIL Lab (The "Test Bench")
- **Gateway:** Raspberry Pi 5 running the following services:
  - **Mosquitto** — MQTT broker (TCP port 1883, WebSocket port 9001)
  - **InfluxDB v2** — Time-series storage for all telemetry and events
  - **Grafana** — Operational dashboards (port 3001)
  - **Node.js MCP Server** — LLM interface layer exposing sensor tools to Claude Code (port 3002)
- **Digital Twin:** Next.js dashboard for real-time telemetry visualization and vibration trend analysis.
- **Chaos Testing Harness:** UI-driven tool to inject faults (Network partition, Gateway restart, Sensor drift) to validate firmware recovery.

### 3. AI Integration Layer (The "LLM Interface")
- **MCP Server:** A lightweight Node.js process running on the Raspberry Pi that exposes structured sensor tools to any MCP-compatible LLM client.
- **Client:** Claude Code (or any MCP client) running on a developer laptop, connecting to the MCP server over the local network.
- **Purpose:** Allows a developer to query live and recent sensor state using natural language — without writing queries, opening dashboards, or reading raw logs.

---

## Data Flow

### Normal Telemetry Path
```
ESP32-S3 (100Hz MPU-6050 sampling)
  → Kalman filter + RMS computation (on-device)
  → MQTT publish QoS 1 @ ~2Hz  ──────────────────────────────────────┐
                                                                      ▼
                                                          Mosquitto (Pi :1883)
                                                                ├── mqtt_to_influx bridge
                                                                │       └── InfluxDB (Pi :8086)
                                                                └── WebSocket (Pi :9001)
                                                                        └── Next.js Dashboard
```

### AI Query Path
```
Developer: "Check my sensor for anomalies"
  → Claude Code
    → MCP tool call (e.g. get_recent_anomalies)
      → MCP Server (Node.js, Pi :3002)
        → Flux query → InfluxDB (Pi :8086)
          → structured JSON result
        → MCP Server formats response
      → Claude Code receives tool result
    → Claude reasons over data
  → Natural language answer + recommendations
```

### Store-and-Forward Path (Network Partition)
```
ESP32-S3 (MQTT disconnect detected)
  → STATE: NORMAL → BUFFERING
  → Telemetry records written to PSRAM circular buffer
  → (reconnect)
  → STATE: BUFFERING → SYNCING
  → Real-time stream resumes + historical burst to gateway
  → STATE: SYNCING → NORMAL
```

---

## Firmware Architecture (FreeRTOS)
The ESP32 runs a multi-tasking scheduler to ensure safety events cannot be blocked by telemetry workloads:
- **Safety Interrupt (Highest):** Triggers emergency stop event immediately.
- **Sensor Task (Medium):** Samples MPU-6050 @ 100Hz.
- **Filtering Task (Medium):** Runs Kalman filters and computes vibration metrics.
- **Telemetry/Sync Tasks (Low):** Manages MQTT publishing and PSRAM buffer flushing.

---

## Resilience Engine (Store-and-Forward)
A state machine governs telemetry behavior during failure:
- **NORMAL:** Real-time streaming via MQTT QoS 1.
- **BUFFERING:** On disconnect, telemetry records are written to a **Circular Buffer in PSRAM**.
- **SYNCING:** On reconnect, the system prioritizes real-time alerts while "bursting" historical data in rate-limited batches to the gateway.

**Data Record Schema:**
```cpp
struct TelemetryRecord {
  uint64_t timestamp;   // Edge-side timestamp to prevent clock drift
  float vibration_rms;  // Computed RMS over sample window
  uint8_t flags;        // Bitmask: bit0=E-Stop, bit1=Anomaly
};
```

---

## MCP Server — Tool Reference

The MCP server exposes the following tools to LLM clients:

| Tool | Description |
|------|-------------|
| `get_latest_telemetry` | Returns the most recent telemetry record (vibration RMS, flags, timestamp) for a node |
| `get_sensor_health` | Returns a health summary: online status, last-seen age, active anomaly/E-Stop flags |
| `get_recent_anomalies` | Returns anomaly events within a configurable lookback window |

**Connection:** The MCP server runs on the Pi and listens for MCP client connections. Configure Claude Code's MCP settings to point to the Pi's IP and port 3002.

---

## Repository Structure

```
industrial-sensor-pipeline/
├── firmware/           # ESP32-S3 PlatformIO project
├── gateway/            # Raspberry Pi 5 services
│   ├── docker-compose.yml
│   ├── bridge/         # Python MQTT → InfluxDB bridge + mock script
│   └── config/         # Mosquitto config
├── mcp-server/         # Node.js MCP server (LLM interface layer)
├── dashboard/          # Next.js digital twin dashboard
└── docs/               # Architecture docs and Claude notes
```

---

## Port Reference

| Service         | Host Port | Notes                              |
|-----------------|-----------|------------------------------------|
| Mosquitto MQTT  | 1883      | Firmware, bridge, mock script      |
| Mosquitto WS    | 9001      | Next.js dashboard (mqtt.js)        |
| InfluxDB        | 8086      | Bridge writes, MCP server queries  |
| Grafana         | 3001      | Operational dashboards             |
| Next.js         | 3000      | Developer dashboard                |
| MCP Server      | 3002      | Claude Code MCP client connection  |
