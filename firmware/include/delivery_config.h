#pragma once

#include <stdint.h>

// Single production source for firmware AND native tests. No hardware includes.
// New-record publish starts are spaced by at least 100 ms in either mode.
// NORMAL follows production (~2 Hz), rather than imposing another 500 ms timer.
// Retries use the independent lease ACK timeout / failure backoff below.
#define DELIVERY_MIN_PUBLISH_INTERVAL_MS 100
#define MQTT_ACK_TIMEOUT_MS           3000
#define DELIVERY_RAPID_ATTEMPTS       5
#define DELIVERY_RETRY_BACKOFF_MS     1000
#define DELIVERY_SLOW_RETRY_MS        30000
#define DELIVERY_PRESSURE_CHECK_MS    1000
#define ACK_SUBSCRIBE_RETRY_MS         1000
#define ACK_SUBSCRIBE_RETRY_MAX_MS     30000
#define DELIVERY_DIAGNOSTIC_MS         5000

struct LeaseConfig {
    uint32_t ack_timeout_ms = MQTT_ACK_TIMEOUT_MS;
    uint32_t rapid_attempts = DELIVERY_RAPID_ATTEMPTS;
    uint32_t retry_backoff_ms = DELIVERY_RETRY_BACKOFF_MS;
    uint32_t slow_retry_ms = DELIVERY_SLOW_RETRY_MS;
    uint32_t pressure_check_ms = DELIVERY_PRESSURE_CHECK_MS;
};

static_assert(DELIVERY_PRESSURE_CHECK_MS > 0 && DELIVERY_RAPID_ATTEMPTS > 0 && MQTT_ACK_TIMEOUT_MS > 0 &&
              DELIVERY_RETRY_BACKOFF_MS > 0 && DELIVERY_MIN_PUBLISH_INTERVAL_MS > 0 &&
              DELIVERY_SLOW_RETRY_MS >= DELIVERY_RETRY_BACKOFF_MS &&
              DELIVERY_SLOW_RETRY_MS >= MQTT_ACK_TIMEOUT_MS,
              "Delivery budgets must permit progress");
static_assert(ACK_SUBSCRIBE_RETRY_MS > 0 &&
              ACK_SUBSCRIBE_RETRY_MAX_MS >= ACK_SUBSCRIBE_RETRY_MS,
              "Invalid subscription retry backoff");
