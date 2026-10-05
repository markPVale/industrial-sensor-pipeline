// =============================================================================
// main.cpp — ESP32-S3 N16R8 Industrial Sensor Node
// =============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
#include <atomic>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>

#include "config.h"
#include "types.h"
#include "KalmanFilter.h"
#include "BufferManager.h"
#include "LossJournal.h"
#include "MqttManager.h"
#include "DeliveryRuntime.h"
#include "TelemetryPayload.h"

// PubSubClient stores the full MQTT packet in one shared transmit/receive
// buffer. Keep its capacity tied to the largest declared payload and topic,
// including the packet identifier carried by an inbound QoS 1 ACK PUBLISH,
// rather than relying on PubSubClient's smaller default MQTT_MAX_PACKET_SIZE.
static constexpr size_t kMqttTopicLengthBytes = 2;
static constexpr size_t kMqttQos1PacketIdBytes = 2;
static constexpr size_t kMqttTelemetryTopicLength = sizeof(MQTT_TOPIC_TELEMETRY) - 1;
static constexpr size_t kMqttEstopTopicLength = sizeof(MQTT_TOPIC_ESTOP) - 1;
static constexpr size_t kMqttAckTopicLength = sizeof(MQTT_TOPIC_ACK) - 1;
static constexpr size_t kMqttLongestTopicLength =
    kMqttTelemetryTopicLength > kMqttEstopTopicLength
        ? (kMqttTelemetryTopicLength > kMqttAckTopicLength
               ? kMqttTelemetryTopicLength : kMqttAckTopicLength)
        : (kMqttEstopTopicLength > kMqttAckTopicLength
               ? kMqttEstopTopicLength : kMqttAckTopicLength);
static constexpr size_t kMqttRequiredBufferSize =
    MQTT_MAX_HEADER_SIZE + kMqttTopicLengthBytes +
    kMqttLongestTopicLength + kMqttQos1PacketIdBytes + MQTT_PAYLOAD_SIZE;

static_assert(MQTT_CLIENT_BUFFER_SIZE >= kMqttRequiredBufferSize,
              "MQTT_CLIENT_BUFFER_SIZE is too small for the configured QoS 1 packet");

// -----------------------------------------------------------------------------
// RawSample — passed from sensorTask to filterTask via g_sensorQueue.
// Internal to this file only.
// -----------------------------------------------------------------------------
struct RawSample {
    uint64_t timestamp_ms;
    float    accel_x;   // m/s²
    float    accel_y;
    float    accel_z;
    float    gyro_x;    // rad/s
    float    gyro_y;
    float    gyro_z;
    uint8_t  status_flags;
};

// -----------------------------------------------------------------------------
// MqttMessage — non-leased messages (E-Stop), drained by connectionTask.
// This is the only mechanism for other tasks to write to the MQTT socket.
// MqttManager is NOT thread-safe; all publish calls happen in connectionTask.
// -----------------------------------------------------------------------------
struct MqttMessage {
    static constexpr size_t kTopicCapacity = 64;

    char topic[kTopicCapacity];
    char payload[MQTT_PAYLOAD_SIZE];
    bool retained;
};

static_assert(kMqttLongestTopicLength < MqttMessage::kTopicCapacity,
              "MqttMessage::topic is too small for a configured MQTT topic");

// -----------------------------------------------------------------------------
// Hardware thresholds — derived from ranges configured in initMPU6050()
//   Accel ±8g  → 8 × 9.81 = 78.48 m/s²
//   Gyro  ±500 deg/s → 500 × π/180 = 8.727 rad/s
// -----------------------------------------------------------------------------
static constexpr float kAccelClipThreshold  = 78.48f;
static constexpr float kGyroClipThreshold   =  8.727f;

// Anomaly: RMS of accel magnitude over FILTER_WINDOW_SIZE samples (m/s²).
// At-rest RMS measured on hardware: 10.24–10.27 m/s² (gravity + calibration offset).
// Threshold set 2.2 m/s² (0.22g) above at-rest ceiling to suppress false positives.
static constexpr float kAnomalyRmsThreshold = 12.5f;

// Spike rejection: 0.5g in m/s² for accel axes.
// Gyro threshold left at 0 (disabled) until noise profile is measured.
static constexpr float kAccelSpikeThreshold = 4.905f;

// Boot ID — loaded from NVS in initBootId(), incremented each boot.
// Initialised to 0; set before any task uses it.
static uint32_t g_bootId = 0;

// Fault-reboot counter — loaded from NVS on boot, written only on state
// transitions (before fault reboot, after sustained recovery, on power-on clear).
// NVS key: "sensor"/"fault_reboots". Cleared on power-on reset so the counter
// is per-power-on-session, matching the original RTC_DATA_ATTR intent.
static uint8_t g_faultRebootCount = 0;

// Sequence ID — monotonic counter shared by filterTask (normal records) and
// sensorTask (fault records). Global so fault records stay in sequence with
// normal telemetry. Resets to 0 each boot.
static std::atomic<uint32_t> g_sequenceId{0};

// -----------------------------------------------------------------------------
// Accelerometer calibration — loaded from NVS in loadCalibration().
// Correction: corrected = (raw - offset) * scale
// Defaults are the values measured during 6-point bench calibration.
// NVS keys (namespace "sensor"): cal_ax_off, cal_ax_scl, etc.
// -----------------------------------------------------------------------------
struct CalibrationData {
    float ax_offset =  0.591f;  float ax_scale = 0.9961f;
    float ay_offset = -0.501f;  float ay_scale = 0.9930f;
    float az_offset = -1.436f;  float az_scale = 0.9866f;
};
static CalibrationData g_cal;
static void loadCalibration();  // defined below initMPU6050()

