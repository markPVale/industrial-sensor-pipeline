"use client";

import { useMqttTelemetry, FLAG_ESTOP, FLAG_ANOMALY } from "@/hooks/useMqttTelemetry";
import HeartbeatIndicator from "./HeartbeatIndicator";

// TODO: Replace with a charting library (e.g. Recharts, Chart.js) for Phase 2
function SparkLine({ values }: { values: number[] }) {
  if (values.length < 2) return null;

  const W = 400, H = 80, PAD = 4;
  const max = Math.max(...values, 0.1);
  const pts = values
    .map((v, i) => {
      const x = PAD + (i / (values.length - 1)) * (W - PAD * 2);
      const y = H - PAD - (v / max) * (H - PAD * 2);
      return `${x},${y}`;
    })
    .join(" ");

  return (
    <svg width={W} height={H} style={{ display: "block" }}>
      <polyline
        points={pts}
        fill="none"
        stroke="var(--blue)"
        strokeWidth={1.5}
        strokeLinejoin="round"
      />
    </svg>
  );
}

// ---------------------------------------------------------------------------

export default function TelemetryDisplay({ nodeId = "node01" }: { nodeId?: string }) {
  const { latest, history, estopEvent, connected } = useMqttTelemetry(nodeId);

  const isEstop   = latest ? (latest.flags & FLAG_ESTOP)   !== 0 : false;
  const isAnomaly = latest ? (latest.flags & FLAG_ANOMALY) !== 0 : false;

  const statusColor = isEstop   ? "var(--red)"
                    : isAnomaly ? "var(--yellow)"
                    :             "var(--green)";

  const statusLabel = isEstop   ? "E-STOP"
                    : isAnomaly ? "ANOMALY"
                    :             "NORMAL";

  const rmsValues = history.map((r) => r.vibration_rms);

  return (
    <div
      style={{
        background:   "var(--surface)",
        border:       `1px solid var(--border)`,
        borderRadius: "8px",
        padding:      "1.5rem",
        maxWidth:     "500px",
      }}
    >
      {/* Header */}
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "1rem" }}>
        <h2 style={{ fontSize: "1rem", color: "var(--text)" }}>
          Node: <span style={{ color: "var(--blue)" }}>{nodeId}</span>
        </h2>
        <HeartbeatIndicator connected={connected} latest={latest} />
      </div>

      {/* Status badge */}
      <div
        style={{
          display:      "inline-block",
          padding:      "0.2rem 0.6rem",
          borderRadius: "4px",
          background:   statusColor + "22",
          color:        statusColor,
          fontSize:     "0.75rem",
          fontWeight:   "bold",
          letterSpacing:"0.1em",
          marginBottom: "1rem",
        }}
      >
        {statusLabel}
      </div>

      {/* Metrics */}
      <table style={{ width: "100%", borderCollapse: "collapse", marginBottom: "1rem" }}>
        <tbody>
          <tr>
            <td style={{ color: "var(--muted)", paddingBottom: "0.4rem" }}>Vibration RMS</td>
            <td style={{ textAlign: "right", color: "var(--text)" }}>
              {latest ? `${latest.vibration_rms.toFixed(4)} m/s²` : "—"}
            </td>
          </tr>
          <tr>
            <td style={{ color: "var(--muted)", paddingBottom: "0.4rem" }}>Flags</td>
            <td style={{ textAlign: "right", color: "var(--text)" }}>
              {latest ? `0b${latest.flags.toString(2).padStart(8, "0")}` : "—"}
            </td>
          </tr>
          <tr>
            <td style={{ color: "var(--muted)" }}>Last seen</td>
            <td style={{ textAlign: "right", color: "var(--text)" }}>
              {latest
                ? new Date(latest.ts).toLocaleTimeString()
                : "—"}
            </td>
          </tr>
        </tbody>
      </table>

      {/* Sparkline */}
      <div style={{ borderTop: "1px solid var(--border)", paddingTop: "1rem" }}>
        <p style={{ color: "var(--muted)", fontSize: "0.75rem", marginBottom: "0.5rem" }}>
          Vibration RMS — last {rmsValues.length} samples
        </p>
        <SparkLine values={rmsValues} />
      </div>

      {/* E-Stop alert */}
      {estopEvent && (
        <div
          style={{
            marginTop:    "1rem",
            padding:      "0.75rem",
            background:   "var(--red)22",
            border:       "1px solid var(--red)",
            borderRadius: "4px",
            color:        "var(--red)",
            fontSize:     "0.8rem",
          }}
        >
          <strong>E-STOP</strong> — {estopEvent.reason} @{" "}
          {new Date(estopEvent.timestamp).toLocaleTimeString()}
        </div>
      )}
    </div>
  );
}
