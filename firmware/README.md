# Firmware — ESP32-S3 N16R8 Industrial Sensor Node

## Hardware Target

| Parameter     | Value                              |
|---------------|------------------------------------|
| MCU           | ESP32-S3                           |
| Module        | N16R8 (16 MB Flash, 8 MB OPI PSRAM)|
| Board def     | `esp32-s3-devkitc-1`               |
| Framework     | Arduino (via PlatformIO)           |

## Project Structure

```
firmware/
├── platformio.ini      # Build config, board overrides, library deps
├── src/
│   └── main.cpp        # Bootstrap: PSRAM, MPU-6050, FreeRTOS scaffolding
├── include/
│   └── config.h        # Pin assignments, buffer sizes, compile-time constants
└── lib/                # Custom drivers added here as the project grows
```

## Building

```bash
pio run -e esp32s3-n16r8
```

## Flashing

```bash
pio run -e esp32s3-n16r8 --target upload
```

## Serial Monitor

```bash
pio device monitor --filter esp32_exception_decoder
```

## Key Configuration Notes

- **PSRAM mode** is set to `qio_opi` (Octal PSRAM). If you swap to an N8R8
  module, change `board_build.arduino.memory_type` to `qio_qspi`.
- **Partition table** uses the built-in `default_16MB.csv` from arduino-esp32.
  Customise if OTA slots or SPIFFS sizing need to change.
- **I2C pins** (SDA=8, SCL=9) are the DevKitC-1 defaults. Override in
  `include/config.h` if your wiring differs.
