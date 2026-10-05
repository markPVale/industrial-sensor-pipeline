#pragma once

#include <DeliveryLease.h>

struct SubscriptionCounters {
    uint64_t attempts = 0;
    uint64_t failures = 0;
};

// Production orchestration seam: connection reconciliation, subscription setup,
// send pacing, catch-up, and diagnostic rate limiting. connectionTask is the
// sole owner; producer tasks only push. NORMAL is producer-paced, not governed
// by a second 2 Hz timer with no headroom for publishing/scheduling overhead.
template <typename BufferT = BufferManager>
class DeliveryRuntime {
public:
    DeliveryRuntime(BufferT& buffer, LossJournal& journal, LeaseConfig config = {})
        : _buffer(buffer), _lease(buffer, journal, config) {}

    const DeliveryLease<BufferT>& lease() const { return _lease; }
    NodeState mode() const { return _mode; }
    bool subscriptionSent() const { return _subscriptionSent; }
    const SubscriptionCounters& subscriptionCounters() const { return _subscriptions; }
    // Coalesce destinations within a connection-task iteration. This preserves
    // brief BUFFERING/SYNCING/NORMAL transitions without logging in callbacks.
    uint8_t takeModeChanges() {
        const uint8_t bits = _modeChanges;
        _modeChanges = 0;
        return bits;
    }

    // Called before potentially blocking reconnect work and from disconnect
    // callbacks. Current transport state, not a one-shot event, drives recovery.
    void observeTransport(uint32_t now, bool up) {
        account(now);
        if (!up) {
            _subscriptionSent = false;
            _subscribeRemaining = 0;
            _subscribeBackoff = ACK_SUBSCRIBE_RETRY_MS;
            setMode(NodeState::BUFFERING);
        }
        _transportUp = up;
        _lease.setConnected(now, up && _subscriptionSent);
    }

    // MQTT connect callback: a new clean session requires a new subscription,
    // even if an intermediate disconnect was not observed in the task loop.
    void newSession(uint32_t now) {
        observeTransport(now, false);
        _lease.onNewSession();
        observeTransport(now, true);
    }

    template <typename SubscribeFn, typename PublishFn, typename ClockFn>
    void step(uint32_t now, bool transportUp,
              const RecordIdentity* ack, SubscribeFn subscribe,
              PublishFn publish, ClockFn clock) {
        observeTransport(now, transportUp);
        if (_transportUp && !_subscriptionSent && !_subscribeRemaining) {
            ++_subscriptions.attempts;
            _subscriptionSent = subscribe();
            now = clock(); // exclude blocking setup from the readiness-time diagnostic
            account(now);
            if (!_subscriptionSent) {
                ++_subscriptions.failures;
                _subscribeRemaining = _subscribeBackoff;
                _subscribeBackoff = _subscribeBackoff > ACK_SUBSCRIBE_RETRY_MAX_MS / 2
                    ? ACK_SUBSCRIBE_RETRY_MAX_MS : _subscribeBackoff * 2;
            }
            _lease.setConnected(now, _subscriptionSent);
        }
        const bool ready = _transportUp && _subscriptionSent;
        if (ready) {
            if (_mode == NodeState::BUFFERING ||
                (_mode == NodeState::NORMAL && _buffer.available() > 1)) {
                setMode(NodeState::SYNCING);
            }
        }
        // One common start-to-start rate limit, independent of mode. Live data
        // naturally arrives at ~2 Hz; buffered data can catch up at <=10 Hz.
        // No stored ticks or burst of missed deadlines after blocking work.
        const bool eligible = ready && _sincePublish >= DELIVERY_MIN_PUBLISH_INTERVAL_MS;
        auto send = [&](const TelemetryRecord& record) {
            account(clock());
            _sincePublish = 0;
            const bool ok = publish(record);
            account(clock());
            return ok;
        };
        const auto result = _lease.step(now, ready, eligible, ack, send, clock);
        // ACK/pressure resolution comes first, but need not discard an eligible
        // send opportunity. No second ACK processing or continuation after errors.
        if ((result == LeaseResult::COMMITTED || result == LeaseResult::ABANDONED) && eligible)
            _lease.step(clock(), ready, true, nullptr, send, clock);

        if (ready && _mode == NodeState::SYNCING && _lease.idle() && _buffer.isEmpty()) {
            setMode(NodeState::NORMAL);
        }
    }

    // One summary per interval, even if an invariant fails every loop iteration.
    // Report cumulative counters, so suppressed individual warnings remain visible.
    bool reportDue(uint32_t now) {
        if (_reported && uint32_t(now - _lastReport) < DELIVERY_DIAGNOSTIC_MS) return false;
        _reported = true;
        _lastReport = now;
        return true;
    }

private:
    void setMode(NodeState mode) {
        if (mode != _mode) _modeChanges |= uint8_t(1U << static_cast<unsigned>(mode));
        _mode = mode;
    }
    void account(uint32_t now) {
        const uint32_t elapsed = now - _lastObserved;
        _lastObserved = now;
        _subscribeRemaining = elapsed >= _subscribeRemaining ? 0 : _subscribeRemaining - elapsed;
        const uint32_t room = UINT32_MAX - _sincePublish;
        _sincePublish += elapsed > room ? room : elapsed;
    }
    BufferT& _buffer;
    DeliveryLease<BufferT> _lease;
    SubscriptionCounters _subscriptions{};
    NodeState _mode = NodeState::BUFFERING;
    uint8_t _modeChanges = 0;
    bool _transportUp = false;
    bool _subscriptionSent = false; // request written, NOT broker SUBACK confirmation
    uint32_t _lastObserved = 0;
    uint32_t _subscribeRemaining = 0;
    uint32_t _subscribeBackoff = ACK_SUBSCRIBE_RETRY_MS;
    uint32_t _sincePublish = UINT32_MAX;
    bool _reported = false;
    uint32_t _lastReport = 0;
};
