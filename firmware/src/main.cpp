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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

#include "config.h"
#include "types.h"
#include "KalmanFilter.h"
#include "BufferManager.h"
#include "MqttManager.h"

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
// MqttMessage — enqueued by telemetryTask/syncTask, drained by connectionTask.
// This is the only mechanism for other tasks to write to the MQTT socket.
// MqttManager is NOT thread-safe; all publish calls happen in connectionTask.
// -----------------------------------------------------------------------------
struct MqttMessage {
    char topic[64];
    char payload[MQTT_PAYLOAD_SIZE];
    bool retained;
};

// -----------------------------------------------------------------------------
// Hardware thresholds — derived from ranges configured in initMPU6050()
//   Accel ±8g  → 8 × 9.81 = 78.48 m/s²
//   Gyro  ±500 deg/s → 500 × π/180 = 8.727 rad/s
// -----------------------------------------------------------------------------
static constexpr float kAccelClipThreshold  = 78.48f;
static constexpr float kGyroClipThreshold   =  8.727f;

// Anomaly: RMS of accel magnitude over FILTER_WINDOW_SIZE samples (m/s²).
// ~1g midpoint between normal (0.30g) and anomaly (1.80g). Tune on hardware.
static constexpr float kAnomalyRmsThreshold = 9.81f;

// Spike rejection: 0.5g in m/s² for accel axes.
// Gyro threshold left at 0 (disabled) until noise profile is measured.
static constexpr float kAccelSpikeThreshold = 4.905f;

// Boot ID — loaded from NVS in initBootId(), incremented each boot.
// Initialised to 0; set before any task uses it.
static uint32_t g_bootId = 0;

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

// -----------------------------------------------------------------------------
// State machine
//
// g_nodeState is the single source of truth for NORMAL/BUFFERING/SYNCING.
// Written only by MqttManager callbacks (connectionTask, Core 0).
// Read by telemetryTask and syncTask (also Core 0).
// Using std::atomic for correctness; all access via getState()/setState().
// -----------------------------------------------------------------------------
static std::atomic<NodeState> g_nodeState{NodeState::NORMAL};

static NodeState getState() { return g_nodeState.load(); }
static void      setState(NodeState s) { g_nodeState.store(s); }

// EventGroup bit set by the onConnect callback to unblock syncTask.
static EventGroupHandle_t    g_mqttEvents  = nullptr;
static constexpr EventBits_t kBitReconnect = BIT0;

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
// Shared globals
// -----------------------------------------------------------------------------
static Adafruit_MPU6050 g_mpu;
static BufferManager    g_buffer;
static MqttManager      g_mqttManager;
static QueueHandle_t    g_sensorQueue  = nullptr;
static QueueHandle_t    g_publishQueue = nullptr;

// -----------------------------------------------------------------------------
// mqttEnqueue() — thread-safe helper used by telemetryTask and syncTask.
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

// -----------------------------------------------------------------------------
// buildPayload() — serialise one TelemetryRecord to a JSON string.
// Uses snprintf — no heap allocation, deterministic timing.
//
// Output example:
//   {"boot":1,"seq":42,"ts":123456789,"ax":-9.8123,"ay":0.1234,"az":9.8765,
//    "gx":0.0012,"gy":-0.0034,"gz":0.0056,"flags":0}
// -----------------------------------------------------------------------------
static void buildPayload(const TelemetryRecord& rec, char* buf, size_t len) {
    snprintf(buf, len,
        "{\"boot\":%u,\"seq\":%u,\"ts\":%llu,"
        "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
        "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f,"
        "\"flags\":%u}",
        rec.boot_id,      rec.sequence_id,  rec.timestamp_ms,
        rec.accel_x,      rec.accel_y,      rec.accel_z,
        rec.gyro_x,       rec.gyro_y,       rec.gyro_z,
        rec.status_flags);
}

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void initBootId();
static void initPSRAM();
static void initMPU6050();
static void IRAM_ATTR safetyISR();
static void connectionTask(void* pvParams);
static void sensorTask(void* pvParams);
static void filterTask(void* pvParams);
static void telemetryTask(void* pvParams);
static void syncTask(void* pvParams);
static void safetyTask(void* pvParams);

