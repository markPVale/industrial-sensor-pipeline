/**
 * sensor-mcp-server
 *
 * Exposes physical sensor telemetry from InfluxDB to LLM clients (Claude Code,
 * etc.) via the Model Context Protocol. Runs on the Raspberry Pi alongside the
 * gateway stack, or locally pointing at a remote InfluxDB instance.
 *
 * Transport: stdio (Claude Code spawns this process directly, or use SSH)
 * To run on the Pi and expose over the network, swap StdioServerTransport for
 * SSEServerTransport — see docs/claude-notes/mcp-server-architecture.md.
 *
 * Usage:
 *   INFLUX_URL=http://raspberrypi.local:8086 npm run dev
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
import { InfluxDB } from "@influxdata/influxdb-client";

// ---------------------------------------------------------------------------
// Configuration — override via environment variables
// ---------------------------------------------------------------------------
const INFLUX_URL    = process.env.INFLUX_URL    ?? "http://localhost:8086";
const INFLUX_TOKEN  = process.env.INFLUX_TOKEN  ?? "dev-token-change-in-production";
const INFLUX_ORG    = process.env.INFLUX_ORG    ?? "industrial";
const INFLUX_BUCKET = process.env.INFLUX_BUCKET ?? "sensors";

// ---------------------------------------------------------------------------
// InfluxDB client
// ---------------------------------------------------------------------------
const influx   = new InfluxDB({ url: INFLUX_URL, token: INFLUX_TOKEN });
const queryApi = influx.getQueryApi(INFLUX_ORG);

/** Execute a Flux query and return all result rows as plain objects. */
async function runFlux(query: string): Promise<Record<string, unknown>[]> {
  const rows: Record<string, unknown>[] = [];
  await new Promise<void>((resolve, reject) => {
    queryApi.queryRows(query, {
      next(row, meta) { rows.push(meta.toObject(row)); },
      error: reject,
      complete: resolve,
    });
  });
  return rows;
}

// ---------------------------------------------------------------------------
// Tool implementations
// ---------------------------------------------------------------------------

/**
 * get_latest_telemetry
 * Returns the single most recent vibration record for a node.
 */
async function getLatestTelemetry(nodeId: string) {
  const flux = `
    from(bucket: "${INFLUX_BUCKET}")
      |> range(start: -1h)
      |> filter(fn: (r) => r._measurement == "vibration" and r.node_id == "${nodeId}")
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> sort(columns: ["_time"], desc: true)
      |> limit(n: 1)
  `;

  const rows = await runFlux(flux);
  if (rows.length === 0) {
    return { node_id: nodeId, status: "no_data", message: "No telemetry found in the last hour." };
  }

  const r = rows[0];
  return {
    node_id:       nodeId,
    timestamp:     r["_time"],
    vibration_rms: r["vibration_rms"],
    flags:         r["flags"],
    flag_estop:    ((r["flags"] as number) & 0x01) !== 0,
    flag_anomaly:  ((r["flags"] as number) & 0x02) !== 0,
  };
}

/**
 * get_sensor_health
 * Returns a human-readable health summary for a node.
 */
async function getSensorHealth(nodeId: string) {
  const flux = `
    from(bucket: "${INFLUX_BUCKET}")
      |> range(start: -5m)
      |> filter(fn: (r) => r._measurement == "vibration" and r.node_id == "${nodeId}")
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> sort(columns: ["_time"], desc: true)
      |> limit(n: 1)
  `;

  const rows = await runFlux(flux);

  if (rows.length === 0) {
    return {
      node_id:   nodeId,
      is_online: false,
      message:   "No telemetry received in the last 5 minutes. Node may be offline or disconnected.",
    };
  }

  const r   = rows[0];
  const age = Math.round((Date.now() - new Date(r["_time"] as string).getTime()) / 1000);

  return {
    node_id:              nodeId,
    is_online:            age < 30,
    last_seen_seconds_ago: age,
    vibration_rms:        r["vibration_rms"],
    flag_estop:           ((r["flags"] as number) & 0x01) !== 0,
    flag_anomaly:         ((r["flags"] as number) & 0x02) !== 0,
    health_summary:       buildHealthSummary(age, r["flags"] as number, r["vibration_rms"] as number),
  };
}

