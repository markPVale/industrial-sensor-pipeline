import TelemetryDisplay from "@/components/TelemetryDisplay";

export default function HomePage() {
  return (
    <main className="container">
      <header style={{ marginBottom: "2rem" }}>
        <h1 style={{ fontSize: "1.2rem", color: "var(--text)", marginBottom: "0.25rem" }}>
          Industrial Sensor Node — Digital Twin
        </h1>
        <p style={{ color: "var(--muted)", fontSize: "0.8rem" }}>
          Real-time telemetry via MQTT WebSocket → ws://localhost:9001
        </p>
      </header>

      {/* TODO Phase 2: map over discovered nodes dynamically */}
      <TelemetryDisplay nodeId="node01" />
    </main>
  );
}
