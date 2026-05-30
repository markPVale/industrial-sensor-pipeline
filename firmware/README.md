# Firmware — ESP32-S3 N16R8 Industrial Sensor Node

## Hardware Target

| Parameter | Value |
|-----------|-------|
| MCU | ESP32-S3 |
| Module | N16R8 (16 MB Flash, 8 MB OPI PSRAM) |
| Board definition | `esp32-s3-devkitc-1` |
| Framework | Arduino (via PlatformIO) |
| IMU | MPU-6050 at I2C address 0x68 (SDA=8, SCL=9) |
| Safety interlock | Photoresistor on GPIO 10, falling-edge ISR |

## Project Structure

```
firmware/
├── platformio.ini          # Build config, board overrides, library deps
├── secrets.ini.template    # Copy to secrets.ini and fill in credentials
├── src/
│   └── main.cpp            # All FreeRTOS tasks, state machine, MQTT
├── include/
│   ├── config.h            # Pin assignments, tuning constants
│   └── types.h             # TelemetryRecord, NodeState, status flags
└── lib/
    ├── BufferManager/      # PSRAM ring buffer (50,000 records, mutex-protected)
    ├── KalmanFilter/       # 1D scalar Kalman filter with spike rejection
    └── MqttManager/        # WiFi + PubSubClient with exponential backoff
```

## Setup

```bash
# Copy secrets template and fill in WiFi credentials + MQTT broker IP
cp secrets.ini.template secrets.ini
```

## Build & Flash

```bash
# Build only
pio run

# Build + flash
pio run --target upload

# Open serial monitor (USB CDC — goes silent once WiFi connects, see note below)
pio device monitor
```

**macOS note:** PlatformIO may auto-detect the wrong port (Bluetooth). If upload
fails, specify the port explicitly:

```bash
pio run --target upload --upload-port /dev/cu.usbmodem1101
```

**USB CDC serial note:** On ESP32-S3, USB CDC shares DMA resources with WiFi.
Serial output goes silent once WiFi is active. Use `mosquitto_sub` on the Pi
for runtime observability instead of the serial monitor.

## Architecture

### FreeRTOS Tasks

| Task | Core | Priority | Role |
|------|------|----------|------|
| `safetyTask` | 0 | 6 | ISR handler — latches interlock, publishes E-Stop |
| `sensorTask` | 1 | 5 | 100Hz MPU-6050 sampling via `vTaskDelayUntil` |
| `filterTask` | 1 | 5 | 6× Kalman filters, rolling RMS, anomaly detection |
| `connectionTask` | 0 | 4 | Sole MQTT socket owner, drains publish queue |
| `telemetryTask` | 0 | 3 | Pops buffer, publishes at 2Hz |
| `syncTask` | 0 | 3 | Burst-drains PSRAM buffer on reconnect |

Safety and sensor work runs on Core 1; all network I/O runs on Core 0.

### Store-and-Forward State Machine

```
NORMAL ──(disconnect)──▶ BUFFERING ──(reconnect)──▶ SYNCING ──(empty)──▶ NORMAL
```

- **NORMAL** — real-time 2Hz telemetry via MQTT
- **BUFFERING** — records written to PSRAM ring buffer (up to 50,000 × 44 bytes ≈ 2.1 MB)
- **SYNCING** — live stream resumes; `syncTask` burst-drains buffer in batches of 20

`NodeState` is a single `std::atomic<NodeState>` — no scattered boolean flags.
All MQTT callbacks only call `xEventGroupSetBits` — no Serial, no state writes
(USB CDC + WiFi interrupt contention causes crashes if callbacks block).

### I2C Fault Recovery

`sensorTask` probes the MPU-6050 via WHO_AM_I on every sample. After 5 consecutive
failures it enters a fault state:

1. Emits one fault record immediately, then one every 5s
2. Attempts 9-pulse SCL bus recovery every 3s
3. After 30s continuous fault: software reboot via `esp_restart()`
4. Reboots are capped at 3 per power-on session (counter stored in NVS)
5. After the 3rd reboot: transitions to `STATUS_SENSOR_UNAVAILABLE` — no further
   reboots, fault records every 5s until SDA is reconnected or power-cycled

The reboot counter uses NVS (not `RTC_DATA_ATTR` — RTC slow memory does not
reliably survive `esp_restart()` on ESP32-S3 with Arduino-ESP32).

`initMPU6050()` issues a `PWR_MGMT_1 DEVICE_RESET` before every `g_mpu.begin()`
to clear any corrupted internal state left by a hot-unplug.

### Status Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `STATUS_OK` | `0x00` | Normal sample |
| `STATUS_ACCEL_CLIPPED` | `0x01` | Accelerometer hit ±8g range limit |
| `STATUS_GYRO_CLIPPED` | `0x02` | Gyro hit ±500°/s range limit |
| `STATUS_INTERLOCK_OPEN` | `0x04` | Safety interlock was open at sample time |
| `STATUS_ANOMALY` | `0x08` | Vibration RMS exceeded threshold (12.5 m/s²) |
| `STATUS_SENSOR_FAULT` | `0x10` | I2C dropout — WHO_AM_I probe failed (legacy) |
| `STATUS_DEGRADED_REBOOT_REQUIRED` | `0x20` | I2C fault; auto-reboot pending |
| `STATUS_SENSOR_UNAVAILABLE` | `0x40` | Max reboots exhausted; power-cycle required |

## Key Configuration (`include/config.h`)

| Constant | Default | Description |
|----------|---------|-------------|
| `SAMPLE_RATE_HZ` | 100 | MPU-6050 sample rate |
| `FILTER_WINDOW_SIZE` | 50 | Samples per telemetry record (~2Hz output) |
| `PSRAM_BUFFER_CAPACITY` | 50000 | Max records in PSRAM ring buffer |
| `SENSOR_FAULT_THRESHOLD` | 5 | Consecutive WHO_AM_I failures before fault |
| `SENSOR_FAULT_RECOVERY_INTERVAL_MS` | 3000 | Interval between SCL recovery attempts |
| `SENSOR_FAULT_REBOOT_MS` | 30000 | Time in fault before auto-reboot |
| `SENSOR_FAULT_MAX_REBOOTS` | 3 | Max reboots per power-on session |
| `SENSOR_FAULT_EMIT_INTERVAL_MS` | 5000 | Fault record rate limit |
| `TELEMETRY_PUBLISH_MS` | 500 | Publish interval in NORMAL/SYNCING state |

## NVS Layout

All persistent state uses the `"sensor"` namespace:

| Key | Type | Description |
|-----|------|-------------|
| `boot_id` | `uint32` | Monotonic boot counter, incremented every boot |
| `fault_reboots` | `uint8` | Fault-triggered reboot count, cleared on power-on or 10s recovery |
| `cal_ax_off` / `cal_ax_scl` | `float` | Accelerometer X calibration (offset / scale) |
| `cal_ay_off` / `cal_ay_scl` | `float` | Accelerometer Y calibration |
| `cal_az_off` / `cal_az_scl` | `float` | Accelerometer Z calibration |

## PSRAM Note

The N16R8 uses OPI (Octal Peripheral Interface) PSRAM. `platformio.ini` sets
`board_build.arduino.memory_type = qio_opi` to select the matching bootloader.
If you swap to an N8R8 module, change this to `qio_qspi`.
