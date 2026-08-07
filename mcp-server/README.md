# MCP Server — Sensor Data LLM Interface

A lightweight Node.js server that exposes live and historical sensor telemetry
to LLM clients (Claude Code, etc.) via the Model Context Protocol. Runs on the
Raspberry Pi alongside the gateway stack and queries InfluxDB directly.

## Tools

| Tool | Description |
|------|-------------|
| `get_latest_telemetry` | Most recent telemetry record — vibration RMS, IMU values, decoded status flags |
| `get_sensor_health` | Health summary — online status, last-seen age, plain-English diagnosis |
| `get_recent_anomalies` | Up to 50 most recent actionable vibration and sensor-fault events within a configurable lookback window |

`get_sensor_health` returns a `health_summary` string with one of:

Health merges the latest `vibration` and `sensor_faults` flags inside the
freshness window. If `STATUS_INTERLOCK_OPEN` is present in either path, E-Stop
takes priority over sensor-fault summaries.

| Summary | Condition |
|---------|-----------|
| `OK — Normal operation` | No flags, data fresh |
| `WARNING — Anomaly detected` | `STATUS_ANOMALY` set |
| `DEGRADED — I2C fault detected. Auto-reboot pending.` | `STATUS_DEGRADED_REBOOT_REQUIRED` set |
| `CRITICAL — E-Stop / safety interlock is active.` | `STATUS_INTERLOCK_OPEN` set |
| `CRITICAL — Sensor unavailable. Max auto-reboots exhausted.` | `STATUS_SENSOR_UNAVAILABLE` set |
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

`.mcp.json` is gitignored (contains your Pi's local IP). Copy the example and
fill in your Pi's IP address — use the IP directly, not `sensor-gateway.local`,
as mDNS hostnames are not reliably resolved by Claude Code:

```bash
cp .mcp.example.json .mcp.json
# Edit .mcp.json and replace <PI_IP> with your Pi's IP (e.g. 192.168.1.189)
```

```json
{
  "mcpServers": {
    "sensor": {
      "type": "sse",
      "url": "http://<PI_IP>:3002/sse"
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
- **Bounded anomaly responses** — `get_recent_anomalies` returns at most 50
  events and sets `truncated: true` when more matches exist. The bound is
  enforced twice: `limit(n: 51)` in both Flux queries (so InfluxDB never streams
  unbounded rows) and a final slice after the two result sets are merged. A cap
  of 500 pretty-printed events measured ~236 KB — roughly 59k tokens, too large
  for a single tool result.
- **Truncation drops the OLDEST matches, and they are unreachable** — the
  queries return the newest rows and the tool exposes no `end`, offset, or
  cursor parameter, so narrowing `window_minutes` only moves the window's start
  closer to now and returns a subset of the same newest events. A shorter window
  can yield a complete (untruncated) set for that period, but there is currently
  no way to page backwards to the start of a long-running fault. Query InfluxDB
  directly for that.
- **`returned_event_count`, not a total** — when `truncated` is `true` this is
  the number of events in *this* response, not the number of matching events.
  The true total is not computed (both queries are row-bounded) rather than
  guessed at.
- **Compact JSON responses** — tool results are serialised without indentation;
  whitespace is pure token cost for an LLM consumer.
- **Data freshness** — ~500ms lag from firmware publish to queryable data
  (bridge write interval).
- **Flux pivot pattern** — InfluxDB stores each field as a separate row. Queries
  use `pivot()` to combine fields into one row per timestamp before returning results.
- **Flag decoding** — `get_latest_telemetry` and `get_sensor_health` return one
  boolean per flag (`flag_anomaly`, `flag_sensor_fault`, …) alongside the raw
  `flags` value. `get_recent_anomalies` instead returns `active_flags` — an array
  naming only the flags that are set — because a full boolean set costs ~185
  bytes per event and it returns many events.
- **Input validation is server-side; schema constraints are advisory** — this
  server uses the low-level MCP `Server` API, which validates the JSON-RPC
  envelope but *not* arguments against each tool's `inputSchema`. `node_id`
  and `window_minutes` are therefore checked in the handler. The `pattern`,
  `minimum`/`maximum`, and `additionalProperties: false` annotations are the
  published contract and are enforced only by clients that pre-validate.

## Error Responses

Failed tool calls return `isError: true` with a JSON body:

```json
{"error":{"code":"INVALID_ARGUMENT","message":"node_id must contain 1–32 letters, numbers, underscores, or hyphens"}}
```

| Code | Meaning |
|------|---------|
| `INVALID_ARGUMENT` | Bad tool arguments or unknown tool name. The `message` is safe to surface to the caller. |
| `DEPENDENCY_MISCONFIGURED` | InfluxDB rejected authentication/authorization or a configured resource was not found (HTTP 401/403/404). Correct the token, organization, bucket, or URL before retrying. |
| `DEPENDENCY_FAILURE` | InfluxDB is unreachable, a query failed for another reason, or an internal bug occurred. The message is deliberately generic; the underlying error is written to stderr (`~/mcp-server.log` on the Pi), so check there rather than the tool response when diagnosing. |