// ACK staging is owned by connectionTask, including its synchronous callback.
// The independent ISR-driven safety event group below remains necessary.
static bool g_ackPending = false;

// -----------------------------------------------------------------------------
// Safety interlock
//
// safetyISR fires on GPIO 10 falling edge (optical loop opens) and sets
// kBitInterlock in g_safetyEvents. safetyTask blocks on that bit and handles
// the latch + MQTT publish. g_interlockActive is the persistent latch: once
// true, filterTask stamps STATUS_INTERLOCK_OPEN on every subsequent record
// until the node reboots.
// -----------------------------------------------------------------------------
static EventGroupHandle_t    g_safetyEvents   = nullptr;
static constexpr EventBits_t kBitInterlock    = BIT0;
static std::atomic<bool>     g_interlockActive{false};

// -----------------------------------------------------------------------------
// NTP time sync
//
// configTime() in setup() starts the ESP-IDF SNTP client, which syncs
// automatically and re-syncs periodically in the background. We read the
// system clock directly via gettimeofday() — no manual state needed.
//
// Pre-sync fallback: tv_sec is near 0 until SNTP sets the clock. The guard
// tv_sec > 1700000000 (~Nov 2023) distinguishes a real UTC time from the
// default post-boot value, and falls back to millis() in that window.
// -----------------------------------------------------------------------------
static uint64_t getEpochMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec > 1700000000L) {
        return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
    }
    return (uint64_t)millis();
}

// -----------------------------------------------------------------------------
// Shared globals
// -----------------------------------------------------------------------------
static Adafruit_MPU6050 g_mpu;
static BufferManager    g_buffer;
static LossJournal      g_lossJournal;
static MqttManager      g_mqttManager;
static QueueHandle_t    g_sensorQueue  = nullptr;
static QueueHandle_t    g_publishQueue = nullptr;
static DeliveryRuntime<> g_delivery(g_buffer, g_lossJournal);
// PubSubClient 2.8 dispatches at most one message per loop(), synchronously on
// connectionTask. This mailbox is consumed before the next loop; it is not
// delivery identity/state and is never read or written by another task.
static RecordIdentity g_pendingAck{};

// -----------------------------------------------------------------------------
// mqttEnqueue() — thread-safe helper used for non-buffered MQTT messages.
// Non-blocking: returns false immediately if the queue is full.
// -----------------------------------------------------------------------------
static bool mqttEnqueue(const char* topic, const char* payload,
                         bool retained = false) {
    MqttMessage msg{};
    strlcpy(msg.topic,   topic,   sizeof(msg.topic));
    strlcpy(msg.payload, payload, sizeof(msg.payload));
    msg.retained = retained;
    return xQueueSend(g_publishQueue, &msg, 0) == pdTRUE;
}

static bool parseAckPayload(const uint8_t* payload, unsigned int length,
                            uint32_t& boot, uint32_t& seq) {
    if (length >= MQTT_PAYLOAD_SIZE) return false;

    char buf[MQTT_PAYLOAD_SIZE];
    memcpy(buf, payload, length);
    buf[length] = '\0';

    unsigned long parsedBoot = 0;
    unsigned long parsedSeq = 0;
    if (sscanf(buf, "{\"boot\":%lu,\"seq\":%lu}", &parsedBoot, &parsedSeq) != 2) {
        return false;
    }

    boot = static_cast<uint32_t>(parsedBoot);
    seq = static_cast<uint32_t>(parsedSeq);
    return true;
}

static void handleMqttMessage(char* topic, uint8_t* payload,
                              unsigned int length) {
    if (strcmp(topic, MQTT_TOPIC_ACK) != 0) {
        return;
    }

    // Safe in PubSubClient callback: parse-only, no Serial, heap, or blocking I/O.
    uint32_t boot = 0;
    uint32_t seq = 0;
    if (!parseAckPayload(payload, length, boot, seq)) {
        return;
    }

    g_pendingAck = {boot, seq};
    g_ackPending = true;
}

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void initBootId();
static void initFaultRebootCount();
static void initPSRAM();
static void initMPU6050();
static void IRAM_ATTR safetyISR();
static void connectionTask(void* pvParams);
static void sensorTask(void* pvParams);
static void filterTask(void* pvParams);
static void safetyTask(void* pvParams);

// =============================================================================
// fatalSetupHalt() — called when a critical resource cannot be allocated in
// setup(). Retries with exponential-ish backoff up to kSetupMaxRetries times,
// then enters a visible terminal loop (never restarts again).
//
// NVS key "sensor"/"setup_fails" tracks attempts across soft resets.
// Cleared at the end of a successful setup() so the counter is per-incident.
// =============================================================================
static constexpr uint8_t  kSetupMaxRetries  = 3;
static const     uint32_t kSetupRetryDelays[] = {1000, 5000, 15000};  // ms

static void fatalSetupHalt(const char* msg) {
    Serial.println(msg);
    Preferences prefs;
    prefs.begin("sensor", false);
    const uint8_t n = prefs.getUChar("setup_fails", 0);
    if (n < kSetupMaxRetries) {
        const uint32_t waitMs = kSetupRetryDelays[n];
        prefs.putUChar("setup_fails", n + 1);
        prefs.end();
        Serial.printf("[Setup] Retry %u/%u — restarting in %u ms.\n",
                      n + 1, kSetupMaxRetries, waitMs);
        delay(waitMs);
        esp_restart();
    }
    prefs.end();
    Serial.println("[Setup] FATAL — max retries exhausted. Power-cycle required.");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(5000)); }
}

