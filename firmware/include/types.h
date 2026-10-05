#pragma once

#include <stdint.h>
#include <stddef.h>

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
#define STATUS_SENSOR_FAULT              0x10  // I2C dropout — WHO_AM_I probe failed
#define STATUS_DEGRADED_REBOOT_REQUIRED  0x20  // I2C fault; auto-reboot pending (reboots remain)
#define STATUS_SENSOR_UNAVAILABLE        0x40  // I2C fault; max reboots exhausted, power-cycle required

// -----------------------------------------------------------------------------
// TelemetryRecord
//
// boot_id + sequence_id together form a globally unique record identity
// across reboots. boot_id should be loaded from NVS on startup and
// incremented each boot cycle.
//
// Size: 4 + 4 + 8 + (7 × 4) + 1 + 3 (padding) = 48 bytes
// At 50,000 records that is ~2.4 MB — well within the 8 MB PSRAM budget.
// -----------------------------------------------------------------------------
struct TelemetryRecord {
    uint32_t boot_id;       // increments each reboot (persisted in NVS)
    uint32_t sequence_id;   // monotonic counter, resets to 0 each boot
    uint64_t timestamp_ms;  // UTC epoch ms when synced; monotonic millis fallback pre-sync
    float    accel_x;       // m/s²
    float    accel_y;       // m/s²
    float    accel_z;       // m/s²
    float    gyro_x;        // rad/s
    float    gyro_y;        // rad/s
    float    gyro_z;        // rad/s
    float    window_rms;    // m/s² rolling RMS over FILTER_WINDOW_SIZE samples
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
    size_t evicted_oldest;
    size_t protected_evictions;
    size_t rejected_newest;
    size_t abandoned; // local abandonment decisions, not confirmed delivery loss
};

// -----------------------------------------------------------------------------
// NodeState — store-and-forward state machine.
//
// Transitions (owned exclusively by DeliveryRuntime in connectionTask):
//   NORMAL    → BUFFERING  on transport loss / unresolved ACK subscription
//   BUFFERING → SYNCING    on current connectivity + subscription request sent
//   NORMAL    → SYNCING    when more than one record is retained (catch-up)
//   SYNCING   → NORMAL     when g_buffer is empty after drain
//
// connectionTask alone owns the socket, pacing, lease accounting and transitions.
// -----------------------------------------------------------------------------
enum class NodeState : uint8_t {
    NORMAL,     // Producer-paced live delivery (~2 Hz), no separate cadence task.
    BUFFERING,  // Transport/subscription unavailable. Accumulate and preserve pin.
    SYNCING,    // Backlog drain: new-record starts at least 100 ms apart, ACK-gated.
};
