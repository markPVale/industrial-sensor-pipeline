#include "BufferManager.h"

bool BufferManager::begin(size_t capacity) {
    if (_buffer || capacity == 0 || capacity > SIZE_MAX / sizeof(TelemetryRecord)) {
        return false;
    }
    auto* buffer = static_cast<TelemetryRecord*>(
        BufferPlatform::allocate(capacity * sizeof(TelemetryRecord)));
    if (!buffer) return false;
    auto mutex = BufferPlatform::createMutex();
    if (!mutex) {
        BufferPlatform::deallocate(buffer);
        return false;
    }
    _buffer = buffer;
    _mutex = mutex;
    _capacity = capacity;
    resetState();
    return true;
}

bool BufferManager::end() {
    // Exclusive lifecycle access is required so nobody can wait on a mutex
    // that is about to be destroyed. No implicit pin release, even at teardown.
    if (_hasPinnedHead) return false;
    BufferPlatform::deallocate(_buffer);
    _buffer = nullptr;
    if (_mutex) BufferPlatform::destroyMutex(_mutex);
    _mutex = nullptr;
    _capacity = 0;
    resetState();
    return true;
}

void BufferManager::resetState() {
    _writeIndex = _readIndex = _count = 0;
    _evictedOldest = _protectedEvictions = _rejectedNewest = _abandoned = 0;
    _hasPinnedHead = false;
    _pinnedIdentity = {};
}

bool BufferManager::clear() {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (_hasPinnedHead) return false;
    resetState();
    return true;
}

PushOutcome BufferManager::push(const TelemetryRecord& rec) {
    if (!_mutex) return {PushResult::NOT_INITIALISED, true, identityOf(rec)};
    BufferPlatform::LockGuard lock(_mutex);
    PushOutcome outcome{PushResult::OK, false, {}};
    if (_count == _capacity) {
        if (_hasPinnedHead && _capacity == 1) {
            ++_rejectedNewest;
            return {PushResult::REJECTED_NEWEST, true, identityOf(rec)};
        }
        if (_hasPinnedHead) {
            const size_t second = (_readIndex + 1) % _capacity;
            outcome = {PushResult::EVICTED_PROTECTED, true, identityOf(_buffer[second])};
            _buffer[second] = _buffer[_readIndex];
            ++_protectedEvictions;
        } else {
            outcome = {PushResult::EVICTED_OLDEST, true, identityOf(_buffer[_readIndex])};
            ++_evictedOldest;
        }
        removeOldest(); // pinned record, if any, has moved to the new read slot
    }
    _buffer[_writeIndex] = rec;
    _writeIndex = (_writeIndex + 1) % _capacity;
    ++_count;
    return outcome;
}

void BufferManager::removeOldest() {
    _readIndex = (_readIndex + 1) % _capacity;
    --_count;
}

bool BufferManager::pop(TelemetryRecord& rec) {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (!_count || _hasPinnedHead) return false;
    rec = _buffer[_readIndex];
    removeOldest();
    return true;
}

bool BufferManager::peek(TelemetryRecord& rec) {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (!_count) return false;
    rec = _buffer[_readIndex];
    return true;
}

bool BufferManager::acquirePin(TelemetryRecord& out) {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (!_count || _hasPinnedHead) return false;
    out = _buffer[_readIndex];
    _pinnedIdentity = identityOf(out);
    _hasPinnedHead = true;
    return true;
}

bool BufferManager::pinnedHeadMatches(RecordIdentity expected) const {
    return _hasPinnedHead && _count && _pinnedIdentity == expected &&
           identityOf(_buffer[_readIndex]) == expected;
}

bool BufferManager::commitPinnedHead(uint32_t boot_id, uint32_t seq) {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (!pinnedHeadMatches({boot_id, seq})) return false;
    removeOldest();
    _hasPinnedHead = false;
    _pinnedIdentity = {};
    return true;
}

bool BufferManager::abandonPinnedHead(uint32_t boot_id, uint32_t seq,
                                      RecordIdentity& abandoned) {
    if (!_mutex) return false;
    BufferPlatform::LockGuard lock(_mutex);
    if (!pinnedHeadMatches({boot_id, seq})) return false;
    abandoned = identityOf(_buffer[_readIndex]);
    removeOldest();
    ++_abandoned;
    _hasPinnedHead = false;
    _pinnedIdentity = {};
    return true;
}

BufferStats BufferManager::getStats() {
    if (!_mutex) return {};
    BufferPlatform::LockGuard lock(_mutex);
    return {_capacity, _count, _capacity - _count,
            _evictedOldest + _protectedEvictions, _evictedOldest,
            _protectedEvictions, _rejectedNewest, _abandoned};
}

size_t BufferManager::available() { return getStats().count; }
size_t BufferManager::freeSpace() { return getStats().free_space; }
size_t BufferManager::capacity() { return getStats().capacity; }
size_t BufferManager::droppedCount() { return getStats().dropped; }
bool BufferManager::isEmpty() { return available() == 0; }
bool BufferManager::isFull() {
    const BufferStats stats = getStats();
    return stats.capacity != 0 && stats.count == stats.capacity;
}
