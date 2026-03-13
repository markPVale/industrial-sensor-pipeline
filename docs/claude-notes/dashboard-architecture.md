# Dashboard Architecture — Next.js Digital Twin

## Context

The `dashboard/` directory is a Next.js 14 application that provides a real-time digital twin view of sensor nodes. Phase 1 establishes the WebSocket telemetry pipeline and a minimal UI. Charting and the chaos panel are deferred to Phase 2.

---

## Key Decisions

### Direct MQTT over WebSocket (no intermediary WebSocket server)
The dashboard connects to Mosquitto's WebSocket listener (port 9001) using `mqtt.js` directly from the browser. This avoids an additional server component (e.g. Socket.IO or a custom WS relay) and keeps the data path as short as possible:

```
Firmware / mock_esp32.py
    → Mosquitto :1883
    → Mosquitto :9001 (WebSocket upgrade)
    → mqtt.js in browser
    → React state
```

Trade-off: the browser is coupled to the broker address. For production, a backend relay with auth would be preferable. This is acceptable for Phase 1 / lab use.

### App Router (Next.js 14)
Uses the App Router (`app/` directory). All interactive components that use hooks are marked `"use client"`. Static layout and page shell are server components.

### `next.config.ts` — webpack fallback
`mqtt.js` references Node.js built-ins (`net`, `tls`, `fs`) that do not exist in the browser. The webpack config sets these to `false` so the browser bundle resolves cleanly without them. This is the standard pattern for browser-targeting mqtt.js in Next.js.

---

## File Structure

```
dashboard/
├── app/
│   ├── layout.tsx           # Root layout, metadata, global CSS
│   ├── page.tsx             # Home page — renders TelemetryDisplay nodes
│   └── globals.css          # Dark theme CSS variables
├── hooks/
│   └── useMqttTelemetry.ts  # MQTT connection + state management
├── components/
│   ├── TelemetryDisplay.tsx # Per-node panel: metrics, sparkline, E-Stop alert
│   └── HeartbeatIndicator.tsx # Connection status + message-rate pulse
├── next.config.ts           # webpack fallback for mqtt.js Node built-ins
├── tsconfig.json
└── package.json
```

---

## Data Flow

```
useMqttTelemetry(nodeId)
    connects to ws://localhost:9001
    subscribes to:
        sensor/{nodeId}/telemetry   → updates latest + history (last 100)
        sensor/{nodeId}/estop       → updates estopEvent
    returns: { latest, history, estopEvent, connected, nodeId }

TelemetryDisplay
    consumes useMqttTelemetry
    renders: status badge, metrics table, sparkline, E-Stop alert

HeartbeatIndicator
    flashes blue on each incoming message
    shows green (idle connected) / muted (disconnected)
```

---

## Flag Bitmask

Mirrors the firmware `TelemetryRecord.flags` field:

| Bit | Constant     | Meaning                    |
|-----|--------------|----------------------------|
| 0   | FLAG_ESTOP   | Safety interlock triggered |
| 1   | FLAG_ANOMALY | Vibration anomaly detected |

---

## Environment Variables

| Variable                  | Default                  | Description                   |
|---------------------------|--------------------------|-------------------------------|
| `NEXT_PUBLIC_MQTT_WS_URL` | `ws://localhost:9001`    | Mosquitto WebSocket endpoint  |

Set in `.env.local` to override for different environments.

---

## Development

```bash
cd dashboard
npm install
npm run dev        # → http://localhost:3000
```

Requires the gateway stack to be running (`docker compose up -d` in `gateway/`).

---

## Next Steps

- [ ] Replace `SparkLine` SVG stub with Recharts `LineChart` for proper time-series rendering
- [ ] Add `ChaosPanel` component — UI to inject network partitions, sensor drift, gateway restart via MQTT command topics
- [ ] Add multi-node support — discover nodes dynamically from broker or config
- [ ] Add `NEXT_PUBLIC_MQTT_WS_URL` to `.env.local.example`
- [ ] Add E-Stop acknowledgement flow (publish to `sensor/{nodeId}/estop/ack`)
