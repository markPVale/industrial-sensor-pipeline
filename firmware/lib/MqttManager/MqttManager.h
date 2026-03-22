#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <functional>

// =============================================================================
// MqttManager — WiFi + MQTT connection lifecycle
//
// Responsibilities
// ----------------
//   - Connect WiFi on first call to loop() after begin().
//   - Connect to the MQTT broker once WiFi is up.
//   - Detect disconnects and fire the onDisconnect callback exactly once.
//   - Reconnect using exponential backoff (1s → 2s → ... → 60s cap).
//   - Fire the onConnect callback on each successful (re)connect.
//   - Drive the PubSubClient keepalive via loop().
//
// Threading model
// ---------------
//   ALL calls (begin, loop, publish, isConnected) MUST come from a single
//   dedicated FreeRTOS task — connectionTask in main.cpp. PubSubClient and
//   the underlying WiFiClient are NOT thread-safe. Other tasks publish by
//   enqueuing MqttMessage items into g_publishQueue; connectionTask drains
//   that queue inside its own loop iteration, before calling loop() here.
//
// Callbacks
// ---------
//   onConnect    — fired after each successful MQTT broker connect.
//   onDisconnect — fired once per disconnect event (WiFi loss or broker drop).
//   Both callbacks run synchronously inside loop(), on Core 0.
//   Keep them short — update the NodeState enum and set an EventGroup bit.
//   Do not block inside a callback.
// =============================================================================

class MqttManager {
public:
    using Callback = std::function<void()>;

    // Backoff parameters (milliseconds)
    static constexpr uint32_t kBackoffInitialMs = 1000;
    static constexpr uint32_t kBackoffMaxMs     = 60000;

    // WiFi connect timeout per attempt (ms). connectWifi() blocks for this
    // long waiting for WL_CONNECTED before giving up and returning.
    static constexpr uint32_t kWifiTimeoutMs    = 10000;

    // -------------------------------------------------------------------------
    // Setup — call once from setup() before tasks start
    // -------------------------------------------------------------------------

    // Register event callbacks. Call before begin().
    void onConnect(Callback cb)    { _onConnect = cb; }
    void onDisconnect(Callback cb) { _onDisconnect = cb; }

    // Store credentials and configure PubSubClient. Does NOT attempt to
    // connect — the first connection happens inside loop().
    void begin(const char* ssid,
               const char* password,
               const char* brokerIp,
               uint16_t    port,
               const char* clientId,
               uint16_t    keepaliveSecs = 15);

    // -------------------------------------------------------------------------
    // Runtime — call from connectionTask only
    // -------------------------------------------------------------------------

    // Drive the connection state machine and PubSubClient keepalive.
    // - Checks WiFi status; calls connectWifi() if needed.
    // - Checks MQTT status; attempts reconnect with backoff if needed.
    // - Calls _mqttClient.loop() when connected.
    // - Fires onConnect / onDisconnect callbacks at transition points.
    void loop();

    // Publish a single message. Returns false if not connected.
    // MUST be called from the same task as loop().
    bool publish(const char* topic, const char* payload, bool retained = false);

    bool isConnected() const { return _mqttClient.connected(); }

private:
    // Block up to kWifiTimeoutMs for WiFi association.
    void connectWifi();

    // Attempt one MQTT broker connect. Returns true on success.
    bool connectMqtt();

    WiFiClient   _wifiClient;
    PubSubClient _mqttClient{_wifiClient};

    const char* _ssid        = nullptr;
    const char* _password    = nullptr;
    const char* _brokerIp    = nullptr;
    uint16_t    _port        = 1883;
    const char* _clientId    = nullptr;

    Callback _onConnect    = nullptr;
    Callback _onDisconnect = nullptr;

    // Disconnect-event tracking — ensures callbacks fire exactly once per event
    bool     _wasConnected  = false;

    // Reconnect backoff state
    uint32_t _backoffMs     = kBackoffInitialMs;
    uint32_t _lastAttemptMs = 0;
};
