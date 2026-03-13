// =============================================================================
// main.cpp — ESP32-S3 N16R8 Industrial Sensor Node
// Minimal bootstrap: PSRAM, MPU-6050, and FreeRTOS task scaffolding.
// Application logic is deliberately absent — add incrementally.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "config.h"

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
Adafruit_MPU6050 mpu;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void initPSRAM();
static void initMPU6050();

// =============================================================================
// setup()
// =============================================================================
void setup() {
    // -------------------------------------------------------------------------
    // Serial — USB CDC (ARDUINO_USB_CDC_ON_BOOT=1 routes Serial over native USB)
    // Give the host a moment to enumerate before printing boot messages.
    // -------------------------------------------------------------------------
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Industrial Sensor Node — Boot ===");

    // -------------------------------------------------------------------------
    // PSRAM
    // -------------------------------------------------------------------------
    initPSRAM();

    // -------------------------------------------------------------------------
    // I2C + MPU-6050
    // -------------------------------------------------------------------------
    Wire.begin(PIN_SDA, PIN_SCL);
    initMPU6050();

    // -------------------------------------------------------------------------
    // TODO: Safety Interlock
    // Attach a GPIO interrupt on PIN_SAFETY_INTERLOCK (falling edge).
    // The ISR must be IRAM_ATTR and should post to a FreeRTOS event group
    // so the safety task can act without blocking the sensor pipeline.
    //
    // pinMode(PIN_SAFETY_INTERLOCK, INPUT_PULLUP);
    // attachInterrupt(digitalPinToInterrupt(PIN_SAFETY_INTERLOCK),
    //                 safetyISR, FALLING);
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // TODO: MQTT / WiFi
    // Initialise WiFi, then PubSubClient.
    // Keep credentials out of source — load from NVS or a provisioning flow.
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // TODO: Store-and-Forward Buffer
    // Allocate circular buffer in PSRAM via ps_malloc().
    // Buffer capacity is defined by PSRAM_BUFFER_CAPACITY in config.h.
    // Implement the NORMAL -> BUFFERING -> SYNCING state machine here.
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // TODO: FreeRTOS Tasks
    // Pin each task to a core and assign stack from PSRAM where appropriate.
    //
    // xTaskCreatePinnedToCore(sensorTask,    "SensorTask",    4096, NULL, 5, NULL, 1);
    // xTaskCreatePinnedToCore(filterTask,    "FilterTask",    4096, NULL, 5, NULL, 1);
    // xTaskCreatePinnedToCore(telemetryTask, "TelemetryTask", 4096, NULL, 3, NULL, 0);
    // xTaskCreatePinnedToCore(syncTask,      "SyncTask",      4096, NULL, 3, NULL, 0);
    // -------------------------------------------------------------------------

    Serial.println("Boot complete. Entering main loop.");
}

// =============================================================================
// loop()
// All real work moves into FreeRTOS tasks. Loop can remain empty or be used
// for a low-priority watchdog / health-check.
// =============================================================================
void loop() {
    // Yield to FreeRTOS scheduler. Remove once tasks are running.
    vTaskDelay(pdMS_TO_TICKS(1000));
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
// Configures the IMU for the sensor pipeline defaults.
// Range and filter settings will be tuned once the FreeRTOS sensor task exists.
// =============================================================================
static void initMPU6050() {
    if (!mpu.begin()) {
        Serial.println("[MPU6050] ERROR — sensor not found on I2C bus.");
        Serial.printf("          SDA=%d  SCL=%d — verify wiring.\n",
                      PIN_SDA, PIN_SCL);
        // Do not halt; allow the rest of the system to continue booting.
        return;
    }

    // Accelerometer range: ±8g is a reasonable starting point for industrial
    // vibration. Narrow to ±2g for fine-grained anomaly detection if needed.
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);

    // Gyro range: ±500 deg/s. Adjust once motion profile is characterised.
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);

    // Digital low-pass filter: 21 Hz cutoff reduces aliasing at 100 Hz sample
    // rate while preserving the vibration frequencies of interest.
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    Serial.println("[MPU6050] OK — initialised.");

    // TODO: Calibrate gyro/accel offsets and store results in NVS.
}
