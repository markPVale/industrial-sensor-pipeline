#pragma once

#include <BufferManager.h>
#include <LossJournal.h>
#include <delivery_config.h>

enum class LeasePhase : uint8_t { IDLE, AWAITING_ACK, RETRY_PENDING };
enum class LeaseResult : uint8_t {
    NONE, PUBLISHED, PUBLISH_FAILED, COMMITTED, ABANDONED, INTERNAL_ERROR
};
struct LeaseState {
    LeasePhase phase = LeasePhase::IDLE;
    RecordIdentity identity{};
    uint32_t publish_calls = 0;
    uint32_t successful_publishes = 0;
    uint32_t connected_wait_ms = 0; // observed readiness diagnostic ONLY, never abandonment authority
    uint32_t sent_at_ms = 0;
    bool everPublished() const { return successful_publishes != 0; }
};
struct LeaseCounters {
    uint64_t ack_stale = 0;
    uint64_t ack_mismatch = 0;
    uint64_t lease_internal_errors = 0;
    uint64_t publish_calls = 0;
    uint64_t publish_failures = 0;
    uint64_t retries = 0;
    uint64_t ack_timeouts = 0;
    uint64_t committed = 0;
    uint64_t abandoned = 0;
};

// Single writer: connectionTask, including its synchronous MQTT callbacks.
// Never call from producer tasks. Buffer owns the pin; this owner cannot release
// it except through a successful identity-checked commit/abandon operation.
// BufferT permits failure injection in tests; production uses real BufferManager.
template <typename BufferT = BufferManager>
class DeliveryLease {
public:
    DeliveryLease(BufferT& buffer, LossJournal& journal, LeaseConfig config = {})
        : _buffer(buffer), _journal(journal), _config(config) {}
    DeliveryLease(const DeliveryLease&) = delete;
    DeliveryLease& operator=(const DeliveryLease&) = delete;

    const LeaseState& state() const { return _state; }
    const LeaseCounters& counters() const { return _counters; }
    bool idle() const { return _state.phase == LeasePhase::IDLE; }
    bool ownershipFault() const { return _ownershipFault; }

    // Only the actual MQTT new-session callback grants this opportunity. A bit,
    // not an accumulating allowance; repeated sessions cannot reset backoff or
    // counters. Staged matching ACKs may still commit before the fresh attempt.
    void onNewSession() {
        if (!idle()) _freshAttemptRequired = true;
    }

    // Call at observed connection edges, before blocking reconnect work. Time
    // since the previous observation belongs to the previous connected state.
    // Service at least once per millis wrap. Readiness can be stale; this clock
    // is diagnostic and is NOT evidence that an application ACK was possible.
    void setConnected(uint32_t now, bool connected) {
        account(now);
        _connected = connected;
        if (!connected && _state.phase == LeasePhase::AWAITING_ACK) {
            _state.phase = LeasePhase::RETRY_PENDING;
            _attemptResolved = false;
            // Preserve retry pacing. An interrupted ACK wait cannot authorize
            // abandonment, even if this MQTT session survives link recovery.
        }
    }

    // No time/publish side effects. Eligibility follows this identity's publish
    // history, not its phase. Useful also for a late ACK in RETRY_PENDING.
    LeaseResult onAck(RecordIdentity ack) {
        if (idle()) { ++_counters.ack_stale; return LeaseResult::NONE; }
        if (!(_state.identity == ack)) {
            ++_counters.ack_mismatch;
            return LeaseResult::NONE;
        }
        if (!_state.everPublished()) { ++_counters.ack_stale; return LeaseResult::NONE; }
        if (!_buffer.commitPinnedHead(ack.boot_id, ack.sequence_id)) return internalError();
        ++_counters.committed;
        reset();
        return LeaseResult::COMMITTED;
    }