// =============================================================================
// setup()
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Industrial Sensor Node — Boot ===");

    initBootId();
    initPSRAM();

    Wire.begin(PIN_SDA, PIN_SCL);
    initMPU6050();

    // -------------------------------------------------------------------------
    // Store-and-forward buffer (PSRAM)
    // -------------------------------------------------------------------------
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
        Serial.println("[Queue] FATAL — could not create sensor queue.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    g_publishQueue = xQueueCreate(MQTT_PUBLISH_QUEUE_DEPTH, sizeof(MqttMessage));
    if (g_publishQueue == nullptr) {
        Serial.println("[Queue] FATAL — could not create publish queue.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    Serial.printf("[Queue] OK — sensor depth %d, publish depth %d.\n",
                  SENSOR_QUEUE_DEPTH, MQTT_PUBLISH_QUEUE_DEPTH);

    // -------------------------------------------------------------------------
    // EventGroup for MQTT reconnect signal (connectionTask → syncTask)
    // -------------------------------------------------------------------------
    g_mqttEvents = xEventGroupCreate();
    if (g_mqttEvents == nullptr) {
        Serial.println("[EventGroup] FATAL — could not create mqtt event group.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // -------------------------------------------------------------------------
    // MQTT connection manager — wire state machine callbacks before tasks start
    // -------------------------------------------------------------------------
    g_mqttManager.onConnect([]() {
        const NodeState prev = getState();
        if (prev == NodeState::BUFFERING) {
            setState(NodeState::SYNCING);
            xEventGroupSetBits(g_mqttEvents, kBitReconnect);
            Serial.printf("[State] BUFFERING → SYNCING (%u records in buffer)\n",
                          g_buffer.available());
        } else {
            setState(NodeState::NORMAL);
            Serial.println("[State] → NORMAL (initial connect)");
        }
    });

    g_mqttManager.onDisconnect([]() {
        setState(NodeState::BUFFERING);
        Serial.println("[State] → BUFFERING");
    });

    g_mqttManager.begin(WIFI_SSID, WIFI_PASSWORD,
                         MQTT_BROKER_IP, MQTT_PORT,
                         MQTT_CLIENT_ID, MQTT_KEEPALIVE_S);

    // -------------------------------------------------------------------------
    // Safety interlock — EventGroup + GPIO interrupt
    // -------------------------------------------------------------------------
    g_safetyEvents = xEventGroupCreate();
    if (g_safetyEvents == nullptr) {
        Serial.println("[EventGroup] FATAL — could not create safety event group.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    pinMode(PIN_SAFETY_INTERLOCK, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SAFETY_INTERLOCK), safetyISR, FALLING);
    Serial.printf("[Safety] Interlock armed on GPIO %d.\n", PIN_SAFETY_INTERLOCK);

    // -------------------------------------------------------------------------
    // FreeRTOS tasks
    //   Core 1: sensorTask + filterTask — time-sensitive sensor pipeline
    //   Core 0: connectionTask + telemetryTask + syncTask — network I/O
    // -------------------------------------------------------------------------
    xTaskCreatePinnedToCore(safetyTask,     "SafetyTask",    2048, nullptr, 6, nullptr, 0);
    xTaskCreatePinnedToCore(connectionTask, "ConnTask",      8192, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(sensorTask,     "SensorTask",    4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(filterTask,     "FilterTask",    8192, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(telemetryTask,  "TelemetryTask", 4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(syncTask,       "SyncTask",      4096, nullptr, 3, nullptr, 0);

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
// Sole owner of MqttManager and the WiFi/MQTT socket. All other tasks that
// need to publish do so by enqueuing into g_publishQueue; this task drains
// that queue and calls publish() on the socket.
//
// Loop cadence: 10ms — fast enough for PubSubClient keepalive (default 15s)
// while leaving plenty of CPU time for telemetryTask and syncTask.
// =============================================================================
static void connectionTask(void* pvParams) {
    MqttMessage msg;

    for (;;) {
        // Drive WiFi + MQTT connection state machine
        g_mqttManager.loop();

        // Drain the publish queue — only while connected
        while (g_mqttManager.isConnected() &&
               xQueueReceive(g_publishQueue, &msg, 0) == pdTRUE) {
            if (!g_mqttManager.publish(msg.topic, msg.payload, msg.retained)) {
                Serial.println("[ConnTask] WARN — publish failed mid-drain.");
                // Record is lost here; buffer is the real durability guarantee
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
static void sensorTask(void* pvParams) {
    const TickType_t period   = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);
    TickType_t       lastWake = xTaskGetTickCount();

    for (;;) {
        sensors_event_t accel_event, gyro_event, temp_event;
        g_mpu.getEvent(&accel_event, &gyro_event, &temp_event);

        RawSample sample{};
        sample.timestamp_ms = (uint64_t)millis();
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
    uint32_t sequence_id       = 0;

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
            rec.sequence_id  = sequence_id++;
            rec.timestamp_ms = sample.timestamp_ms;
            rec.accel_x      = filtered_ax;
            rec.accel_y      = filtered_ay;
            rec.accel_z      = filtered_az;
            rec.gyro_x       = filtered_gx;
            rec.gyro_y       = filtered_gy;
            rec.gyro_z       = filtered_gz;
            rec.status_flags = accumulated_flags;

            if (!g_buffer.push(rec)) {
                Serial.println("[FilterTask] WARN — buffer not initialised, record lost.");
            }

            sum_sq            = 0.0f;
            windowCount       = 0;
            accumulated_flags = STATUS_OK;
        }
    }
}

// =============================================================================
// telemetryTask — Core 0, Priority 3
//
// In NORMAL or SYNCING state: peeks the oldest record from g_buffer, serialises
// it to JSON, and enqueues it for connectionTask to publish. Only pops the
// record after a successful enqueue — if the publish queue is full the record
// stays in g_buffer and will be retried next tick.
//
// In BUFFERING state: does nothing. Records accumulate in PSRAM until syncTask
// drains them on reconnect.
// =============================================================================
static void telemetryTask(void* pvParams) {
    char payload[MQTT_PAYLOAD_SIZE];

    for (;;) {
        const NodeState state = getState();

        if (state == NodeState::NORMAL || state == NodeState::SYNCING) {
            TelemetryRecord rec;
            if (g_buffer.peek(rec)) {
                buildPayload(rec, payload, sizeof(payload));
                if (mqttEnqueue(MQTT_TOPIC_TELEMETRY, payload)) {
                    g_buffer.pop(rec);  // commit only after successful enqueue
                } else {
                    // Publish queue full — record stays in buffer, retry next tick
                    Serial.println("[TelemetryTask] WARN — publish queue full, will retry.");
                }
            }
        }
        // BUFFERING state: fall through with no action

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PUBLISH_MS));
    }
}

// =============================================================================
// syncTask — Core 0, Priority 3
//
// Waits for the kBitReconnect EventGroup bit, set by the onConnect callback
// when the state transitions BUFFERING → SYNCING.
//
// Drains g_buffer in batches of SYNC_BATCH_SIZE with SYNC_BATCH_DELAY_MS
// between batches to avoid flooding the MQTT broker. Uses the same
// peek/enqueue/pop pattern as telemetryTask.
//
// When the buffer is empty, transitions SYNCING → NORMAL.
// If the connection drops mid-drain, exits the inner loop; the outer loop
// blocks on the EventGroup again and the drain resumes on next reconnect.
// =============================================================================
static void syncTask(void* pvParams) {
    char payload[MQTT_PAYLOAD_SIZE];

    for (;;) {
        // Block until a reconnect event is signalled
        xEventGroupWaitBits(g_mqttEvents, kBitReconnect,
                            pdTRUE,    // clear bit on exit
                            pdFALSE,   // wait for any set bit
                            portMAX_DELAY);

        Serial.printf("[SyncTask] Drain starting — %u records buffered.\n",
                      g_buffer.available());

        // Drain in rate-limited batches
        while (!g_buffer.isEmpty()) {
            if (getState() != NodeState::SYNCING) {
                // Connection dropped again mid-drain — stop here
                Serial.println("[SyncTask] Connection lost during drain — pausing.");
                break;
            }

            for (int i = 0; i < SYNC_BATCH_SIZE && !g_buffer.isEmpty(); i++) {
                TelemetryRecord rec;
                if (g_buffer.peek(rec)) {
                    buildPayload(rec, payload, sizeof(payload));
                    if (mqttEnqueue(MQTT_TOPIC_TELEMETRY, payload)) {
                        g_buffer.pop(rec);
                    } else {
                        // Publish queue full — back off and retry this batch
                        Serial.println("[SyncTask] WARN — publish queue full, backing off.");
                        break;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(SYNC_BATCH_DELAY_MS));
        }

        // Transition to NORMAL when drain is complete
        if (g_buffer.isEmpty() && getState() == NodeState::SYNCING) {
            setState(NodeState::NORMAL);
            Serial.println("[State] SYNCING → NORMAL (buffer drained)");
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
                 (unsigned long long)millis());

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
