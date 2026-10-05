#include "LossJournal.h"

LossJournal::~LossJournal() {
    if (_mutex) BufferPlatform::destroyMutex(_mutex);
}

bool LossJournal::begin(size_t capacity, uint32_t maxSpan, uint32_t intervalMs) {
    if (_mutex || !capacity || capacity > kMaxEvents || !maxSpan || !intervalMs ||
        intervalMs >= 0x80000000U) return false;
    _mutex = BufferPlatform::createMutex();
    if (!_mutex) return false;
    _capacity = capacity;
    _maxSpan = maxSpan;
    _intervalMs = intervalMs;
    return true;
}

bool LossJournal::expired(uint32_t nowMs) const {
    const uint32_t elapsed = nowMs - _openedMs;
    // Handles millis wrap and timestamps captured before a competing caller
    // acquired the mutex. The owner must service expiry within half a wrap.
    return _stats.range_open && elapsed < 0x80000000U && elapsed >= _intervalMs;
}

void LossJournal::enqueue(const JournalEvent& event) {
    if (_stats.queued == _capacity) {
        ++_stats.diagnostic_events_lost;
        return;
    }
    _events[(_read + _stats.queued) % _capacity] = event;
    ++_stats.queued;
}

void LossJournal::closeRange() {
    if (!_stats.range_open) return;
    JournalEvent event{};
    event.kind = JournalEventKind::LOSS;
    event.loss = _open;
    enqueue(event);
    _stats.range_open = false;
}

bool LossJournal::observePush(const PushOutcome& outcome, uint32_t nowMs) {
    if (!_mutex) return false;
    LossPolicy policy{};
    switch (outcome.result) {
        case PushResult::OK: return !outcome.record_lost;
        case PushResult::EVICTED_OLDEST: policy = LossPolicy::EVICTED_OLDEST; break;
        case PushResult::EVICTED_PROTECTED: policy = LossPolicy::EVICTED_PROTECTED; break;
        case PushResult::REJECTED_NEWEST: policy = LossPolicy::REJECTED_NEWEST; break;
        case PushResult::NOT_INITIALISED: policy = LossPolicy::BUFFER_UNAVAILABLE; break;
        default: return false;
    }
    if (!outcome.record_lost) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (policy == LossPolicy::BUFFER_UNAVAILABLE) ++_stats.buffer_unavailable;
    if (_stats.range_open &&
        (expired(nowMs) || _open.policy != policy || _open.boot_id != outcome.lost.boot_id ||
         outcome.lost.sequence_id != uint32_t(_open.last_seq + 1U))) closeRange();
    if (!_stats.range_open) {
        _open = {};
        _open.policy = policy;
        _open.boot_id = outcome.lost.boot_id;
        _open.first_seq = outcome.lost.sequence_id;
        _openedMs = nowMs;
        _stats.range_open = true;
    }
    _open.last_seq = outcome.lost.sequence_id;
    ++_open.count;
    if (_open.count >= _maxSpan) closeRange();
    return true;
}

bool LossJournal::observeAbandonment(const LeaseAbandonment& abandonment) {
    if (!_mutex || abandonment.successful_publishes > abandonment.publish_calls) return false;
    BufferPlatform::LockGuard lock(_mutex);
    closeRange(); // preserve observation order across event kinds
    JournalEvent event{};
    event.kind = JournalEventKind::LEASE_ABANDONMENT;
    event.abandonment = abandonment;
    if (abandonment.successful_publishes) {
        event.reason = AbandonmentReason::BUFFER_PRESSURE_ACK_EXHAUSTED;
        event.delivery_outcome = DeliveryOutcome::UNKNOWN_AFTER_PUBLISH;
        ++_stats.lease_abandoned_after_publish;
    } else {
        event.reason = AbandonmentReason::BUFFER_PRESSURE_PUBLISH_FAILURE;
        event.delivery_outcome = DeliveryOutcome::NOT_PUBLISHED;
        ++_stats.lease_abandoned_unpublished;
    }
    enqueue(event);
    return true;
}

void LossJournal::closeExpired(uint32_t nowMs) {
    if (!_mutex) return;
    BufferPlatform::LockGuard lock(_mutex);
    if (expired(nowMs)) closeRange();
}

void LossJournal::flush() {
    if (!_mutex) return;
    BufferPlatform::LockGuard lock(_mutex);
    closeRange();
}

bool LossJournal::pop(JournalEvent& out) {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (!_stats.queued) return false;
    out = _events[_read];
    _read = (_read + 1) % _capacity;
    --_stats.queued;
    return true;
}

JournalStats LossJournal::getStats() {
    if (!_mutex) return {};
    BufferPlatform::LockGuard lock(_mutex);
    return _stats;
}
