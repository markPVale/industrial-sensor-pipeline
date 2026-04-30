#pragma once

#include <stdint.h>

// =============================================================================
// types.h — Shared data structures for the industrial sensor node
// ESP32-S3 N16R8
//
// This header is included by firmware tasks, BufferManager, and any layer
// that serialises records for MQTT or MCP. Keep this struct layout stable —
// changing field sizes or order breaks binary compatibility with stored records.
// =============================================================================

// -----------------------------------------------------------------------------
// TelemetryRecord status flag bits
// -----------------------------------------------------------------------------
#define STATUS_OK             0x00  // normal sample
#define STATUS_ACCEL_CLIPPED  0x01  // accelerometer hit range limit
#define STATUS_GYRO_CLIPPED   0x02  // gyro hit range limit
#define STATUS_INTERLOCK_OPEN 0x04  // safety interlock was open at sample time
#define STATUS_ANOMALY        0x08  // anomaly detection flagged this record
#define STATUS_SENSOR_FAULT   0x10  // I2C dropout — consecutive zero reads from MPU-6050

// -----------------------------------------------------------------------------
// TelemetryRecord
//
// boot_id + sequence_id together form a globally unique record identity
// across reboots. boot_id should be loaded from NVS on startup and
// incremented each boot cycle.
//
// Size: 4 + 4 + 8 + (6 × 4) + 1 + 3 (padding) = 44 bytes
// At 50,000 records that is ~2.1 MB — well within the 8 MB PSRAM budget.
// -----------------------------------------------------------------------------
struct TelemetryRecord {
    uint32_t boot_id;       // increments each reboot (persisted in NVS)
    uint32_t sequence_id;   // monotonic counter, resets to 0 each boot
    uint64_t timestamp_ms;  // millis() at time of sample
    float    accel_x;       // m/s²
    float    accel_y;       // m/s²
    float    accel_z;       // m/s²
    float    gyro_x;        // rad/s
    float    gyro_y;        // rad/s
    float    gyro_z;        // rad/s
    uint8_t  status_flags;  // bitmask — see STATUS_* defines above
};

// -----------------------------------------------------------------------------
// BufferStats — opaque snapshot of BufferManager state.
// Serialise this to JSON at the telemetry/MCP layer; do not embed JSON
// formatting inside BufferManager itself.
// -----------------------------------------------------------------------------
struct BufferStats {
    size_t capacity;
    size_t count;
    size_t free_space;
    size_t dropped;
};

// -----------------------------------------------------------------------------
// NodeState — store-and-forward state machine.
//
// Transitions (enforced in main.cpp via setState()):
//   NORMAL    → BUFFERING  on MQTT disconnect
//   BUFFERING → SYNCING    on MQTT reconnect
//   SYNCING   → NORMAL     when g_buffer is empty after drain
//
// All tasks key their behavior off this single enum.
// No task owns a local "connected" flag — check getState() instead.
// -----------------------------------------------------------------------------
enum class NodeState : uint8_t {
    NORMAL,     // Connected. telemetryTask pops and publishes live.
    BUFFERING,  // Disconnected. Records accumulate in PSRAM. No task pops.
    SYNCING,    // Reconnected. syncTask drains buffer; telemetryTask resumes.
};
