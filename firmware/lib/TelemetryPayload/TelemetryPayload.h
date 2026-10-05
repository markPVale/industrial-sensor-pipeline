#pragma once

#include <types.h>
#include <math.h>
#include <stdio.h>

// No Arduino dependency or heap allocation. Normal values retain four decimal
// places; extreme finite floats use scientific notation instead of truncation.
inline bool buildPayload(const TelemetryRecord& rec, char* buf, size_t len) {
    if (!buf || !len) return false;
    buf[0] = '\0';
    const float values[] = {rec.accel_x, rec.accel_y, rec.accel_z,
                           rec.gyro_x, rec.gyro_y, rec.gyro_z, rec.window_rms};
    char numbers[7][24];
    for (size_t i = 0; i < 7; ++i) {
        int n = isfinite(values[i])
            ? snprintf(numbers[i], sizeof(numbers[i]), "%.4f", double(values[i]))
            : snprintf(numbers[i], sizeof(numbers[i]), "null");
        if (n >= int(sizeof(numbers[i])))
            n = snprintf(numbers[i], sizeof(numbers[i]), "%.9g", double(values[i]));
        if (n < 0 || n >= int(sizeof(numbers[i]))) return false;
    }
    const int n = snprintf(buf, len,
        "{\"boot\":%lu,\"seq\":%lu,\"ts\":%llu,"
        "\"ax\":%s,\"ay\":%s,\"az\":%s,\"gx\":%s,\"gy\":%s,\"gz\":%s,"
        "\"wrms\":%s,\"flags\":%u}",
        (unsigned long)rec.boot_id, (unsigned long)rec.sequence_id,
        (unsigned long long)rec.timestamp_ms,
        numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5], numbers[6],
        unsigned(rec.status_flags));
    if (n < 0 || size_t(n) >= len) { buf[0] = '\0'; return false; }
    return true;
}
