#pragma once

// =============================================================================
// config.h — Board pin assignments and compile-time constants
// ESP32-S3 N16R8 Industrial Sensor Node
// =============================================================================

// -----------------------------------------------------------------------------
// I2C — MPU-6050
// Default Arduino-ESP32 I2C pins on ESP32-S3-DevKitC-1.
// Verify against your physical wiring.
// -----------------------------------------------------------------------------
#define PIN_SDA  8
#define PIN_SCL  9

// -----------------------------------------------------------------------------
// Safety Interlock — Photoresistor GPIO interrupt
// Adjust to the GPIO you wire the optical safety loop to.
// Must be an RTC-capable pin if deep-sleep wake is required.
// -----------------------------------------------------------------------------
#define PIN_SAFETY_INTERLOCK  10

// -----------------------------------------------------------------------------
// Sensor Sampling
// -----------------------------------------------------------------------------
#define SAMPLE_RATE_HZ        100   // MPU-6050 target sample rate (100 Hz)

// -----------------------------------------------------------------------------
// Store-and-Forward Buffer
// Sized against available PSRAM. 8MB OPI PSRAM gives ~8,388,608 bytes.
// TelemetryRecord is 13 bytes; 50,000 records ~ 650 KB — well within budget.
// -----------------------------------------------------------------------------
#define PSRAM_BUFFER_CAPACITY 50000  // max telemetry records held in PSRAM

// -----------------------------------------------------------------------------
// MQTT
// Placeholders — fill from NVS, provisioning flow, or compile-time defines.
// -----------------------------------------------------------------------------
#define MQTT_PORT             1883
// #define MQTT_BROKER_IP     "192.168.1.x"   // set at runtime or via NVS
// #define MQTT_TOPIC_TELEMETRY "sensor/node01/telemetry"
// #define MQTT_TOPIC_ESTOP     "sensor/node01/estop"
