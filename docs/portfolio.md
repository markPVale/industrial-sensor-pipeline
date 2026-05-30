# Portfolio Writeup — Industrial Sensor Pipeline

_Source document. Adapt sections for LinkedIn posts, profile About section,
and portfolio site as needed._

---

## Headline Options

- Built a production-grade industrial IoT pipeline from bare metal to AI query layer
- End-to-end vibration monitoring system: ESP32 firmware → MQTT → InfluxDB → Claude Code
- From interrupt handlers to LLM tool calls — a full-stack IoT system with AI integration

---

## Short Summary (LinkedIn About / Portfolio Card)

Built a full-stack industrial sensor pipeline from the ground up — bare-metal
ESP32-S3 firmware through a Raspberry Pi gateway to a live Grafana dashboard and
an AI query layer via Model Context Protocol. The system monitors vibration
anomalies in rotating machinery, enforces hardware safety interlocks, and
guarantees zero data loss through network partitions using a store-and-forward
architecture backed by 8MB of on-chip PSRAM.

**Stack:** C++ / FreeRTOS · Python · Node.js / TypeScript · MQTT · InfluxDB ·
Grafana · Docker · MCP (Model Context Protocol)

---

## Full Project Description (Portfolio Site)

### Overview

An end-to-end industrial vibration monitoring system targeting rotating machinery
(motors, pumps). Real IMU data flows from hardware to a live dashboard and is
queryable by Claude Code using natural language — no dashboards, no raw logs,
just: _"Is my sensor healthy? Were there any anomalies in the last hour?"_

The project covers every layer of the stack: interrupt-driven firmware, a
resilient MQTT pipeline, time-series storage, operational dashboards, and an AI
integration layer built on the Model Context Protocol.

### What It Does

- **Samples** a 6-axis MPU-6050 IMU at 100Hz, applies per-axis Kalman filtering,
  and computes vibration RMS over a rolling window
- **Detects anomalies** when RMS exceeds a calibrated threshold and stamps the
  record with a status flag
- **Enforces a hardware safety interlock** via a photoresistor optical loop — a
  falling-edge GPIO interrupt triggers an E-Stop event within microseconds,
  regardless of what the telemetry pipeline is doing
- **Buffers locally** in 8MB of OPI PSRAM during WiFi outages (up to 50,000
  records) and burst-syncs to the gateway on reconnect with no gaps or duplicates
- **Recovers from sensor faults** — an SDA hot-unplug triggers a graduated
  escalation: bus recovery attempts, bounded auto-reboots, and a terminal
  "unavailable" state, with full observability at every step
- **Exposes live data to Claude Code** via a Model Context Protocol server on
  the Pi — tool calls translate to Flux queries against InfluxDB and return
  structured, human-readable results

### Hard Problems Solved

**Store-and-forward without data loss**
The NORMAL → BUFFERING → SYNCING state machine uses a single `std::atomic<NodeState>`
and a mutex-protected PSRAM ring buffer. MQTT callbacks are forbidden from
touching state or Serial — a lesson learned after a crash traced to USB CDC and
WiFi interrupt contention on the ESP32-S3. All state transitions happen in a
dedicated `connectionTask` that owns the MQTT socket exclusively.

**I2C fault recovery under hostile conditions**
A long-duration SDA hot-unplug leaves the MPU's I2C state machine in an
undefined state that survives power-to-the-sensor. The firmware issues a 9-pulse
SCL clock recovery sequence to unstick the bus, followed by a `PWR_MGMT_1
DEVICE_RESET` on every boot to clear any corrupted internal sensor state. Three
bounded auto-reboots escalate the fault flag from `DEGRADED` to `UNAVAILABLE` —
validated by hardware testing.