    // Ordering: account connection -> ACK -> timeout/abandonment -> publish.
    // PublishFn builds/sends synchronously from the owned copy. ClockFn samples
    // AFTER that potentially blocking call, so ACK timeout starts on return.
    // No telemetry QUEUED phase, retained pointer into the ring, or generation.
    template <typename PublishFn, typename ClockFn>
    LeaseResult step(uint32_t now, bool connected, bool deliveryDue,
                     const RecordIdentity* ack, PublishFn publish, ClockFn clock) {
        setConnected(now, connected);
        if (ack) {
            const auto result = onAck(*ack);
            if (result != LeaseResult::NONE) return result;
        }
        if (!_connected) return LeaseResult::NONE;
        // A failed ownership check is not equivalent to NOT_FULL. Keep retries
        // blocked even on iterations that skip the rate-limited validation.
        // ACK commits above still validate atomically and may recover directly.
        if (_ownershipFault) {
            if (_pressureCheckRemaining) return LeaseResult::NONE;
            if (!_buffer.validatePinnedHead(_state.identity.boot_id, _state.identity.sequence_id))
                return internalError();
            _ownershipFault = false;
            // Only connectionTask can release this validated pin; producers
            // may relocate it but cannot invalidate ownership before publish.
        }
        if (_state.phase == LeasePhase::AWAITING_ACK &&
            uint32_t(now - _state.sent_at_ms) >= _config.ack_timeout_ms) {
            ++_counters.ack_timeouts;
            _state.phase = LeasePhase::RETRY_PENDING;
            _attemptResolved = true;
        }
        if (_state.phase == LeasePhase::RETRY_PENDING &&
            _state.publish_calls >= _config.rapid_attempts &&
            _attemptResolved && !_freshAttemptRequired && !_pressureCheckRemaining) {
            RecordIdentity abandoned{};
            _pressureCheckRemaining = _config.pressure_check_ms;
            const auto result = _buffer.abandonPinnedHeadIfFull(_state.identity.boot_id,
                                                               _state.identity.sequence_id, abandoned);
            if (result == AbandonResult::INVALID_PIN) return internalError();
            if (result == AbandonResult::ABANDONED) {
                LeaseAbandonment outcome{};
                outcome.identity = abandoned;
                outcome.publish_calls = _state.publish_calls;
                outcome.successful_publishes = _state.successful_publishes;
                outcome.connected_wait_ms = _state.connected_wait_ms;
                if (!_journal.observeAbandonment(outcome)) ++_counters.lease_internal_errors;
                ++_counters.abandoned;
                reset();
                return LeaseResult::ABANDONED;
            }
            // NOT_FULL: retain the pin and keep probing. Attempt exhaustion is
            // a slow-retry threshold, never a terminal delivery state.
        }
        if (idle()) {
            if (!deliveryDue || !_buffer.acquirePin(_record)) return LeaseResult::NONE;
            _state = {};
            _state.identity = {_record.boot_id, _record.sequence_id};
            _state.phase = LeasePhase::RETRY_PENDING;
            _failureBackoff = _config.retry_backoff_ms;
            _retryRemaining = 0;
            _freshAttemptRequired = true;
            _attemptResolved = false;
        }
        if (_state.phase != LeasePhase::RETRY_PENDING || _retryRemaining) return LeaseResult::NONE;

        if (_state.publish_calls) ++_counters.retries;
        ++_counters.publish_calls;
        if (_state.publish_calls != UINT32_MAX) ++_state.publish_calls;
        _freshAttemptRequired = false;
        _attemptResolved = false;
        _pressureCheckRemaining = 0; // inspect promptly once this attempt resolves
        const bool published = publish(_record);
        const uint32_t finished = clock();
        account(finished);
        const bool slow = _state.publish_calls >= _config.rapid_attempts;
        // Cooldown runs alongside the ACK timer and through disconnects. A new
        // session makes one attempt eligible, never immediate or unpaced.
        _retryRemaining = slow ? _config.slow_retry_ms
                               : (published ? _config.retry_backoff_ms : _failureBackoff);
        if (published) {
            if (_state.successful_publishes != UINT32_MAX) ++_state.successful_publishes;
            _state.sent_at_ms = finished;
            _state.phase = LeasePhase::AWAITING_ACK;
            return LeaseResult::PUBLISHED;
        }
        _state.phase = LeasePhase::RETRY_PENDING;
        _attemptResolved = true; // local failure is completed, no ACK window to wait out
        ++_counters.publish_failures;
        _failureBackoff = _failureBackoff > _config.slow_retry_ms / 2
            ? _config.slow_retry_ms : _failureBackoff * 2;
        return LeaseResult::PUBLISH_FAILED;
    }

private:
    void account(uint32_t now) {
        const uint32_t elapsed = now - _lastObserved;
        _lastObserved = now;
        if (idle()) return;
        if (_connected) {
            const uint32_t room = UINT32_MAX - _state.connected_wait_ms;
            _state.connected_wait_ms += elapsed > room ? room : elapsed;
        }
        // Count down rather than comparing a stale absolute deadline after a
        // multi-week outage. Backoff elapses during disconnect, but never resets.
        _retryRemaining = elapsed >= _retryRemaining ? 0 : _retryRemaining - elapsed;
        _pressureCheckRemaining = elapsed >= _pressureCheckRemaining
            ? 0 : _pressureCheckRemaining - elapsed;
    }
    LeaseResult internalError() {
        _ownershipFault = true;
        _pressureCheckRemaining = _config.pressure_check_ms;
        ++_counters.lease_internal_errors;
        return LeaseResult::INTERNAL_ERROR;
    }
    void reset() {
        _state = {}; _record = {}; _retryRemaining = 0; _pressureCheckRemaining = 0;
        _freshAttemptRequired = false; _attemptResolved = false;
        _ownershipFault = false;
    }
    BufferT& _buffer;
    LossJournal& _journal;
    LeaseConfig _config;
    LeaseState _state{};
    LeaseCounters _counters{};
    TelemetryRecord _record{};
    bool _connected = false;
    uint32_t _lastObserved = 0;
    uint32_t _retryRemaining = 0;
    uint32_t _pressureCheckRemaining = 0;
    uint32_t _failureBackoff = 0;
    bool _freshAttemptRequired = false;
    bool _attemptResolved = false;
    bool _ownershipFault = false;
};
