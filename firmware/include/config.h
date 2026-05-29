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
// TelemetryRecord is 44 bytes; 50,000 records ~ 2.1 MB — well within budget.
// -----------------------------------------------------------------------------
#define PSRAM_BUFFER_CAPACITY 50000  // max telemetry records held in PSRAM

// -----------------------------------------------------------------------------
// Filter / Telemetry pipeline
// -----------------------------------------------------------------------------
// filterTask accumulates this many raw samples before emitting one
// TelemetryRecord. At SAMPLE_RATE_HZ=100 this gives a ~2 Hz output rate.
#define FILTER_WINDOW_SIZE    50

// Consecutive failed WHO_AM_I probes before entering fault state.
// At 100 Hz, 5 failures = 50ms — long enough to ignore a transient glitch,
// short enough to catch a real I2C dropout quickly.
#define SENSOR_FAULT_THRESHOLD  5

// I2C fault recovery interval (milliseconds).
// recoverI2cBus() is first called 3s after fault entry, then every 3s.
// The 3s deferral avoids calling Wire.begin() while SDA is still absent,
// which corrupts the Wire peripheral and blocks all subsequent recovery.
#define SENSOR_FAULT_RECOVERY_INTERVAL_MS   3000

// Hard reboot after this many ms of continuous sensor fault.
// Wire.begin() on a broken bus corrupts the Wire peripheral; a soft reset
// reinitialises it from a clean hardware state. 30s allows multiple recovery
// attempts before giving up. boot_id increments on restart — visible in telemetry.
#define SENSOR_FAULT_REBOOT_MS             30000

// Maximum fault-triggered reboots per power-on session (tracked in RTC memory).
// After this many resets the node stays in fault state permanently until power-cycled.
// Prevents infinite reboot loops when the MPU is unrecoverably damaged.
#define SENSOR_FAULT_MAX_REBOOTS           3

// Depth of the RawSample queue between sensorTask and filterTask.
// At 100 Hz and a 10 ms filter budget, 20 slots gives 200 ms of slack.
#define SENSOR_QUEUE_DEPTH    20

// Interval at which telemetryTask pops and publishes one record (~2 Hz).
#define TELEMETRY_PUBLISH_MS  500

// Rate limit for fault record emission while in fault state.
// One record per interval — prevents flooding the PSRAM buffer and MQTT.
#define SENSOR_FAULT_EMIT_INTERVAL_MS   5000

// Sync burst parameters — records per batch and inter-batch delay.
#define SYNC_BATCH_SIZE       20
#define SYNC_BATCH_DELAY_MS   100

// -----------------------------------------------------------------------------
// WiFi credentials
// TODO: Replace these with NVS reads once NVS storage is implemented.
//       Never commit real credentials — override via a local untracked header
//       or a PlatformIO build_flags -D override.
// -----------------------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID             "your-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD         "your-password"
#endif

// -----------------------------------------------------------------------------
// MQTT
// MQTT_BROKER_IP: IP of the Raspberry Pi (or dev machine) running the
//                 docker-compose gateway stack on port 1883.
// MQTT_CLIENT_ID: Must be unique per node if you add more nodes.
// TODO: Load broker IP and client ID from NVS in a later phase.
// -----------------------------------------------------------------------------
#ifndef MQTT_BROKER_IP
#define MQTT_BROKER_IP        "192.168.1.100"
#endif
#define MQTT_PORT             1883
#define MQTT_CLIENT_ID        "sensor-node01"
#define MQTT_TOPIC_TELEMETRY  "sensor/node01/telemetry"
#define MQTT_TOPIC_ESTOP      "sensor/node01/estop"
#define MQTT_KEEPALIVE_S      15    // PubSubClient keepalive interval in seconds

// Outbound publish queue: MqttMessage items enqueued by telemetryTask/syncTask
// and drained by connectionTask. 40 slots × ~390 bytes each ≈ 15 KB.
#define MQTT_PUBLISH_QUEUE_DEPTH  40
#define MQTT_PAYLOAD_SIZE         320   // bytes — fits one ArduinoJson telemetry record
