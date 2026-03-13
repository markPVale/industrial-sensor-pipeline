# PlatformIO Firmware Initialisation

## Context

Initial setup of the `/firmware` directory as a PlatformIO project targeting an ESP32-S3 N16R8 module (16 MB Flash, 8 MB Octal PSRAM). The firmware will run FreeRTOS tasks for vibration sensing, Kalman filtering, MQTT telemetry, and a store-and-forward resilience engine backed by PSRAM.

---

## Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Board definition | `esp32-s3-devkitc-1` | No native N16R8 board JSON in PlatformIO; DevKitC-1 is the closest match with manual overrides applied |
| PSRAM mode | `qio_opi` | N16R8 uses OPI (Octal) PSRAM, not the QSPI variant default on DevKitC-1 |
| Flash override | 16 MB | DevKitC-1 defaults to 8 MB; explicitly overridden for both build and upload |
| Partition table | `default_16MB.csv` | Built-in arduino-esp32 table; ~3 MB per OTA slot, ~9.9 MB SPIFFS |
| USB Serial | CDC on boot | `ARDUINO_USB_CDC_ON_BOOT=1` routes `Serial` over native USB, not UART bridge |

---

## Configuration Details

### `platformio.ini`

```ini
board_build.flash_size          = 16MB
board_upload.flash_size         = 16MB
board_build.partitions          = default_16MB.csv
board_build.arduino.memory_type = qio_opi

build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
    knolleary/PubSubClient
    adafruit/Adafruit MPU6050
    bblanchon/ArduinoJson
```

### `include/config.h`

- I2C: SDA=8, SCL=9 (DevKitC-1 defaults — verify against physical wiring)
- Safety interlock: GPIO 10 (placeholder; update to actual wired pin)
- Sample rate: 100 Hz
- PSRAM buffer capacity: 50,000 `TelemetryRecord` entries (~650 KB)

### `src/main.cpp`

Performs in order:
1. Serial init (USB CDC, 1 s delay for host enumeration)
2. `ESP.getPsramSize()` — logs size or prints diagnostic if 0
3. `Wire.begin()` + `Adafruit_MPU6050::begin()` — range and DLPF configured
4. Commented stubs for: safety ISR, WiFi/MQTT init, PSRAM buffer allocation, FreeRTOS task creation

MPU-6050 defaults set:
- Accelerometer: ±8 g
- Gyro: ±500 °/s
- DLPF: 21 Hz cutoff

---

## Reasoning

### `board_build.arduino.memory_type = qio_opi`

arduino-esp32 ships multiple bootloader binaries keyed by flash/PSRAM type. `qio_opi` selects the OPI-PSRAM-aware bootloader. Without it, the PSRAM controller is not initialised and `ESP.getPsramSize()` returns 0 regardless of physical hardware.

### `-DBOARD_HAS_PSRAM`

Required by the ESP-IDF Arduino core to include PSRAM initialisation code at startup. The `memory_type` flag selects the bootloader; this flag enables the runtime initialisation path.

### 21 Hz DLPF on MPU-6050

At a 100 Hz sample rate, a 21 Hz low-pass cutoff prevents aliasing of high-frequency mechanical noise while preserving the vibration frequency bands relevant to rotating machinery (typically 5–50 Hz for motors/pumps).

---

## Hardware Verification Checklist

Before deploying to real hardware:

- [ ] Run `esptool.py flash_id` — confirm flash reports 16 MB
- [ ] Boot and check `ESP.getPsramSize()` — should report ~8,388,608 bytes
- [ ] Confirm I2C device appears at `0x68` (or `0x69` if AD0 is pulled high)
- [ ] Verify SDA/SCL GPIO assignments match physical wiring
- [ ] Confirm USB CDC enumerates on host OS (`/dev/ttyACM*` or similar)
- [ ] If swapping to N8R8 module: change `memory_type` to `qio_qspi`

---

## Next Steps

- [ ] Implement FreeRTOS task skeletons: `sensorTask`, `filterTask`, `telemetryTask`, `syncTask`
- [ ] Add `lib/BufferManager` — circular buffer over `ps_malloc()` for store-and-forward
- [ ] Add `lib/KalmanFilter` — 1D Kalman for vibration axis noise reduction
- [ ] Implement safety ISR (`IRAM_ATTR`) + FreeRTOS event group for E-Stop propagation
- [ ] Add MQTT connection manager with reconnect backoff
- [ ] Implement `NORMAL → BUFFERING → SYNCING` state machine
- [ ] Store MPU-6050 calibration offsets in NVS
- [ ] Create custom partition table if app binary exceeds OTA slot or SPIFFS needs change