// =============================================================================
// setup()
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Industrial Sensor Node — Boot ===");

    initBootId();
    initFaultRebootCount();
    initPSRAM();

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setTimeOut(10);  // I2C read timeout: 10ms — fail fast on dropout, return zeros
    initMPU6050();

    // -------------------------------------------------------------------------
    // Store-and-forward buffer (PSRAM)
    // -------------------------------------------------------------------------
    // Journal storage is fixed internal RAM, independent of PSRAM availability.
    // Initialise its mutex before starting either producer; never silently run
    // without the ability to count lost diagnostic evidence.
    if (!g_lossJournal.begin()) {
        fatalSetupHalt("[Journal] FATAL — could not create loss journal mutex.");
    }
    if (!g_buffer.begin(PSRAM_BUFFER_CAPACITY)) {
        Serial.println("[Buffer] ERROR — PSRAM allocation failed. Telemetry will"
                       " be lost during network outages.");
    } else {
        Serial.printf("[Buffer] OK — %u record slots allocated in PSRAM.\n",
                      PSRAM_BUFFER_CAPACITY);
    }

    // -------------------------------------------------------------------------
    // Queues
    // -------------------------------------------------------------------------
    g_sensorQueue = xQueueCreate(SENSOR_QUEUE_DEPTH, sizeof(RawSample));
    if (g_sensorQueue == nullptr) {
        fatalSetupHalt("[Queue] FATAL — could not create sensor queue.");
    }

    g_publishQueue = xQueueCreate(MQTT_PUBLISH_QUEUE_DEPTH, sizeof(MqttMessage));
    if (g_publishQueue == nullptr) {
        fatalSetupHalt("[Queue] FATAL — could not create publish queue.");
    }

    Serial.printf("[Queue] OK — sensor depth %d, publish depth %d.\n",
                  SENSOR_QUEUE_DEPTH, MQTT_PUBLISH_QUEUE_DEPTH);

    // -------------------------------------------------------------------------
    // MQTT connection manager — wire state machine callbacks before tasks start
    // -------------------------------------------------------------------------
    g_mqttManager.onConnect([]() {
        g_delivery.newSession(millis());
    });

    g_mqttManager.onDisconnect([]() {
        g_delivery.observeTransport(millis(), false);
    });

    g_mqttManager.onMessage(handleMqttMessage);

    if (!g_mqttManager.begin(WIFI_SSID, WIFI_PASSWORD,
                             MQTT_BROKER_IP, MQTT_PORT,
                             MQTT_CLIENT_ID, MQTT_KEEPALIVE_S,
                             MQTT_CLIENT_BUFFER_SIZE)) {
        fatalSetupHalt("[MQTT] FATAL — could not allocate MQTT packet buffer.");
    }
    Serial.printf("[MQTT] OK — %u-byte packet buffer allocated.\n",
                  static_cast<unsigned>(MQTT_CLIENT_BUFFER_SIZE));

    // NTP — sync to UTC on WiFi connect. No offset, no DST.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // -------------------------------------------------------------------------
    // Safety interlock — EventGroup + GPIO interrupt
    // -------------------------------------------------------------------------
    g_safetyEvents = xEventGroupCreate();
    if (g_safetyEvents == nullptr) {
        fatalSetupHalt("[EventGroup] FATAL — could not create safety event group.");
    }
    pinMode(PIN_SAFETY_INTERLOCK, INPUT_PULLUP);
    delay(20);  // allow pin to settle before arming interrupt (prevents boot false trigger)
    attachInterrupt(digitalPinToInterrupt(PIN_SAFETY_INTERLOCK), safetyISR, FALLING);
    Serial.printf("[Safety] Interlock armed on GPIO %d (pin=%s).\n",
                  PIN_SAFETY_INTERLOCK,
                  digitalRead(PIN_SAFETY_INTERLOCK) == LOW ? "LOW — interlock open" : "HIGH — OK");
    if (digitalRead(PIN_SAFETY_INTERLOCK) == LOW) {
        // Interlock was already open at boot — latch without waiting for the FALLING edge.
        g_interlockActive.store(true);
        xEventGroupSetBits(g_safetyEvents, kBitInterlock);
        Serial.println("[Safety] Interlock open at boot — latch armed, E-Stop will publish on connect.");
    }

    // -------------------------------------------------------------------------
    // FreeRTOS tasks
    //   Core 1: sensorTask + filterTask — time-sensitive sensor pipeline
    //   Core 0: safetyTask + connectionTask — interlock handling and network I/O
    // -------------------------------------------------------------------------
    if (xTaskCreatePinnedToCore(safetyTask,     "SafetyTask",    2048, nullptr, 6, nullptr, 0) != pdPASS) {
        fatalSetupHalt("[Task] FATAL — could not create SafetyTask.");
    }
    if (xTaskCreatePinnedToCore(connectionTask, "ConnTask",      8192, nullptr, 4, nullptr, 0) != pdPASS) {
        fatalSetupHalt("[Task] FATAL — could not create ConnTask.");
    }
    if (xTaskCreatePinnedToCore(sensorTask,     "SensorTask",    4096, nullptr, 5, nullptr, 1) != pdPASS) {
        fatalSetupHalt("[Task] FATAL — could not create SensorTask.");
    }
    if (xTaskCreatePinnedToCore(filterTask,     "FilterTask",    8192, nullptr, 5, nullptr, 1) != pdPASS) {
        fatalSetupHalt("[Task] FATAL — could not create FilterTask.");
    }

    {
        Preferences prefs;
        prefs.begin("sensor", false);
        prefs.putUChar("setup_fails", 0);
        prefs.end();
    }
    Serial.println("Boot complete — FreeRTOS tasks running.");
}

