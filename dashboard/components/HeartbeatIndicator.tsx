"use client";

import { useEffect, useState } from "react";
import { TelemetryRecord } from "@/hooks/useMqttTelemetry";

interface Props {
  connected:  boolean;
  latest:     TelemetryRecord | null;
}

export default function HeartbeatIndicator({ connected, latest }: Props) {
  const [pulse, setPulse] = useState(false);

  // Flash the indicator on each new message
  useEffect(() => {
    if (!latest) return;
    setPulse(true);
    const t = setTimeout(() => setPulse(false), 300);
    return () => clearTimeout(t);
  }, [latest]);

  const color = !connected ? "var(--muted)"
              : pulse      ? "var(--blue)"
              :               "var(--green)";

  const label = !connected ? "DISCONNECTED"
              : pulse      ? "RECEIVING"
              :               "CONNECTED";

  return (
    <div style={{ display: "flex", alignItems: "center", gap: "0.5rem" }}>
      <span
        style={{
          display:      "inline-block",
          width:        "10px",
          height:       "10px",
          borderRadius: "50%",
          background:   color,
          transition:   "background 0.15s ease",
        }}
      />
      <span style={{ color, fontSize: "0.75rem", letterSpacing: "0.08em" }}>
        {label}
      </span>
    </div>
  );
}
