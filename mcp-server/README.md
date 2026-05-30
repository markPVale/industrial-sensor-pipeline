# MCP Server — Sensor Data LLM Interface

A lightweight Node.js server that exposes live and historical sensor telemetry
to LLM clients (Claude Code, etc.) via the Model Context Protocol. Runs on the
Raspberry Pi alongside the gateway stack and queries InfluxDB directly.

## Tools

| Tool | Description |
|------|-------------|
| `get_latest_telemetry` | Most recent telemetry record — vibration RMS, IMU values, decoded status flags |
| `get_sensor_health` | Health summary — online status, last-seen age, plain-English diagnosis |
| `get_recent_anomalies` | All flagged events (any non-zero flag) within a configurable lookback window |

`get_sensor_health` returns a `health_summary` string with one of:

| Summary | Condition |
|---------|-----------|
| `OK — Normal operation` | No flags, data fresh |
| `WARNING — Anomaly detected` | `STATUS_ANOMALY` set |
| `DEGRADED — I2C fault detected. Auto-reboot pending.` | `STATUS_DEGRADED_REBOOT_REQUIRED` set |
| `CRITICAL — Sensor unavailable. Max auto-reboots exhausted.` | `STATUS_SENSOR_UNAVAILABLE` set |
| `CRITICAL — E-Stop / safety interlock is active.` | `STATUS_INTERLOCK_OPEN` set |
| `OFFLINE — no recent telemetry.` | No data in last 30s |

## Setup

```bash
npm install
npm run build       # compiles src/index.ts → dist/index.js
```

## Running

**Local (stdio, for development):**
```bash
node dist/index.js
```

**Pi (SSE transport, network mode):**
```bash
TRANSPORT=sse MCP_PORT=3002 \
  INFLUX_URL=http://localhost:8086 \
  INFLUX_TOKEN=dev-token-change-in-production \
  INFLUX_ORG=industrial \
  INFLUX_BUCKET=sensors \
  nohup node dist/index.js > ~/mcp-server.log 2>&1 &
```

Verify: `curl -s http://sensor-gateway.local:3002/sse`
Should return: `event: endpoint` / `data: /messages?sessionId=...`

## Deploy (Pi)

The Pi pulls from GitHub. Standard flow:

```bash
# From Mac
git push

# On Pi
cd /home/pi/industrial-sensor-pipeline
git pull
cd mcp-server
npm run build
# restart process (see Running above)
```

**Note:** The server is not managed by systemd or pm2 — it will not survive a Pi
reboot. Restart manually, or add to `gateway/docker-compose.yml` as a service.

## Claude Code Configuration

Project `.mcp.json` points Claude Code at the Pi SSE endpoint:

```json
{
  "mcpServers": {
    "sensor": {
      "type": "sse",
      "url": "http://sensor-gateway.local:3002/sse"
    }
  }
}
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `TRANSPORT` | `stdio` | `stdio` for local dev, `sse` for Pi network mode |
| `MCP_PORT` | `3002` | HTTP port (SSE mode only) |
| `INFLUX_URL` | `http://localhost:8086` | InfluxDB base URL |
| `INFLUX_TOKEN` | `dev-token-change-in-production` | InfluxDB API token |
| `INFLUX_ORG` | `industrial` | InfluxDB organisation |
| `INFLUX_BUCKET` | `sensors` | InfluxDB bucket |

## Architecture Notes

- **Stateless query layer** — no MQTT subscription, no in-memory state. Every
  tool call is a fresh Flux query against InfluxDB.
- **Data freshness** — ~500ms lag from firmware publish to queryable data
  (bridge write interval).
- **Flux pivot pattern** — InfluxDB stores each field as a separate row. Queries
  use `pivot()` to combine fields into one row per timestamp before returning results.
- **Flag decoding** — all three tools decode the full 8-bit status flags bitmask
  and return individual boolean fields (`flag_anomaly`, `flag_sensor_fault`, etc.)
  alongside the raw `flags` value.
