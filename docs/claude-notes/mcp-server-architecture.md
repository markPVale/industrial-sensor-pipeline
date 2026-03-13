# MCP Server Architecture

## Context

A Node.js MCP (Model Context Protocol) server that exposes physical sensor telemetry to LLM clients. Running on the Raspberry Pi 5 alongside the gateway stack, it allows Claude Code (or any MCP-compatible client) to query live and historical sensor data using natural language.

---

## Responsibilities

| Responsibility | Detail |
|---|---|
| Telemetry access | Queries InfluxDB v2 via Flux for structured time-series data |
| Tool exposure | Provides `get_latest_telemetry`, `get_sensor_health`, `get_recent_anomalies` |
| MCP transport | stdio (current) — Claude Code spawns or connects via SSH |
| Error handling | Returns `isError: true` with message; never crashes on bad queries |

The MCP server does **not** subscribe to MQTT, write to any database, or maintain in-memory state. It is a stateless query layer over InfluxDB.

---

## Key Decisions

### stdio transport (initial)
The server uses `StdioServerTransport` from `@modelcontextprotocol/sdk`. This means Claude Code spawns the process directly, or connects via an SSH pipe. It avoids running an HTTP listener on the Pi for Phase 1.

**Upgrade path to network transport:**
Swap `StdioServerTransport` for `SSEServerTransport` (HTTP/SSE) when you need Claude Code on a remote machine to connect directly without SSH. The tool handlers are transport-agnostic — only the `main()` function changes.

```typescript
// SSE transport (for remote Pi access without SSH):
import { SSEServerTransport } from "@modelcontextprotocol/sdk/server/sse.js";
import http from "http";

const httpServer = http.createServer();
const transport  = new SSEServerTransport("/mcp", response);
httpServer.listen(3002);
await server.connect(transport);
```

### InfluxDB as the single data source
The MCP server queries InfluxDB exclusively. It does not subscribe to MQTT. This means:
- Data is only as fresh as the `mqtt_to_influx` bridge write interval (~500ms lag)
- Historical queries are fully supported
- No stateful MQTT connection to manage

### Flux pivot pattern
Each InfluxDB field is stored as a separate row. The queries use `pivot()` to combine `vibration_rms` and `flags` into a single row per timestamp before returning results to the tool handler.

---

## Tool Designs

### `get_latest_telemetry`
- Flux range: last 1 hour
- Returns: single most recent record with decoded flag bits
- Use case: "What is the current vibration reading?"

### `get_sensor_health`
- Flux range: last 5 minutes (tighter window for online/offline detection)
- Computes `last_seen_seconds_ago` from current wall clock
- Node considered **offline** if last record is >30 seconds old
- Returns: structured health object + plain-English `health_summary` string
- Use case: "Is my sensor node online and healthy?"

### `get_recent_anomalies`
- Flux range: configurable `window_minutes` (default 60)
- Filters for `flags > 0` (any anomaly or E-Stop bit set)
- Returns: count + list of events with decoded flag bits
- Use case: "Were there any anomalies in the last hour?"

---

## Claude Code Configuration

To use the MCP server from Claude Code, add to `~/.claude/mcp_settings.json` (or project `.mcp.json`):

```json
{
  "mcpServers": {
    "sensor": {
      "command": "ssh",
      "args": [
        "pi@raspberrypi.local",
        "cd /home/pi/industrial-sensor-pipeline/mcp-server && node dist/index.js"
      ],
      "env": {
        "INFLUX_URL":   "http://localhost:8086",
        "INFLUX_TOKEN": "dev-token-change-in-production"
      }
    }
  }
}
```

For local development (laptop running docker compose):
```json
{
  "mcpServers": {
    "sensor": {
      "command": "node",
      "args": ["/path/to/mcp-server/dist/index.js"],
      "env": {
        "INFLUX_URL":   "http://localhost:8086",
        "INFLUX_TOKEN": "dev-token-change-in-production"
      }
    }
  }
}
```

---

## Environment Variables

| Variable       | Default                           | Description                  |
|----------------|-----------------------------------|------------------------------|
| `INFLUX_URL`   | `http://localhost:8086`           | InfluxDB base URL            |
| `INFLUX_TOKEN` | `dev-token-change-in-production`  | InfluxDB API token           |
| `INFLUX_ORG`   | `industrial`                      | InfluxDB organisation        |
| `INFLUX_BUCKET`| `sensors`                         | InfluxDB bucket name         |

---

## Next Steps

- [ ] Add `.mcp.json` to repo root with local dev configuration
- [ ] Build and verify `npm run build` compiles cleanly
- [ ] Test tools end-to-end with mock data (run mock_esp32.py + bridge first)
- [ ] Add `get_estop_history` tool for dedicated E-Stop event log
- [ ] Switch to `SSEServerTransport` when Pi deployment is ready
- [ ] Add to `gateway/docker-compose.yml` as a service (once SSE transport is in place)