**RTC memory reliability on ESP32-S3**
`RTC_DATA_ATTR` was supposed to persist the fault reboot counter across
`esp_restart()`. Hardware testing revealed it resets to zero on every software
reboot with Arduino-ESP32, causing an infinite reboot loop. Replaced with NVS
storage, written only on state transitions — an `ESP_RST_POWERON` guard
replicates the original "clear on power-cycle, persist on software reset"
semantics correctly.

**AI-queryable sensor data**
The MCP server is a stateless query layer — no MQTT subscription, no in-memory
state. Every tool call is a Flux query against InfluxDB. The health tool decodes
the full 8-bit status flags bitmask and returns a plain-English `health_summary`
string, making the sensor state immediately legible to an LLM without prompt
engineering.

### Architecture

```
ESP32-S3 (100Hz sampling, FreeRTOS, 8MB PSRAM)
  └─ MPU-6050 IMU + Photoresistor safety interlock
       │ MQTT QoS 1 over WiFi
       ▼
Raspberry Pi 5 Gateway (Docker)
  ├─ Mosquitto MQTT broker
  ├─ Python bridge → InfluxDB 2.7
  ├─ Grafana dashboards
  └─ Node.js MCP server (SSE transport)
       │ Model Context Protocol
       ▼
Claude Code — natural language sensor queries
```

### Tech Stack

| Layer | Technology |
|-------|-----------|
| Firmware | C++17, FreeRTOS, Arduino-ESP32, PlatformIO |
| Transport | MQTT (PubSubClient, QoS 1) |
| Gateway | Python 3.12, paho-mqtt, influxdb-client |
| Storage | InfluxDB 2.7, Flux query language |
| Visualisation | Grafana, Next.js (MQTT WebSocket) |
| AI interface | Node.js, TypeScript, Model Context Protocol SDK |
| Infrastructure | Docker Compose, Raspberry Pi 5 |

---

## LinkedIn Post Draft

---

I built an end-to-end industrial IoT pipeline — from bare-metal firmware to an
AI query layer — and wanted to share some of the interesting problems it surfaced.

**The system:** an ESP32-S3 samples a 6-axis IMU at 100Hz, applies Kalman
filtering, detects vibration anomalies, and streams telemetry to a Raspberry Pi
gateway running Mosquitto + InfluxDB + Grafana. A Model Context Protocol server
on the Pi lets Claude Code query live sensor data in plain English.

**Three things that were harder than expected:**

1. **MQTT callbacks can't touch Serial on ESP32-S3.** USB CDC and WiFi share
interrupt resources. One `Serial.flush()` inside an `onConnect` callback caused
a crash that took a while to trace. The fix: callbacks only call
`xEventGroupSetBits` — all state transitions happen in a dedicated task that
owns the MQTT socket.

2. **`RTC_DATA_ATTR` doesn't survive `esp_restart()` on Arduino-ESP32.** I was
using RTC slow memory to persist a fault reboot counter across software reboots.
It reset to zero every time, causing an infinite reboot loop instead of capping
at 3 attempts. The fix was NVS storage with an `esp_reset_reason()` guard —
confirmed by watching `boot_id` increment correctly through a long-duration
SDA hot-unplug test.

3. **An SDA hot-unplug leaves the I2C bus in a state that survives
power-to-the-sensor.** The MPU-6050's internal state machine can end up stuck
mid-transaction. The solution: 9-pulse SCL clock recovery + `PWR_MGMT_1
DEVICE_RESET` before every `g_mpu.begin()` on boot. Combined with bounded
auto-reboots and graduated status flags (`DEGRADED` → `UNAVAILABLE`), the system
now communicates its health state clearly rather than silently looping.

The part I'm most happy with: querying the sensor with Claude Code and getting
back _"DEGRADED — I2C fault detected. Auto-reboot pending."_ from a live tool
call. Closing the loop from hardware fault to AI-readable diagnosis.

Repo: github.com/markPVale/industrial-sensor-pipeline

---

_#ESP32 #IoT #FirmwareDevelopment #EmbeddedSystems #MCP #ClaudeCode_
