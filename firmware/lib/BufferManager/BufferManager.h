#pragma once

#include "BufferPlatform.h"
#include "types.h"

struct RecordIdentity {
    uint32_t boot_id;
    uint32_t sequence_id;
};

inline bool operator==(const RecordIdentity& a, const RecordIdentity& b) {
    return a.boot_id == b.boot_id && a.sequence_id == b.sequence_id;
}

enum class PushResult : uint8_t {
    OK, EVICTED_OLDEST, EVICTED_PROTECTED, REJECTED_NEWEST, NOT_INITIALISED
};

struct PushOutcome {
    PushResult result;
    bool record_lost;
    RecordIdentity lost;  // valid only when record_lost is true
};

// Fixed-capacity PSRAM ring. All runtime operations take the same mutex; never
// call from an ISR. begin()/end()/destruction require exclusive lifecycle access
// (no concurrent users). Call end() before destroying this non-copyable owner.
//
// Two producers on core 1: filterTask and sensorTask. connectionTask on core 0
// is the sole pop caller; its future lease controller will own acquire/commit/
// abandon. Until it lands, main still uses legacy peek/pop and never acquires
// a pin: see docs/delivery-lease-design.md §1.1 for the peek/push/pop race.
//
// One identity-based pin protects the oldest record until commit or counted
// abandonment. At capacity, push relocates it and evicts the second-oldest in
// O(1), or rejects the incoming record at capacity 1. Without a pin, drop oldest.
// No producer may release a pin. A returned loss identity is captured under the
// lock, ready for journaling by the caller outside the buffer lock.
class BufferManager {
public:
    BufferManager() = default;
    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    // Reject zero/overflowing capacity, repeat begin, or allocation failure.
    bool begin(size_t capacity);
    // End/clear refuse an active pin. Only commit/abandon may release one.
    // end() is idempotent; clear() returns false when uninitialised.
    bool end();
    bool clear();

    PushOutcome push(const TelemetryRecord& rec);
    bool peek(TelemetryRecord& rec);
    // Transitional legacy API: never removes a pinned record.
    bool pop(TelemetryRecord& rec);

    // Acquire and copy oldest in one lock. Empty/uninitialised/already pinned
    // returns false without changing the output or existing pin.
    bool acquirePin(TelemetryRecord& out);
    // Validate both pin identity AND current oldest identity before removing.
    // Failure changes nothing. Commit does not count as a drop/abandonment.
    bool commitPinnedHead(uint32_t boot_id, uint32_t seq);
    bool abandonPinnedHead(uint32_t boot_id, uint32_t seq,
                           RecordIdentity& abandoned);

    size_t available();
    size_t freeSpace();
    size_t capacity();
    bool isEmpty();
    bool isFull();
    size_t droppedCount();  // evictions only (legacy meaning)
    BufferStats getStats(); // count and all counters from one locked snapshot

private:
    static RecordIdentity identityOf(const TelemetryRecord& rec) {
        return {rec.boot_id, rec.sequence_id};
    }
    bool pinnedHeadMatches(RecordIdentity expected) const; // caller holds lock
    void removeOldest(); // caller holds lock
    void resetState();   // caller holds lock, or exclusive lifecycle access

    TelemetryRecord* _buffer = nullptr;
    size_t _writeIndex = 0;  // next write slot
    size_t _readIndex = 0;   // oldest record
    size_t _capacity = 0;
    size_t _count = 0;
    size_t _evictedOldest = 0;
    size_t _protectedEvictions = 0;
    size_t _rejectedNewest = 0;
    size_t _abandoned = 0;   // local release decisions, not proven delivery loss
    bool _hasPinnedHead = false;
    RecordIdentity _pinnedIdentity{};
    BufferPlatform::Mutex _mutex = nullptr;
};
