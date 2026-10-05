/**
 * sensor-mcp-server
 *
 * Exposes physical sensor telemetry from InfluxDB to LLM clients (Claude Code,
 * etc.) via the Model Context Protocol.
 *
 * Transport:
 *   stdio (default) — Claude Code spawns this process directly, local dev
 *   sse             — HTTP SSE server for network access from Pi
 *                     Set TRANSPORT=sse and optionally MCP_PORT=3002
 *
 * Usage:
 *   npm run dev                                      # local stdio
 *   TRANSPORT=sse MCP_PORT=3002 node dist/index.js   # Pi network mode
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { SSEServerTransport } from "@modelcontextprotocol/sdk/server/sse.js";
import express from "express";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
import { HttpError, InfluxDB } from "@influxdata/influxdb-client";
import {
  InputError, MAX_ANOMALY_EVENTS, NODE_ID_RE, classifyDependencyError, parseNodeId,
} from "./tool-utils.js";
import { createTelemetryTools } from "./telemetry-tools.js";

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

const { getLatestTelemetry, getSensorHealth, getRecentAnomalies } =
  createTelemetryTools(runFlux, INFLUX_BUCKET);

// ---------------------------------------------------------------------------
// MCP Server factory — one instance per SSE connection.
// The SDK's Server can only be connected to one transport at a time; reusing
// a global instance crashes on the second connection (e.g. Claude Code
// reconnecting after /mcp). Creating a fresh instance per connection avoids
// the "Already connected to a transport" error.
// ---------------------------------------------------------------------------
function createServer(): Server {
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
          additionalProperties: false,
          properties: {
            node_id: {
              type:        "string",
              pattern:     NODE_ID_RE.source,
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
          additionalProperties: false,
          properties: {
            node_id: {
              type:        "string",
              pattern:     NODE_ID_RE.source,
              description: "Sensor node ID (default: node01)",
              default:     "node01",
            },
          },
        },
      },
      {
        name:        "get_recent_anomalies",
        description: `Returns up to ${MAX_ANOMALY_EVENTS} most recent flagged or invalid-data events within a lookback window. Does not assess telemetry coverage; an empty result does not prove healthy operation.`,
        inputSchema: {
          type: "object",
          additionalProperties: false,
          properties: {
            node_id: {
              type:        "string",
              pattern:     NODE_ID_RE.source,
              description: "Sensor node ID (default: node01)",
              default:     "node01",
            },
            window_minutes: {
              type:        "integer",
              minimum:     1,
              maximum:     1440,
              description: "How many minutes of history to search (1–1440; default: 60)",
              default:     60,
            },
          },
        },
      },
    ],
  }));

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const { name, arguments: args } = request.params;

    try {
      const nodeId = parseNodeId(args?.node_id);
      let result: unknown;

      switch (name) {
        case "get_latest_telemetry":
          result = await getLatestTelemetry(nodeId);
          break;
        case "get_sensor_health":
          result = await getSensorHealth(nodeId);
          break;
        case "get_recent_anomalies":
          result = await getRecentAnomalies(nodeId, args?.window_minutes);
          break;
        default:
          throw new InputError(`Unknown tool: ${name}`);
      }

      // Compact, not pretty-printed — indentation is pure token cost for an
      // LLM consumer and roughly a third of a large anomaly response.
      return {
        content: [{ type: "text", text: JSON.stringify(result) }],
      };
    } catch (err) {
      const error = err instanceof InputError
        ? { code: "INVALID_ARGUMENT", message: err.message }
        : classifyDependencyError(err instanceof HttpError ? err.statusCode : undefined);

      if (!(err instanceof InputError)) {
        console.error(`MCP tool "${name}" failed:`, err);
      }

      return {
        content: [{ type: "text", text: JSON.stringify({ error }) }],
        isError: true,
      };
    }
  });

  return server;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
async function main() {
  const transportMode = process.env.TRANSPORT ?? "stdio";

  if (transportMode === "sse") {
    const port = parseInt(process.env.MCP_PORT ?? "3002");
    const app  = express();

    const transports = new Map<string, SSEServerTransport>();

    app.get("/sse", async (_req, res) => {
      const transport = new SSEServerTransport("/messages", res);
      transports.set(transport.sessionId, transport);
      res.on("close", () => transports.delete(transport.sessionId));
      await createServer().connect(transport);
    });

    app.post("/messages", async (req, res) => {
      const sessionId = req.query.sessionId as string;
      const transport = transports.get(sessionId);
      if (!transport) {
        res.status(400).json({ error: "Unknown sessionId" });
        return;
      }
      await transport.handlePostMessage(req, res);
    });

    app.listen(port, () => {
      console.error(`MCP SSE server listening on http://0.0.0.0:${port}`);
    });
  } else {
    const transport = new StdioServerTransport();
    await createServer().connect(transport);
    // stdio mode — do not write to stdout after this point.
  }
}

main().catch((err) => {
  console.error("MCP server fatal error:", err);
  process.exit(1);
});
