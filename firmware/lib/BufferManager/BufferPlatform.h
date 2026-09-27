#pragma once

#include <stddef.h>

// Platform services only; the ring/pin algorithm is identical on host and ESP32.
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#elif defined(BUFFER_MANAGER_NATIVE)
#include <cstdlib>
#include <mutex>
#include <new>
#else
#error "BufferManager requires ESP32 or BUFFER_MANAGER_NATIVE"
#endif

namespace BufferPlatform {
#if defined(ARDUINO_ARCH_ESP32)
using Mutex = SemaphoreHandle_t;
inline Mutex createMutex() { return xSemaphoreCreateMutex(); }
inline void destroyMutex(Mutex mutex) { vSemaphoreDelete(mutex); }
inline void lock(Mutex mutex) { xSemaphoreTake(mutex, portMAX_DELAY); }
inline void unlock(Mutex mutex) { xSemaphoreGive(mutex); }
inline void* allocate(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}
inline void deallocate(void* ptr) { heap_caps_free(ptr); }
#else
using Mutex = std::mutex*;
inline Mutex createMutex() { return new (std::nothrow) std::mutex; }
inline void destroyMutex(Mutex mutex) { delete mutex; }
inline void lock(Mutex mutex) { mutex->lock(); }
inline void unlock(Mutex mutex) { mutex->unlock(); }
inline void* allocate(size_t bytes) { return std::malloc(bytes); }
inline void deallocate(void* ptr) { std::free(ptr); }
#endif

class LockGuard {
public:
    explicit LockGuard(Mutex mutex) : _mutex(mutex) { lock(_mutex); }
    ~LockGuard() { unlock(_mutex); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    Mutex _mutex;
};
}  // namespace BufferPlatform
