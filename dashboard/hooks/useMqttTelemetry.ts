"use client";

import { useEffect, useRef, useState } from "react";
import mqtt, { MqttClient } from "mqtt";

// ---------------------------------------------------------------------------
// Status flag bit positions — must match firmware/include/types.h and
// docs/telemetry-schema.md exactly.
// ---------------------------------------------------------------------------
export const FLAG_ACCEL_CLIPPED  = 0x01;
export const FLAG_GYRO_CLIPPED   = 0x02;
export const FLAG_INTERLOCK_OPEN = 0x04;  // E-Stop / safety interlock
export const FLAG_ANOMALY        = 0x08;

// Convenience alias used by TelemetryDisplay
export const FLAG_ESTOP = FLAG_INTERLOCK_OPEN;

// ---------------------------------------------------------------------------
// Types — mirrors TelemetryRecord from firmware + docs/telemetry-schema.md
// ---------------------------------------------------------------------------
export interface TelemetryRecord {
  boot:  number;   // boot cycle counter (from NVS)
  seq:   number;   // per-boot sequence counter
  ts:    number;   // epoch ms (edge-side timestamp)
  ax:    number;   // m/s² Kalman-filtered accel X
  ay:    number;   // m/s² Kalman-filtered accel Y
  az:    number;   // m/s² Kalman-filtered accel Z
  gx:    number;   // rad/s Kalman-filtered gyro X
  gy:    number;   // rad/s Kalman-filtered gyro Y
  gz:    number;   // rad/s Kalman-filtered gyro Z
  flags: number;   // bitmask — see FLAG_* constants above
  // Client-derived: not transmitted by firmware; computed on receipt
  vibration_rms: number;  // m/s², sqrt(ax² + ay² + az²)
}

export interface EstopEvent {
  timestamp: number;
  triggered: number;
  reason:    string;
}

export interface TelemetryState {
  latest:     TelemetryRecord | null;
  history:    TelemetryRecord[];   // last N records for charting
  estopEvent: EstopEvent | null;
  connected:  boolean;
  nodeId:     string;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
const BROKER_WS_URL  = process.env.NEXT_PUBLIC_MQTT_WS_URL ?? "ws://localhost:9001";
const HISTORY_LENGTH = 100;

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------
export function useMqttTelemetry(nodeId: string = "node01"): TelemetryState {
  const clientRef = useRef<MqttClient | null>(null);

  const [connected,  setConnected]  = useState(false);
  const [latest,     setLatest]     = useState<TelemetryRecord | null>(null);
  const [history,    setHistory]    = useState<TelemetryRecord[]>([]);
  const [estopEvent, setEstopEvent] = useState<EstopEvent | null>(null);

  useEffect(() => {
    const client = mqtt.connect(BROKER_WS_URL, {
      clientId: `dashboard-${Math.random().toString(16).slice(2)}`,
      clean:    true,
    });

    clientRef.current = client;

    client.on("connect", () => {
      setConnected(true);
      client.subscribe(`sensor/${nodeId}/telemetry`);
      client.subscribe(`sensor/${nodeId}/estop`);
    });

    client.on("disconnect", () => setConnected(false));
    client.on("error",      () => setConnected(false));

    client.on("message", (topic: string, payload: Buffer) => {
      let data: unknown;
      try {
        data = JSON.parse(payload.toString());
      } catch {
        return;
      }

      if (topic.endsWith("/telemetry")) {
        const raw = data as Omit<TelemetryRecord, "vibration_rms">;
        const record: TelemetryRecord = {
          ...raw,
          // Derive vibration_rms client-side — not transmitted by firmware
          vibration_rms: Math.sqrt(raw.ax ** 2 + raw.ay ** 2 + raw.az ** 2),
        };
        setLatest(record);
        setHistory((prev: TelemetryRecord[]) => [...prev.slice(-(HISTORY_LENGTH - 1)), record]);
      } else if (topic.endsWith("/estop")) {
        setEstopEvent(data as EstopEvent);
      }
    });

    return () => {
      client.end(true);
    };
  }, [nodeId]);

  return { latest, history, estopEvent, connected, nodeId };
}