// =============================================================================
// loop() — all real work is in tasks; yield to scheduler
// =============================================================================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// =============================================================================
// connectionTask — Core 0, Priority 4
//
// Sole MQTT, pacing and lease owner. This task pins,
// builds and publishes telemetry inline. g_publishQueue is only for E-Stop etc.
//
// Loop cadence: 10ms — fast enough for PubSubClient keepalive (default 15s)
// while yielding to other work on core 0. Sensor/filter tasks run on core 1.
// =============================================================================
static void connectionTask(void* pvParams) {
    MqttMessage msg;

    for (;;) {
        // Close ranges even if no further losses arrive or MQTT is offline.
        // Diagnostic publishing is a later milestone; do not drain/discard here.
        g_lossJournal.closeExpired(millis());
        // Account observed readiness BEFORE potentially blocking reconnect work. MQTT
        // callbacks also account exact observed edges inside this loop call.
        g_delivery.observeTransport(millis(), WiFi.status() == WL_CONNECTED &&
                                             g_mqttManager.isConnected());
        g_mqttManager.loop();
        const bool ackReceived = g_ackPending;
        g_ackPending = false;
        const bool connected = WiFi.status() == WL_CONNECTED && g_mqttManager.isConnected();
        const RecordIdentity ack = g_pendingAck;
        const auto subscribeAttempts = g_delivery.subscriptionCounters().attempts;
        g_delivery.step(
            millis(), connected,
            ackReceived ? &ack : nullptr,
            []() { return g_mqttManager.subscribe(MQTT_TOPIC_ACK, 1); },
            [](const TelemetryRecord& rec) {
                char payload[MQTT_PAYLOAD_SIZE];
                if (!buildPayload(rec, payload, sizeof(payload))) {
                    // Fail locally: retain the lease, apply normal retry pacing.
                    Serial.println("[Delivery] payload formatting failed; not published");
                    return false;
                }
                return g_mqttManager.publish(MQTT_TOPIC_TELEMETRY, payload, false);
            }, []() { return millis(); });
        // Log outside MQTT callbacks. Short-lived modes survive as pending bits;
        // within this iteration transitions follow BUFFERING -> SYNCING -> NORMAL.
        const uint8_t changes = g_delivery.takeModeChanges();
        if (changes & (1U << static_cast<unsigned>(NodeState::BUFFERING)))
            Serial.printf("[Delivery] t=%u entered BUFFERING\n", millis());
        if (changes & (1U << static_cast<unsigned>(NodeState::SYNCING)))
            Serial.printf("[Delivery] t=%u entered SYNCING\n", millis());
        if (changes & (1U << static_cast<unsigned>(NodeState::NORMAL)))
            Serial.printf("[Delivery] t=%u entered NORMAL\n", millis());
        if (subscribeAttempts != g_delivery.subscriptionCounters().attempts)
            Serial.printf("[Delivery] t=%u ACK subscription request %s (not SUBACK confirmation)\n",
                          millis(), g_delivery.subscriptionSent() ? "sent" : "failed");
        if (g_delivery.reportDue(millis())) {
            const auto& lease = g_delivery.lease().state();
            const auto& c = g_delivery.lease().counters();
            const auto& sub = g_delivery.subscriptionCounters();
            Serial.printf("[Delivery] mode=%u sub_sent=%u phase=%u boot=%u seq=%u calls=%u observed_ready_ms=%u buffered=%u\n",
                static_cast<unsigned>(g_delivery.mode()), static_cast<unsigned>(g_delivery.subscriptionSent()),
                static_cast<unsigned>(lease.phase), lease.identity.boot_id, lease.identity.sequence_id,
                lease.publish_calls, lease.connected_wait_ms, static_cast<unsigned>(g_buffer.available()));
            Serial.printf("[Delivery] publishes=%llu failures=%llu retries=%llu timeouts=%llu committed=%llu abandoned=%llu\n",
                (unsigned long long)c.publish_calls, (unsigned long long)c.publish_failures,
                (unsigned long long)c.retries, (unsigned long long)c.ack_timeouts,
                (unsigned long long)c.committed, (unsigned long long)c.abandoned);
            Serial.printf("[Delivery] stale=%llu mismatch=%llu internal_errors=%llu ownership_fault=%u sub_attempts=%llu sub_failures=%llu\n",
                (unsigned long long)c.ack_stale, (unsigned long long)c.ack_mismatch,
                (unsigned long long)c.lease_internal_errors,
                static_cast<unsigned>(g_delivery.lease().ownershipFault()),
                (unsigned long long)sub.attempts, (unsigned long long)sub.failures);
        }
        // Bounded non-telemetry work prevents producers starving lease service.
        for (size_t n = 0; n < MQTT_PUBLISH_QUEUE_DEPTH && g_mqttManager.isConnected() &&
             xQueueReceive(g_publishQueue, &msg, 0) == pdTRUE; ++n) {
            if (!g_mqttManager.publish(msg.topic, msg.payload, msg.retained)) {
                Serial.println("[ConnTask] WARN — publish failed mid-drain.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =============================================================================
// sensorTask — Core 1, Priority 5
//
// Samples the MPU-6050 at exactly SAMPLE_RATE_HZ using vTaskDelayUntil.
// Posts RawSamples to g_sensorQueue for filterTask to consume.
// Drops samples (not blocks) if the queue is full to preserve timing.

// =============================================================================
// Read the WHO_AM_I register (0x75) — expected response is 0x68.
// Proves actual I2C data transfer, unlike an address-only ACK probe.
// Wire.setTimeOut(10) bounds this to 10ms on failure.
// Last WHO_AM_I value read from the MPU. Set by isMpuHealthy().
// 0xFF = endTransmission failed; 0xFE = requestFrom failed; otherwise actual chip ID.
static uint8_t g_lastWhoAmI = 0xFF;

static bool isMpuHealthy(bool logResult = false) {
    Wire.beginTransmission(0x68);
    Wire.write(0x75);  // WHO_AM_I register
    if (Wire.endTransmission() != 0) {
        g_lastWhoAmI = 0xFF;
        return false;
    }
    if (Wire.requestFrom(static_cast<uint8_t>(0x68), static_cast<uint8_t>(1)) != 1) {
        g_lastWhoAmI = 0xFE;
        return false;
    }
    g_lastWhoAmI = Wire.read();
    if (logResult) {
        Serial.printf("[MPU] WHO_AM_I=0x%02X\n", g_lastWhoAmI);
    }
    // Accept genuine MPU-6050 (0x68) and common pin-compatible clones.
    return g_lastWhoAmI == 0x68 || g_lastWhoAmI == 0x70 || g_lastWhoAmI == 0x71;
}

// Attempt to unstick a frozen I2C bus after an SDA disconnect mid-transaction.
//
// Wire.end() releases the I2C peripheral's GPIO mux hold so gpio_set_level()
// can actually drive SCL. 9 clock pulses clock out whatever byte the MPU was
// stuck on; the STOP condition returns the bus to idle. Wire.begin() reclaims
// the pins. Called periodically while faulted (SENSOR_FAULT_RECOVERY_INTERVAL_MS)
// to retry after SDA reconnects.
static void recoverI2cBus() {
    Serial.println("[I2C] Bus recovery — clocking 9 SCL pulses.");

    // Wire.end() first so the I2C peripheral releases its signal-mux hold on the
    // pins. Without this, gpio_set_level() writes to the GPIO output register but
    // the peripheral's mux output takes precedence and SCL doesn't move.
    Wire.end();

    gpio_set_direction((gpio_num_t)PIN_SCL, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_SDA, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level((gpio_num_t)PIN_SDA, 1);

    for (int i = 0; i < 9; i++) {
        gpio_set_level((gpio_num_t)PIN_SCL, 0);
        delayMicroseconds(5);   // ~100 kHz
        gpio_set_level((gpio_num_t)PIN_SCL, 1);
        delayMicroseconds(5);
    }

    // STOP condition: SDA rises while SCL is high
    gpio_set_level((gpio_num_t)PIN_SDA, 0);
    delayMicroseconds(5);
    gpio_set_level((gpio_num_t)PIN_SCL, 1);
    delayMicroseconds(5);
    gpio_set_level((gpio_num_t)PIN_SDA, 1);
    delayMicroseconds(5);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setTimeOut(10);
}

// Re-initialise MPU-6050 to a known configuration after a fault clears.
// Handles edge cases where the sensor internally reset, its state machine
// got confused, or noise during reconnect corrupted its config registers.
// Returns true if the sensor responds and accepts configuration.
static bool reinitMpu() {
    if (!g_mpu.begin()) return false;
    g_mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    g_mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    g_mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    return true;
}

// Builds and pushes one fault TelemetryRecord. Called from both the immediate
// fault-state-entry path and the rate-limited slow-poll path in sensorTask.
// window_rms is set to NAN so buildPayload emits "wrms":null.
static void emitFaultRecord() {
    uint8_t faultFlag = (g_faultRebootCount >= SENSOR_FAULT_MAX_REBOOTS)
        ? STATUS_SENSOR_UNAVAILABLE : STATUS_DEGRADED_REBOOT_REQUIRED;
    if (g_interlockActive.load()) {
        faultFlag |= STATUS_INTERLOCK_OPEN;
    }
    TelemetryRecord fault{};
    fault.boot_id      = g_bootId;
    fault.sequence_id  = g_sequenceId.fetch_add(1);
    fault.timestamp_ms = getEpochMs();
    fault.status_flags = faultFlag;
    fault.accel_x      = (float)g_lastWhoAmI;
    fault.window_rms   = NAN;
    const PushOutcome outcome = g_buffer.push(fault);
    g_lossJournal.observePush(outcome, millis()); // push has released the buffer lock
    if (outcome.result == PushResult::NOT_INITIALISED) {
        Serial.println("[SensorTask] WARN — buffer not initialised, fault record lost.");
    } else if (outcome.result == PushResult::REJECTED_NEWEST) {
        Serial.println("[SensorTask] WARN — pinned buffer full, new fault record rejected.");
    }
    Serial.printf("[SensorTask] WARN — sensor fault seq=%u flags=0x%02X\n",
                  fault.sequence_id, faultFlag);
}

static void sensorTask(void* pvParams) {
    const TickType_t period   = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);
    TickType_t       lastWake = xTaskGetTickCount();
    uint8_t          consecutiveFailures = 0;
    bool             inFaultState        = false;
    uint32_t         faultEnteredMs      = 0;
    uint32_t         lastRecoveryMs      = 0;
    uint32_t         lastFaultEmitMs     = 0;
    uint32_t         recoveredAtMs       = 0;  // millis() when fault last cleared

    for (;;) {
        // ---- FAULT STATE: slow poll, rate-limited emission ----
        if (inFaultState) {
            vTaskDelay(pdMS_TO_TICKS(500));
            const uint32_t now = millis();

            if (isMpuHealthy()) {
                Serial.printf("[MPU] WHO_AM_I=0x%02X — bus recovered, reinitialising.\n",
                              g_lastWhoAmI);
                if (reinitMpu()) {
                    Serial.printf("[SensorTask] INFO — fault cleared after %u ms. "
                                  "Reboot counter held until 10s sustained operation.\n",
                                  now - faultEnteredMs);
                    inFaultState  = false;
                    recoveredAtMs = now;
                    lastWake      = xTaskGetTickCount();  // resync vTaskDelayUntil
                    continue;
                }
                // WHO_AM_I passed but reinit failed — stay faulted, retry next cycle
            } else {
                // Bus still unhealthy: periodic recovery attempt
                if (now - lastRecoveryMs >= SENSOR_FAULT_RECOVERY_INTERVAL_MS) {
                    lastRecoveryMs = now;
                    Serial.printf("[SensorTask] FAULT — bus recovery attempt at t=%u\n", now);
                    recoverI2cBus();
                }

                // Last-resort reboot — gives a clean Wire + MPU reset via initMPU6050().
                // Counter is NOT reset until 10s of sustained healthy operation so a
                // brief recovery that immediately re-faults doesn't burn a reboot slot.
                if (now - faultEnteredMs >= SENSOR_FAULT_REBOOT_MS) {
                    if (g_faultRebootCount < SENSOR_FAULT_MAX_REBOOTS) {
                        g_faultRebootCount++;
                        {
                            Preferences prefs;
                            prefs.begin("sensor", false);
                            prefs.putUChar("fault_reboots", g_faultRebootCount);
                            prefs.end();
                        }
                        Serial.printf("[SensorTask] FAULT — rebooting (attempt %u/%u)\n",
                                      g_faultRebootCount, SENSOR_FAULT_MAX_REBOOTS);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        esp_restart();
                    } else {
                        // Max reboots exhausted — emit once per 30s, wait for power-cycle.
                        Serial.println("[SensorTask] FAULT — sensor unavailable, "
                                       "power-cycle required.");
                        faultEnteredMs = now;
                    }
                }
            }

            // Rate-limited fault record — one per SENSOR_FAULT_EMIT_INTERVAL_MS.
            if (now - lastFaultEmitMs >= SENSOR_FAULT_EMIT_INTERVAL_MS) {
                lastFaultEmitMs = now;
                emitFaultRecord();
            }
            continue;
        }

        // ---- NORMAL OPERATION: 100 Hz ----

        if (!isMpuHealthy()) {
            if (++consecutiveFailures >= SENSOR_FAULT_THRESHOLD) {
                consecutiveFailures = 0;
                const uint32_t now = millis();
                inFaultState    = true;
                faultEnteredMs  = now;
                lastRecoveryMs  = now;   // first recovery deferred to RECOVERY_INTERVAL_MS
                lastFaultEmitMs = now;   // emit first fault record immediately
                recoveredAtMs   = 0;
                Serial.printf("[SensorTask] FAULT — entering fault state at t=%u "
                              "(reboots this session: %u/%u)\n",
                              now, g_faultRebootCount, SENSOR_FAULT_MAX_REBOOTS);
                // Emit one record immediately rather than waiting for the 500ms slow-poll.
                emitFaultRecord();
            }
            vTaskDelayUntil(&lastWake, period);
            continue;
        }

        // Healthy sample received. Reset failure counter.
        consecutiveFailures = 0;

        // After fault recovery, wait for 10s of sustained healthy operation before
        // clearing the reboot counter. This prevents a brief WHO_AM_I pass followed
        // by an immediate re-fault from resetting the counter and restarting the loop.
        if (recoveredAtMs != 0 && (millis() - recoveredAtMs) >= 10000) {
            Serial.printf("[SensorTask] INFO — 10s sustained recovery; "
                          "reboot counter cleared (was %u).\n", g_faultRebootCount);
            g_faultRebootCount = 0;
            {
                Preferences prefs;
                prefs.begin("sensor", false);
                prefs.putUChar("fault_reboots", 0);
                prefs.end();
            }
            recoveredAtMs = 0;
        }

        sensors_event_t accel_event, gyro_event, temp_event;
        g_mpu.getEvent(&accel_event, &gyro_event, &temp_event);

        RawSample sample{};
        sample.timestamp_ms = getEpochMs();
        sample.accel_x = (accel_event.acceleration.x - g_cal.ax_offset) * g_cal.ax_scale;
        sample.accel_y = (accel_event.acceleration.y - g_cal.ay_offset) * g_cal.ay_scale;
        sample.accel_z = (accel_event.acceleration.z - g_cal.az_offset) * g_cal.az_scale;
        sample.gyro_x       = gyro_event.gyro.x;
        sample.gyro_y       = gyro_event.gyro.y;
        sample.gyro_z       = gyro_event.gyro.z;
        sample.status_flags = STATUS_OK;

        // Clipping detection — flag readings at or beyond the hardware range
        if (fabsf(sample.accel_x) >= kAccelClipThreshold ||
            fabsf(sample.accel_y) >= kAccelClipThreshold ||
            fabsf(sample.accel_z) >= kAccelClipThreshold) {
            sample.status_flags |= STATUS_ACCEL_CLIPPED;
        }
        if (fabsf(sample.gyro_x) >= kGyroClipThreshold ||
            fabsf(sample.gyro_y) >= kGyroClipThreshold ||
            fabsf(sample.gyro_z) >= kGyroClipThreshold) {
            sample.status_flags |= STATUS_GYRO_CLIPPED;
        }

        if (xQueueSend(g_sensorQueue, &sample, 0) != pdTRUE) {
            Serial.println("[SensorTask] WARN — queue full, sample dropped.");
        }

        vTaskDelayUntil(&lastWake, period);
    }
}

// =============================================================================
// filterTask — Core 1, Priority 5
//
// Consumes RawSamples, applies 6 Kalman filters (one per IMU axis), accumulates
// a rolling RMS window, and emits one TelemetryRecord per FILTER_WINDOW_SIZE
// samples (~2 Hz). Pushes records to g_buffer for the publish layer.
// =============================================================================
static void filterTask(void* pvParams) {
    KalmanFilter kf_ax(KalmanFilter::kDefaultQ, KalmanFilter::kDefaultR, kAccelSpikeThreshold);
    KalmanFilter kf_ay(KalmanFilter::kDefaultQ, KalmanFilter::kDefaultR, kAccelSpikeThreshold);
    KalmanFilter kf_az(KalmanFilter::kDefaultQ, KalmanFilter::kDefaultR, kAccelSpikeThreshold);
    KalmanFilter kf_gx;
    KalmanFilter kf_gy;
    KalmanFilter kf_gz;

    float    sum_sq      = 0.0f;
    uint32_t windowCount = 0;

    float filtered_ax = 0.0f, filtered_ay = 0.0f, filtered_az = 0.0f;
    float filtered_gx = 0.0f, filtered_gy = 0.0f, filtered_gz = 0.0f;

    uint8_t  accumulated_flags = STATUS_OK;

    RawSample sample;

    for (;;) {
        if (xQueueReceive(g_sensorQueue, &sample, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        filtered_ax = kf_ax.update(sample.accel_x);
        filtered_ay = kf_ay.update(sample.accel_y);
        filtered_az = kf_az.update(sample.accel_z);
        filtered_gx = kf_gx.update(sample.gyro_x);
        filtered_gy = kf_gy.update(sample.gyro_y);
        filtered_gz = kf_gz.update(sample.gyro_z);

        sum_sq += filtered_ax * filtered_ax
                + filtered_ay * filtered_ay
                + filtered_az * filtered_az;
        windowCount++;
        accumulated_flags |= sample.status_flags;

        if (windowCount >= FILTER_WINDOW_SIZE) {
            const float rms = sqrtf(sum_sq / (float)windowCount);
            if (rms > kAnomalyRmsThreshold) {
                accumulated_flags |= STATUS_ANOMALY;
            }
            if (g_interlockActive.load()) {
                accumulated_flags |= STATUS_INTERLOCK_OPEN;
            }

            TelemetryRecord rec{};
            rec.boot_id      = g_bootId;
            rec.sequence_id  = g_sequenceId.fetch_add(1);
            rec.timestamp_ms = sample.timestamp_ms;  // window-end timestamp (last sample in window)
            rec.accel_x      = filtered_ax;
            rec.accel_y      = filtered_ay;
            rec.accel_z      = filtered_az;
            rec.gyro_x       = filtered_gx;
            rec.gyro_y       = filtered_gy;
            rec.gyro_z       = filtered_gz;
            rec.window_rms   = rms;
            rec.status_flags = accumulated_flags;

            const PushOutcome outcome = g_buffer.push(rec);
            g_lossJournal.observePush(outcome, millis());
            if (outcome.result == PushResult::NOT_INITIALISED) {
                Serial.println("[FilterTask] WARN — buffer not initialised, record lost.");
            } else if (outcome.result == PushResult::REJECTED_NEWEST) {
                Serial.println("[FilterTask] WARN — pinned buffer full, new record rejected.");
            }

            sum_sq            = 0.0f;
            windowCount       = 0;
            accumulated_flags = STATUS_OK;
        }
    }
}

// =============================================================================
// safetyISR — IRAM_ATTR, hardware interrupt context
//
// Fires on GPIO 10 falling edge (optical loop opens → pin pulled low).
// Must complete in microseconds: no heap allocation, no blocking calls,
// no non-IRAM functions. Only ISR-safe FreeRTOS APIs permitted.
// =============================================================================
static void IRAM_ATTR safetyISR() {
    // Atomically set latch — exchange returns previous value.
    // If it was already true, this is a bounce; return immediately.
    if (g_interlockActive.exchange(true)) return;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(g_safetyEvents, kBitInterlock,
                              &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// =============================================================================
// safetyTask — Core 0, Priority 6 (highest task in the system)
//
// Blocks on kBitInterlock. When the ISR fires:
//   1. Latches g_interlockActive — filterTask stamps STATUS_INTERLOCK_OPEN on
//      all subsequent TelemetryRecords until reboot.
//   2. Enqueues an E-Stop JSON event for connectionTask to publish.
//
// The latch is intentional: interlock trips are not self-clearing. A reboot
// (or explicit reset mechanism added later) is required to clear the state.
// =============================================================================
static void safetyTask(void* pvParams) {
    char payload[MQTT_PAYLOAD_SIZE];

    for (;;) {
        xEventGroupWaitBits(g_safetyEvents, kBitInterlock,
                            pdTRUE,    // clear bit on exit
                            pdFALSE,   // any bit sufficient
                            portMAX_DELAY);

        g_interlockActive.store(true);

        snprintf(payload, sizeof(payload),
                 "{\"ts\":%llu,\"triggered\":1,\"reason\":\"interlock\"}",
                 (unsigned long long)getEpochMs());

        if (!mqttEnqueue(MQTT_TOPIC_ESTOP, payload)) {
            Serial.println("[Safety] WARN — publish queue full, E-Stop event dropped.");
        }

        Serial.println("[Safety] INTERLOCK OPEN — E-Stop published.");
    }
}

// =============================================================================
// initBootId()
//
// Reads the boot counter from NVS namespace "sensor", increments it, writes
// it back, and stores it in g_bootId.
//
// First boot (key absent): getUInt returns the default 0, so g_bootId = 1.
// Subsequent boots: counter increments monotonically.
// NVS erase / corruption: falls back to 0 + 1 = 1 and resumes from there.
//
// g_bootId is used by filterTask to stamp every TelemetryRecord. Combined
// with sequence_id (per-boot counter), (boot_id, seq) uniquely identifies
// every record across reboots.
// =============================================================================
static void initBootId() {
    Preferences prefs;
    prefs.begin("sensor", false);   // namespace "sensor", read-write
    const uint32_t prev = prefs.getUInt("boot_id", 0);
    g_bootId = prev + 1;
    prefs.putUInt("boot_id", g_bootId);
    prefs.end();
    Serial.printf("[NVS] boot_id = %u (previous = %u)\n", g_bootId, prev);
}

// =============================================================================
// initFaultRebootCount()
//
// Loads the fault-reboot counter from NVS. On a power-on reset the counter is
// cleared to 0 (fresh session). On a software reset (esp_restart()) the stored
// value is kept so the counter accumulates across fault-triggered reboots.
//
// Written only on state transitions — before fault reboot, after sustained
// recovery, and here on power-on clear. Never written per fault record.
// =============================================================================
static void initFaultRebootCount() {
    Preferences prefs;
    prefs.begin("sensor", false);
    if (esp_reset_reason() == ESP_RST_POWERON) {
        prefs.putUChar("fault_reboots", 0);
        prefs.putUChar("setup_fails",   0);
        g_faultRebootCount = 0;
        Serial.println("[NVS] fault_reboots + setup_fails cleared (power-on reset).");
    } else {
        g_faultRebootCount = prefs.getUChar("fault_reboots", 0);
        Serial.printf("[NVS] fault_reboots = %u (software reset).\n",
                      g_faultRebootCount);
    }
    prefs.end();
}

// =============================================================================
// initPSRAM()
// Verifies that the OPI PSRAM was detected and initialised by the bootloader.
// board_build.arduino.memory_type = qio_opi and -DBOARD_HAS_PSRAM must both
// be set in platformio.ini for this to report anything other than 0 bytes.
// =============================================================================
static void initPSRAM() {
    size_t psramSize = ESP.getPsramSize();
    if (psramSize > 0) {
        Serial.printf("[PSRAM] OK — %u bytes available (%.2f MB)\n",
                      psramSize, psramSize / 1048576.0f);
    } else {
        Serial.println("[PSRAM] ERROR — 0 bytes detected. Check:");
        Serial.println("        1. board_build.arduino.memory_type = qio_opi");
        Serial.println("        2. -DBOARD_HAS_PSRAM build flag is set");
        Serial.println("        3. Physical module is genuinely N16R8");
    }
}

// =============================================================================
// initMPU6050()
// =============================================================================
static void initMPU6050() {
    // Unconditional hardware reset before g_mpu.begin().
    // If SDA was disconnected mid-transaction and recoverI2cBus() clocked SCL
    // pulses without a visible STOP, the MPU's I2C state machine can be left in
    // limbo. It survives esp_restart() (still powered). A write to PWR_MGMT_1
    // DEVICE_RESET bit clears all internal registers and returns the slave to
    // idle, letting the subsequent g_mpu.begin() start from a known state.
    Wire.beginTransmission(0x68);
    Wire.write(0x6B);   // PWR_MGMT_1
    Wire.write(0x80);   // DEVICE_RESET
    Wire.endTransmission();
    delay(100);         // datasheet: 100ms for reset to complete

    isMpuHealthy(true);  // log WHO_AM_I value before full init
    if (!g_mpu.begin()) {
        Serial.println("[MPU6050] ERROR — sensor not found on I2C bus.");
        Serial.printf("          SDA=%d  SCL=%d — verify wiring.\n",
                      PIN_SDA, PIN_SCL);
        return;
    }

    g_mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    g_mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    g_mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    Serial.println("[MPU6050] OK — initialised.");

    loadCalibration();
}

// =============================================================================
// loadCalibration — reads per-axis offset/scale from NVS; falls back to the
// bench-measured defaults baked into CalibrationData if keys are absent.
// =============================================================================
static void loadCalibration() {
    Preferences prefs;
    prefs.begin("sensor", true);  // read-only

    g_cal.ax_offset = prefs.getFloat("cal_ax_off", g_cal.ax_offset);
    g_cal.ax_scale  = prefs.getFloat("cal_ax_scl",  g_cal.ax_scale);
    g_cal.ay_offset = prefs.getFloat("cal_ay_off", g_cal.ay_offset);
    g_cal.ay_scale  = prefs.getFloat("cal_ay_scl",  g_cal.ay_scale);
    g_cal.az_offset = prefs.getFloat("cal_az_off", g_cal.az_offset);
    g_cal.az_scale  = prefs.getFloat("cal_az_scl",  g_cal.az_scale);

    prefs.end();

    Serial.printf("[Cal] ax: off=%.4f scl=%.4f\n", g_cal.ax_offset, g_cal.ax_scale);
    Serial.printf("[Cal] ay: off=%.4f scl=%.4f\n", g_cal.ay_offset, g_cal.ay_scale);
    Serial.printf("[Cal] az: off=%.4f scl=%.4f\n", g_cal.az_offset, g_cal.az_scale);
}
