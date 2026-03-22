// =============================================================================
// BufferManager.cpp
// =============================================================================

#include "BufferManager.h"
#include <esp_heap_caps.h>

// =============================================================================
// Lifecycle
// =============================================================================

bool BufferManager::begin(size_t capacity) {
    if (_buffer != nullptr) {
        // Already initialised. Caller must call end() before reinitialising.
        return false;
    }

    TelemetryRecord* new_buffer = static_cast<TelemetryRecord*>(
        heap_caps_malloc(capacity * sizeof(TelemetryRecord), MALLOC_CAP_SPIRAM)
    );
    if (!new_buffer) {
        return false;
    }

    SemaphoreHandle_t new_mutex = xSemaphoreCreateMutex();
    if (!new_mutex) {
        heap_caps_free(new_buffer);
        return false;
    }

    // Only commit state once both resources are confirmed good.
    _buffer   = new_buffer;
    _mutex    = new_mutex;
    _capacity = capacity;
    _head     = 0;
    _tail     = 0;
    _count    = 0;
    _dropped  = 0;

    return true;
}

void BufferManager::end() {
    if (_mutex) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
    }

    if (_buffer) {
        heap_caps_free(_buffer);
        _buffer = nullptr;
    }

    _capacity = 0;
    _head     = 0;
    _tail     = 0;
    _count    = 0;
    _dropped  = 0;

    if (_mutex) {
        xSemaphoreGive(_mutex);
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

void BufferManager::clear() {
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _head    = 0;
    _tail    = 0;
    _count   = 0;
    _dropped = 0;
    xSemaphoreGive(_mutex);
}

// =============================================================================
// Write / Read
// =============================================================================

bool BufferManager::push(const TelemetryRecord& rec) {
    if (!_mutex || !_buffer) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_count == _capacity) {
        // Buffer full: evict oldest record so the newest data is preserved.
        _tail = (_tail + 1) % _capacity;
        _dropped++;
        _count--;
    }

    _buffer[_head] = rec;
    _head = (_head + 1) % _capacity;
    _count++;

    xSemaphoreGive(_mutex);
    return true;
}

bool BufferManager::pop(TelemetryRecord& rec) {
    if (!_mutex || !_buffer) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_count == 0) {
        xSemaphoreGive(_mutex);
        return false;
    }

    rec   = _buffer[_tail];
    _tail = (_tail + 1) % _capacity;
    _count--;

    xSemaphoreGive(_mutex);
    return true;
}

bool BufferManager::peek(TelemetryRecord& rec) {
    if (!_mutex || !_buffer) return false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_count == 0) {
        xSemaphoreGive(_mutex);
        return false;
    }

    rec = _buffer[_tail];

    xSemaphoreGive(_mutex);
    return true;
}

// =============================================================================
// Inspection
// =============================================================================

size_t BufferManager::available() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    size_t n = _count;
    xSemaphoreGive(_mutex);
    return n;
}

size_t BufferManager::freeSpace() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    size_t n = _capacity - _count;
    xSemaphoreGive(_mutex);
    return n;
}

size_t BufferManager::capacity() {
    // _capacity is immutable after begin() — no lock needed.
    return _capacity;
}

bool BufferManager::isEmpty() {
    if (!_mutex) return true;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool empty = (_count == 0);
    xSemaphoreGive(_mutex);
    return empty;
}

bool BufferManager::isFull() {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool full = (_count == _capacity);
    xSemaphoreGive(_mutex);
    return full;
}

size_t BufferManager::droppedCount() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    size_t n = _dropped;
    xSemaphoreGive(_mutex);
    return n;
}

BufferStats BufferManager::getStats() {
    BufferStats stats = {};
    if (!_mutex) return stats;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    stats.capacity   = _capacity;
    stats.count      = _count;
    stats.free_space = _capacity - _count;
    stats.dropped    = _dropped;
    xSemaphoreGive(_mutex);

    return stats;
}
