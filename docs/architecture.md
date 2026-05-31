# System Architecture

## Full System

```mermaid
graph LR
    subgraph hw["ESP32-S3 N16R8"]
        mpu["MPU-6050\n100Hz IMU"]
        ldr["Photoresistor\nSafety Interlock"]
        fw["FreeRTOS\n6 Tasks"]
        psram["PSRAM Buffer\n50,000 records"]
        mpu --> fw
        ldr -->|"falling-edge ISR"| fw
        fw <--> psram
    end

    subgraph pi["Raspberry Pi 5 Gateway (Docker)"]
        mosquitto["Mosquitto\nMQTT Broker\n:1883 / :9001 WS"]
        bridge["Python Bridge\nmqtt_to_influx.py"]
        influx["InfluxDB 2.7\n:8086"]
        grafana["Grafana\n:3001"]
        mcp["MCP Server\nNode.js :3002"]
    end

    subgraph clients["Clients"]
        dashboard["Next.js Dashboard\n:3000"]
        claude["Claude Code\nMCP Client"]
    end

    fw -->|"MQTT QoS 0\nWiFi"| mosquitto
    mosquitto --> bridge
    bridge --> influx
    influx --> grafana
    influx --> mcp
    mosquitto -->|"WebSocket"| dashboard
    mcp -->|"SSE / MCP Protocol"| claude
```

---

## Store-and-Forward State Machine

Minimises data loss through network partitions — records buffer in PSRAM during
outages and drain on reconnect. Not end-to-end guaranteed delivery: QoS 0 publish
and a small in-memory queue between the buffer and the MQTT socket mean a
connection drop mid-drain can lose up to one batch of records. All state is a
single `std::atomic<NodeState>` — no scattered boolean flags.

```mermaid
stateDiagram-v2
    [*] --> NORMAL : boot + MQTT connect

    NORMAL --> BUFFERING : MQTT disconnect\n(WiFi loss / broker down)
    BUFFERING --> SYNCING : MQTT reconnect

    NORMAL : NORMAL\nReal-time 2Hz publish via MQTT
    BUFFERING : BUFFERING\nRecords written to PSRAM\n(up to 50,000 × 44 bytes ≈ 2.1 MB)
    SYNCING : SYNCING\nLive stream resumes\nsyncTask burst-drains buffer\nin batches of 20

    SYNCING --> NORMAL : buffer empty
    SYNCING --> BUFFERING : disconnect during drain
```

---

## I2C Fault Escalation

Graduated response to MPU-6050 communication failure (e.g. SDA hot-unplug).
Reboot counter is stored in NVS — persists across `esp_restart()` but clears
on power-cycle or after 10s of sustained healthy operation.

```mermaid
stateDiagram-v2
    [*] --> Healthy : boot

    Healthy --> Faulted : 5 consecutive\nWHO_AM_I failures\n(flags = 0x20 DEGRADED)

    Faulted --> Healthy : WHO_AM_I passes\n+ reinitMpu() succeeds\n(reboot counter held\nuntil 10s stable)

    Faulted --> Faulted : every 3s — 9-pulse\nSCL bus recovery attempt\nevery 5s — emit fault record

    Faulted --> Rebooting : 30s elapsed\nAND reboots remaining\n(g_faultRebootCount < 3)

    Rebooting --> Faulted : esp_restart()\nboot_id increments\nfault_reboots written to NVS

    Faulted --> Unavailable : 30s elapsed\nAND reboots exhausted\n(g_faultRebootCount = 3)\n(flags = 0x40 UNAVAILABLE)

    Unavailable : UNAVAILABLE\nNo further reboots\nFault record every 5s\nPower-cycle or reconnect SDA required

    Unavailable --> Healthy : SDA reconnected\nWHO_AM_I passes\n+ reinitMpu() succeeds
```
