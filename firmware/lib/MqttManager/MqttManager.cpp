// =============================================================================
// MqttManager.cpp
// =============================================================================

#include "MqttManager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================================
// begin() — store config; first connection attempt happens in loop()
// =============================================================================
void MqttManager::begin(const char* ssid,
                         const char* password,
                         const char* brokerIp,
                         uint16_t    port,
                         const char* clientId,
                         uint16_t    keepaliveSecs) {
    _ssid     = ssid;
    _password = password;
    _brokerIp = brokerIp;
    _port     = port;
    _clientId = clientId;

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);   // MqttManager owns the reconnect logic

    _mqttClient.setServer(_brokerIp, _port);
    _mqttClient.setKeepAlive(keepaliveSecs);
}

// =============================================================================
// loop() — called every ~10ms from connectionTask
//
// State machine:
//   WiFi down  → connectWifi() (blocks up to kWifiTimeoutMs)
//   WiFi up, MQTT down → wait for backoff, then connectMqtt()
//   WiFi up, MQTT up   → _mqttClient.loop() (keepalive / incoming messages)
//
// onDisconnect fires exactly once when connection is lost.
// onConnect fires exactly once after each successful (re)connect.
// =============================================================================
void MqttManager::loop() {

    // -------------------------------------------------------------------------
    // WiFi layer
    // -------------------------------------------------------------------------
    if (WiFi.status() != WL_CONNECTED) {
        if (_wasConnected) {
            _wasConnected = false;
            Serial.println("[MqttManager] WiFi lost — transitioning to BUFFERING.");
            if (_onDisconnect) _onDisconnect();
        }
        connectWifi();
        return;
    }

    // -------------------------------------------------------------------------
    // MQTT layer
    // -------------------------------------------------------------------------
    if (!_mqttClient.connected()) {
        if (_wasConnected) {
            _wasConnected = false;
            Serial.println("[MqttManager] Broker disconnected — transitioning to BUFFERING.");
            if (_onDisconnect) _onDisconnect();
        }

        // Respect backoff window before next attempt
        const uint32_t now = millis();
        if (now - _lastAttemptMs < _backoffMs) {
            return;
        }
        _lastAttemptMs = now;

        if (connectMqtt()) {
            _wasConnected = true;
            _backoffMs    = kBackoffInitialMs;   // reset on success
            if (_onConnect) _onConnect();
        } else {
            _backoffMs = min(_backoffMs * 2, kBackoffMaxMs);
        }
        return;
    }

    // -------------------------------------------------------------------------
    // Connected — drive keepalive and process any incoming messages
    // -------------------------------------------------------------------------
    _mqttClient.loop();
}

// =============================================================================
// publish()
// =============================================================================
bool MqttManager::publish(const char* topic, const char* payload, bool retained) {
    if (!_mqttClient.connected()) {
        return false;
    }
    return _mqttClient.publish(topic, payload, retained);
}

// =============================================================================
// connectWifi() — blocks up to kWifiTimeoutMs for association
//
// Called from loop() which runs inside connectionTask (Core 0). Blocking here
// is acceptable: the sensor pipeline is on Core 1 and continues unaffected.
// During the block, filterTask keeps pushing records to the PSRAM buffer.
// =============================================================================
void MqttManager::connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("[WiFi] Connecting to \"%s\"...", _ssid);
    WiFi.begin(_ssid, _password);

    const uint32_t deadline = millis() + kWifiTimeoutMs;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(200));
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected — IP %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        WiFi.disconnect(true);
        Serial.println("[WiFi] Connect timed out — will retry.");
    }
}

// =============================================================================
// connectMqtt() — one broker connect attempt; returns true on success
// =============================================================================
bool MqttManager::connectMqtt() {
    Serial.printf("[MQTT] Connecting to %s:%u as \"%s\"... ",
                  _brokerIp, _port, _clientId);

    if (_mqttClient.connect(_clientId)) {
        Serial.println("OK.");
        return true;
    }

    Serial.printf("failed (rc=%d). Next attempt in %ums.\n",
                  _mqttClient.state(), _backoffMs * 2 <= kBackoffMaxMs
                                           ? _backoffMs * 2
                                           : kBackoffMaxMs);
    return false;
}
