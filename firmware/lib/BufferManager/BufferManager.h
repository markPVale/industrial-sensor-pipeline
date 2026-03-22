#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "types.h"

// =============================================================================
// BufferManager — PSRAM-backed fixed-capacity ring buffer
//
// Concurrency contract
// --------------------
//   - Task-safe: all public methods are protected by a FreeRTOS mutex.
//   - NOT ISR-safe: never call from an interrupt context.
//   - Intended for exactly one producer (sensorTask) and one consumer
//     (syncTask). Works correctly with N producers/consumers but the
//     single-producer/single-consumer assumption is how the system is wired.
//
// Overflow policy
// ---------------
//   When the buffer is full, push() drops the oldest unread record (advances
//   _tail), increments _dropped, then writes the new record. This preserves
//   the most-recent data — correct for industrial telemetry where a stale
//   reading is less useful than a fresh one. FIFO ordering is maintained
//   among all retained records.
//
// Lifecycle
// ---------
//   1. Construct (zero-initialised by default member values).
//   2. Call begin(capacity) — allocates PSRAM and mutex.
//      Returns false and performs no allocation if already initialised,
//      PSRAM is unavailable, or mutex creation fails.
//   3. Use push() / pop() / peek() from tasks.
//   4. Call end() to release all resources. Safe to call multiple times.
//   5. Call begin() again to reinitialise if needed (e.g. after a config
//      change that requires a different capacity).
// =============================================================================
class BufferManager {
public:
    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    // Allocate a PSRAM-backed ring buffer of `capacity` TelemetryRecords.
    // Returns true on success. Returns false without allocating anything if:
    //   - already initialised (call end() first)
    //   - PSRAM heap_caps_malloc fails
    //   - FreeRTOS mutex creation fails (buffer memory is freed in this case)
    bool begin(size_t capacity);

    // Release the PSRAM buffer and mutex. Resets all counters.
    // Safe to call before begin() or after end().
    void end();

    // Reset head, tail, count, and dropped to zero without releasing memory.
    // Existing records are logically discarded (not zeroed).
    // No-op if not initialised.
    void clear();

    // -------------------------------------------------------------------------
    // Write / Read
    // -------------------------------------------------------------------------

    // Copy `rec` into the buffer. If full, the oldest record is evicted and
    // _dropped is incremented before writing. Always returns true if
    // initialised; returns false only if begin() was never called.
    bool push(const TelemetryRecord& rec);

    // Copy the oldest record into `rec` and remove it from the buffer.
    // Returns false if the buffer is empty or not initialised.
    bool pop(TelemetryRecord& rec);

    // Copy the oldest record into `rec` without removing it.
    // Returns false if the buffer is empty or not initialised.
    bool peek(TelemetryRecord& rec);

    // -------------------------------------------------------------------------
    // Inspection — all take the mutex internally
    // -------------------------------------------------------------------------

    size_t available();      // number of records ready to read
    size_t freeSpace();      // number of additional records that can be pushed
    size_t capacity();       // total record slots (immutable after begin())

    bool isEmpty();
    bool isFull();

    size_t droppedCount();   // records evicted due to overflow since last clear()

    // Snapshot of all stats in one mutex acquisition — prefer this over
    // calling the individual methods separately when you need multiple values.
    BufferStats getStats();

private:
    TelemetryRecord*  _buffer   = nullptr;
    size_t            _head     = 0;   // index of next write slot
    size_t            _tail     = 0;   // index of next read slot
    size_t            _capacity = 0;
    size_t            _count    = 0;   // records currently held
    size_t            _dropped  = 0;   // cumulative eviction count
    SemaphoreHandle_t _mutex    = nullptr;
};
