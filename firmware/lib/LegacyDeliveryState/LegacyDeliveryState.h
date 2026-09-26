#pragma once

#include <atomic>
#include <stdint.h>

// =============================================================================
// LegacyDeliveryState
//
// A host-testable extraction of the current split in-flight ACK state. This is
// intentionally a behaviour-preserving seam, not the delivery-lease fix. In
// particular, reserving the next record sets `recordInFlight` before its
// identity is replaced by markPublished(), so the previous identity remains
// temporarily live. The native R0/R1 regression test captures that defect.
//
// Replace this class with the owned lease controller described in
// docs/delivery-lease-design.md after the regression is reproducible.
// =============================================================================

struct DeliveryIdentity {
    uint32_t boot_id;
    uint32_t sequence_id;
};

inline bool operator==(const DeliveryIdentity& lhs,
                       const DeliveryIdentity& rhs) {
    return lhs.boot_id == rhs.boot_id &&
           lhs.sequence_id == rhs.sequence_id;
}

enum class LegacyAckResult : uint8_t {
    COMMITTED,
    COMMIT_MISMATCH_CLEARED,
    STALE,
};

class LegacyDeliveryState {
public:
    bool tryReserve() {
        bool expected = false;
        return _recordInFlight.compare_exchange_strong(expected, true);
    }

    void markPublished(const DeliveryIdentity& identity, uint32_t sentAtMs) {
        _bootId.store(identity.boot_id);
        _sequenceId.store(identity.sequence_id);
        _sentAtMs.store(sentAtMs);
    }

    void release() {
        // Preserve the legacy ordering and stale-identity behaviour: current
        // firmware clears the flag/timer but never resets boot_id/sequence_id.
        _recordInFlight.store(false);
        _sentAtMs.store(0);
    }

    bool isInFlight() const { return _recordInFlight.load(); }
    uint32_t sentAtMs() const { return _sentAtMs.load(); }

    DeliveryIdentity identity() const {
        return {_bootId.load(), _sequenceId.load()};
    }

private:
    std::atomic<bool>     _recordInFlight{false};
    std::atomic<uint32_t> _bootId{0};
    std::atomic<uint32_t> _sequenceId{0};
    std::atomic<uint32_t> _sentAtMs{0};
};

// Extracts the current ACK transition, including its unsafe peek-then-pop
// ordering and its clearing of in-flight state after a head mismatch. PeekFn
// and PopFn make the transition executable in a native test without FreeRTOS,
// Arduino, or PSRAM. Production passes BufferManager adapters.
template <typename PeekFn, typename PopFn>
LegacyAckResult handleLegacyAck(LegacyDeliveryState& state,
                                const DeliveryIdentity& ack,
                                PeekFn peekOldest,
                                PopFn popOldest) {
    // Preserve the legacy load order: identity globals are read before the
    // in-flight flag is evaluated in connectionTask.
    const DeliveryIdentity tracked = state.identity();
    if (!state.isInFlight() || !(ack == tracked)) {
        return LegacyAckResult::STALE;
    }

    DeliveryIdentity oldest{};
    if (peekOldest(oldest) && oldest == ack) {
        DeliveryIdentity removed{};
        (void)popOldest(removed);  // Legacy code ignores pop()'s return value.
        state.release();
        return LegacyAckResult::COMMITTED;
    }

    // This is the current regression: a duplicate ACK matching the stale
    // identity clears a newer record's reservation when the ring head differs.
    state.release();
    return LegacyAckResult::COMMIT_MISMATCH_CLEARED;
}
