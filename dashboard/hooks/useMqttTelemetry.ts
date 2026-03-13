"use client";

import { useEffect, useRef, useState } from "react";
import mqtt, { MqttClient } from "mqtt";

// ---------------------------------------------------------------------------
// Types — mirrors TelemetryRecord from firmware
// ---------------------------------------------------------------------------
export interface TelemetryRecord {
  timestamp:     number;   // ms epoch (edge-side)
  vibration_rms: number;   // g
  flags:         number;   // bitmask
}

export interface EstopEvent {
  timestamp: number;
  triggered: number;
  reason:    string;
}

export const FLAG_ESTOP   = 0b00000001;
export const FLAG_ANOMALY = 0b00000010;

export interface TelemetryState {
  latest:       TelemetryRecord | null;
  history:      TelemetryRecord[];   // last N records for charting
  estopEvent:   EstopEvent | null;
  connected:    boolean;
  nodeId:       string;
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
        const record = data as TelemetryRecord;
        setLatest(record);
        setHistory((prev) => [...prev.slice(-(HISTORY_LENGTH - 1)), record]);
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
