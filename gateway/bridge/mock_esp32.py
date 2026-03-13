"""
mock_esp32.py — Simulated ESP32-S3 Sensor Node
Publishes synthetic vibration telemetry and E-Stop events via MQTT,
replicating the message schema the real firmware will produce.

Usage:
    python mock_esp32.py [--host localhost] [--port 1883] [--node node01] [--rate 2]

Modes (cycle automatically):
    NORMAL    — steady vibration RMS ~0.3g, no flags
    ANOMALY   — elevated RMS ~1.8g, anomaly flag set
    ESTOP     — triggers E-Stop event, publishes to estop topic
"""

import argparse
import json
import logging
import math
import random
import signal
import sys
import time

import paho.mqtt.client as mqtt

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Telemetry flags (mirrors firmware config.h / TelemetryRecord)
# ---------------------------------------------------------------------------
FLAG_NONE     = 0b00000000
FLAG_ESTOP    = 0b00000001
FLAG_ANOMALY  = 0b00000010

# ---------------------------------------------------------------------------
# Simulation state machine
# ---------------------------------------------------------------------------
class NodeState:
    NORMAL  = "NORMAL"
    ANOMALY = "ANOMALY"
    ESTOP   = "ESTOP"


class MockESP32:
    def __init__(self, node_id: str, publish_rate_hz: float):
        self.node_id   = node_id
        self.interval  = 1.0 / publish_rate_hz
        self.state     = NodeState.NORMAL
        self.tick      = 0
        self._state_tick = 0

    # ------------------------------------------------------------------
    # Synthetic signal generation
    # ------------------------------------------------------------------
    def _vibration_rms(self) -> float:
        """
        Simulate vibration RMS as a noisy signal.
        NORMAL:  ~0.3g  (motor running smoothly)
        ANOMALY: ~1.8g  (bearing wear / imbalance)
        """
        t = self.tick * self.interval
        if self.state == NodeState.NORMAL:
            base      = 0.30
            harmonic  = 0.05 * math.sin(2 * math.pi * 10 * t)   # 10 Hz fundamental
            noise     = random.gauss(0, 0.02)
        else:  # ANOMALY
            base      = 1.80
            harmonic  = 0.30 * math.sin(2 * math.pi * 10 * t)
            harmonic += 0.15 * math.sin(2 * math.pi * 20 * t)   # 2nd harmonic
            noise     = random.gauss(0, 0.10)

        return max(0.0, base + harmonic + noise)

    def _flags(self) -> int:
        if self.state == NodeState.ESTOP:
            return FLAG_ESTOP
        if self.state == NodeState.ANOMALY:
            return FLAG_ANOMALY
        return FLAG_NONE

    # ------------------------------------------------------------------
    # State transitions — advance roughly every N ticks
    # ------------------------------------------------------------------
    def _advance_state(self):
        self._state_tick += 1
        if self.state == NodeState.NORMAL and self._state_tick > 30:
            # Transition to ANOMALY every ~30 publishes
            log.info("[%s] STATE → ANOMALY (simulated bearing wear)", self.node_id)
            self.state = NodeState.ANOMALY
            self._state_tick = 0
        elif self.state == NodeState.ANOMALY and self._state_tick > 15:
            # Trigger E-Stop after sustained anomaly
            log.warning("[%s] STATE → ESTOP (safety interlock triggered)", self.node_id)
            self.state = NodeState.ESTOP
            self._state_tick = 0
        elif self.state == NodeState.ESTOP and self._state_tick > 5:
            # Reset back to NORMAL after brief E-Stop window
            log.info("[%s] STATE → NORMAL (reset after E-Stop)", self.node_id)
            self.state = NodeState.NORMAL
            self._state_tick = 0

    # ------------------------------------------------------------------
    # Message builders
    # ------------------------------------------------------------------
    def telemetry_payload(self) -> dict:
        return {
            "timestamp":     int(time.time() * 1000),  # milliseconds
            "vibration_rms": round(self._vibration_rms(), 4),
            "flags":         self._flags(),
        }

    def estop_payload(self) -> dict:
        return {
            "timestamp": int(time.time() * 1000),
            "triggered": 1,
            "reason":    "optical_interlock",
        }

    # ------------------------------------------------------------------
    # Publish one tick
    # ------------------------------------------------------------------
    def publish(self, client: mqtt.Client):
        tel = self.telemetry_payload()
        topic_tel = f"sensor/{self.node_id}/telemetry"
        client.publish(topic_tel, json.dumps(tel), qos=1)
        log.info("[%s] %s | rms=%.4f flags=%d",
                 self.node_id, self.state, tel["vibration_rms"], tel["flags"])

        if self.state == NodeState.ESTOP and self._state_tick == 1:
            topic_estop = f"sensor/{self.node_id}/estop"
            client.publish(topic_estop, json.dumps(self.estop_payload()), qos=1)
            log.warning("[%s] Published E-Stop event → %s", self.node_id, topic_estop)

        self.tick += 1
        self._advance_state()


# ---------------------------------------------------------------------------
# MQTT callbacks
# ---------------------------------------------------------------------------
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        log.info("Mock ESP32 connected to broker.")
    else:
        log.error("Failed to connect to broker — rc=%d", rc)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Mock ESP32 MQTT publisher")
    parser.add_argument("--host",  default="localhost", help="MQTT broker host")
    parser.add_argument("--port",  default=1883, type=int)
    parser.add_argument("--node",  default="node01",   help="Node ID")
    parser.add_argument("--rate",  default=2.0, type=float,
                        help="Publish rate in Hz (default: 2)")
    args = parser.parse_args()

    node = MockESP32(node_id=args.node, publish_rate_hz=args.rate)

    client = mqtt.Client(client_id=f"mock-{args.node}")
    client.on_connect = on_connect
    client.connect(args.host, args.port, keepalive=60)
    client.loop_start()

    def shutdown(sig, frame):
        log.info("Stopping mock node.")
        client.loop_stop()
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    log.info("Mock ESP32 '%s' publishing at %.1f Hz → %s:%d",
             args.node, args.rate, args.host, args.port)

    while True:
        node.publish(client)
        time.sleep(node.interval)


if __name__ == "__main__":
    main()