function buildHealthSummary(ageSeconds: number, flags: number, rms: number): string {
  if (ageSeconds >= 30) return "OFFLINE — no recent telemetry.";
  if (flags & 0x01)     return "CRITICAL — E-Stop is active.";
  if (flags & 0x02)     return `WARNING — Anomaly detected. Vibration RMS: ${rms.toFixed(4)}g`;
  return `OK — Normal operation. Vibration RMS: ${rms.toFixed(4)}g`;
}

/**
 * get_recent_anomalies
 * Returns all records within the window where an anomaly or E-Stop flag was set.
 */
async function getRecentAnomalies(nodeId: string, windowMinutes: number) {
  const flux = `
    from(bucket: "${INFLUX_BUCKET}")
      |> range(start: -${windowMinutes}m)
      |> filter(fn: (r) => r._measurement == "vibration" and r.node_id == "${nodeId}")
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> filter(fn: (r) => r.flags > 0)
      |> sort(columns: ["_time"], desc: true)
  `;

  const rows = await runFlux(flux);

  if (rows.length === 0) {
    return {
      node_id:        nodeId,
      window_minutes: windowMinutes,
      anomaly_count:  0,
      message:        `No anomalies detected in the last ${windowMinutes} minutes.`,
      events:         [],
    };
  }

  const events = rows.map((r) => ({
    timestamp:     r["_time"],
    vibration_rms: r["vibration_rms"],
    flags:         r["flags"],
    flag_estop:    ((r["flags"] as number) & 0x01) !== 0,
    flag_anomaly:  ((r["flags"] as number) & 0x02) !== 0,
  }));

  return {
    node_id:        nodeId,
    window_minutes: windowMinutes,
    anomaly_count:  events.length,
    events,
  };
}

// ---------------------------------------------------------------------------
// MCP Server
// ---------------------------------------------------------------------------
const server = new Server(
  { name: "sensor-mcp-server", version: "0.1.0" },
  { capabilities: { tools: {} } },
);

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name:        "get_latest_telemetry",
      description: "Returns the most recent telemetry record for a sensor node, including vibration RMS, status flags, and timestamp.",
      inputSchema: {
        type: "object",
        properties: {
          node_id: {
            type:        "string",
            description: "Sensor node ID (default: node01)",
            default:     "node01",
          },
        },
      },
    },
    {
      name:        "get_sensor_health",
      description: "Returns a health summary for a sensor node: online status, seconds since last message, current vibration RMS, and whether an anomaly or E-Stop is active.",
      inputSchema: {
        type: "object",
        properties: {
          node_id: {
            type:        "string",
            description: "Sensor node ID (default: node01)",
            default:     "node01",
          },
        },
      },
    },
    {
      name:        "get_recent_anomalies",
      description: "Returns all anomaly and E-Stop events detected within a lookback window for a sensor node.",
      inputSchema: {
        type: "object",
        properties: {
          node_id: {
            type:        "string",
            description: "Sensor node ID (default: node01)",
            default:     "node01",
          },
          window_minutes: {
            type:        "number",
            description: "How many minutes of history to search (default: 60)",
            default:     60,
          },
        },
      },
    },
  ],
}));

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;
  const nodeId = (args?.node_id as string) ?? "node01";

  try {
    let result: unknown;

    switch (name) {
      case "get_latest_telemetry":
        result = await getLatestTelemetry(nodeId);
        break;
      case "get_sensor_health":
        result = await getSensorHealth(nodeId);
        break;
      case "get_recent_anomalies":
        result = await getRecentAnomalies(nodeId, (args?.window_minutes as number) ?? 60);
        break;
      default:
        throw new Error(`Unknown tool: ${name}`);
    }

    return {
      content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
    };
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    return {
      content: [{ type: "text", text: `Error: ${message}` }],
      isError: true,
    };
  }
});

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  // MCP servers communicate over stdio — do not write to stdout after this point.
}

main().catch((err) => {
  console.error("MCP server fatal error:", err);
  process.exit(1);
});
