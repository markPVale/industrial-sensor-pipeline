# Demo Video Shot List

Target runtime: ~70 seconds

---

## Shot 1 — Hardware establish (5–8s)

Camera on ESP32-S3 + MPU-6050 wired up.

Slow push-in or static close-up.

Hold for a beat.

Optional narration:
> "This is a wireless industrial vibration sensor node built on ESP32."

---

## Shot 2 — Live telemetry / prove it's real (10–12s)

Cut to Grafana dashboard.

Show vibration RMS panel updating live.

Tap or shake the sensor.

Capture the RMS spike on screen.

**Goal:** hardware → telemetry → dashboard → live response

---

## Shot 3 — Fault injection + recovery (25–30s) — Hero shot

Back to hardware.

Pull SDA.

Cut between **two views only** (hardware + Grafana — most readable for any viewer):
- ESP32 / sensor hardware
- Grafana panel

Show:
- fault detection (flatline / gap appears on Grafana)
- dashboard gap / flatline while faulted
- reconnect SDA
- telemetry returning on Grafana

Key engineering concepts demonstrated:
- I2C fault detection
- degraded-state handling
- escalation logic
- recovery path

---

## Shot 4 — AI / MCP query (15–20s)

Cut to Claude Code terminal.

Run `get_sensor_health` or ask:
> "What's the health of node01 right now?"

Show structured response — use the real MCP output fields:

```json
{
  "node_id": "node01",
  "is_online": true,
  "health_summary": "OK — Normal operation. Vibration RMS: 10.2184 m/s²",
  "vibration_rms_mps2": 10.2184,
  "flag_sensor_fault": false,
  "flag_degraded_reboot_required": false,
  "flag_sensor_unavailable": false
}
```

This lands best after the fault demo — the viewer just watched the failure happen
live. The AI layer is now interpreting a real system they already understand.

---

## Shot 5 — Closing (5–8s)

End on Grafana with full timeline visible:
- normal telemetry
- fault event
- recovery

Or cut back to the hardware running normally.

Optional closing line:
> "End-to-end edge sensing, fault handling, store-and-forward buffering, and AI observability — running live on-device."

---

## Timing Summary

| Shot | Content | Duration |
|------|---------|----------|
| 1 | Hardware intro | 6s |
| 2 | Live telemetry | 10s |
| 3 | Fault demo | 28s |
| 4 | AI query | 18s |
| 5 | Closing | 8s |
| | **Total** | **~70s** |

---

## Story Arc

**hardware → live data → failure → recovery → AI explanation → close**
