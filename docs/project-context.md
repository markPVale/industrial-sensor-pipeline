# Project Context: High-Reliability Industrial Sensor Node & HIL Harness

## Executive Summary (The "Why")
This project addresses three major operational risks in mission-critical industrial environments:
1. **Unplanned Downtime:** Early detection of harmonic vibration anomalies in rotating machinery (motors, pumps) to enable predictive maintenance.
2. **Safety Liability:** Real-time enforcement of hardware interlocks via optical safety loops to prevent workplace injury.
3. **Data Integrity & Compliance:** Guaranteeing audit-ready telemetry logs through network partitions using a "Store-and-Forward" architecture.

---

## System Architecture

### 1. Industrial Sensor Node (The "Product")
- **Hardware:** ESP32-S3 (N16R8) with 8MB PSRAM.
- **Sensors:** MPU-6050 (Vibration DAQ) and Photoresistor (Safety Interlock).
- **Edge Intelligence:**
  - Kalman filtering for noise reduction.
  - RMS and Peak amplitude tracking for anomaly scoring.
- **Safety Interlock:** GPIO interrupt tied to a photoresistor loop for near-instantaneous (<10ms) "E-Stop" logic.

### 2. Edge Gateway & HIL Lab (The "Test Bench")
- **Gateway:** Raspberry Pi 5 running Mosquitto MQTT and InfluxDB.
- **Digital Twin:** Next.js dashboard for real-time telemetry visualization and vibration trend analysis.
- **Chaos Testing Harness:** UI-driven tool to inject faults (Network partition, Gateway restart, Sensor drift) to validate firmware recovery.

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
  uint64_t timestamp; // Edge-side timestamp to prevent clock drift
  float vibration_rms;
  uint8_t flags;      // e.g., E-Stop status, Anomaly detected
};