"""
mock_esp32.py — Simulated ESP32-S3 Sensor Node

Publishes synthetic telemetry that exactly matches the JSON payload produced by
firmware/src/main.cpp buildPayload(). Field names, types, units, and flag bit
positions must stay in sync with docs/telemetry-schema.md.

Usage:
    python mock_esp32.py [--host localhost] [--port 1883] [--node node01] [--rate 2]

Simulation states (cycle automatically):
    NORMAL  — steady vibration ~0.3g on ax/ay, no flags set
    ANOMALY — elevated vibration ~1.8g, STATUS_ANOMALY flag set
    ESTOP   — STATUS_INTERLOCK_OPEN flag set, estop event published separately
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
# Status flag bit positions — must match firmware/include/types.h exactly.
# See docs/telemetry-schema.md for the authoritative definitions.
# ---------------------------------------------------------------------------
STATUS_OK             = 0x00
STATUS_ACCEL_CLIPPED  = 0x01
STATUS_GYRO_CLIPPED   = 0x02
STATUS_INTERLOCK_OPEN = 0x04  # E-Stop / safety interlock
STATUS_ANOMALY        = 0x08

# ---------------------------------------------------------------------------
# Simulation states
# ---------------------------------------------------------------------------
class SimState:
    NORMAL  = "NORMAL"
    ANOMALY = "ANOMALY"
    ESTOP   = "ESTOP"


class MockESP32:
    def __init__(self, node_id: str, publish_rate_hz: float):
        self.node_id        = node_id
        self.interval       = 1.0 / publish_rate_hz
        self.state          = SimState.NORMAL
        self.tick           = 0
        self.seq_id         = 0
        self.boot_id        = 1   # matches firmware kBootId (fixed until NVS)
        self._state_tick    = 0

    # ------------------------------------------------------------------
    # Synthetic signal generation
    # ------------------------------------------------------------------

    def _vibration_amplitude_g(self) -> float:
        """
        Scalar vibration amplitude in g for the current simulation state.
        NORMAL:  ~0.30g  (motor running smoothly)
        ANOMALY: ~1.80g  (bearing wear / imbalance)
        """
        t = self.tick * self.interval
        if self.state == SimState.NORMAL:
            base     = 0.30
            harmonic = 0.05 * math.sin(2 * math.pi * 10 * t)
            noise    = random.gauss(0, 0.02)
        else:
            base     = 1.80
            harmonic = 0.30 * math.sin(2 * math.pi * 10 * t)
            harmonic += 0.15 * math.sin(2 * math.pi * 20 * t)
            noise    = random.gauss(0, 0.10)
        return max(0.0, base + harmonic + noise)

    def _axis_values(self) -> dict:
        """
        Generate Kalman-filtered-equivalent 6-axis values in firmware units.
        Gravity is excluded — simulates a calibrated sensor output.
          ax/ay/az in m/s²  (vibration component only, not including 9.81g on az)
          gx/gy/gz in rad/s
        """
        amp_mps2 = self._vibration_amplitude_g() * 9.81
        t        = self.tick * self.interval

        # Primary vibration on ax/ay; small residual on az
        ax = amp_mps2 * math.sin(2 * math.pi * 10 * t) + random.gauss(0, 0.05)
        ay = amp_mps2 * math.cos(2 * math.pi * 10 * t) + random.gauss(0, 0.05)
        az = 0.1 * amp_mps2 * math.sin(2 * math.pi * 3 * t) + random.gauss(0, 0.02)

        gx = random.gauss(0, 0.005)
        gy = random.gauss(0, 0.005)
        gz = random.gauss(0, 0.005)

        if self.state == SimState.ANOMALY:
            # Add 2nd harmonic — characteristic of bearing wear
            ax += 0.3 * amp_mps2 * math.sin(2 * math.pi * 20 * t)
            ay += 0.15 * amp_mps2 * math.cos(2 * math.pi * 20 * t)
            gx += 0.05 * math.sin(2 * math.pi * 3 * t)
            gy += 0.05 * math.cos(2 * math.pi * 3 * t)

        return {
            "ax": round(ax, 4),
            "ay": round(ay, 4),
            "az": round(az, 4),
            "gx": round(gx, 4),
            "gy": round(gy, 4),
            "gz": round(gz, 4),
        }

    def _flags(self) -> int:
        if self.state == SimState.ESTOP:
            return STATUS_INTERLOCK_OPEN
        if self.state == SimState.ANOMALY:
            return STATUS_ANOMALY
        return STATUS_OK

    # ------------------------------------------------------------------
    # State transitions — advance roughly every N publishes
    # ------------------------------------------------------------------

    def _advance_state(self):
        self._state_tick += 1
        if self.state == SimState.NORMAL and self._state_tick > 30:
            log.info("[%s] STATE → ANOMALY (simulated bearing wear)", self.node_id)
            self.state = SimState.ANOMALY
            self._state_tick = 0
        elif self.state == SimState.ANOMALY and self._state_tick > 15:
            log.warning("[%s] STATE → ESTOP (safety interlock triggered)", self.node_id)
            self.state = SimState.ESTOP
            self._state_tick = 0
        elif self.state == SimState.ESTOP and self._state_tick > 5:
            log.info("[%s] STATE → NORMAL (reset after E-Stop)", self.node_id)
            self.state = SimState.NORMAL
            self._state_tick = 0

    # ------------------------------------------------------------------
    # Message builders
    # ------------------------------------------------------------------

    def telemetry_payload(self) -> dict:
        """Build a payload matching firmware buildPayload() exactly."""
        axes = self._axis_values()
        return {
            "boot":  self.boot_id,
            "seq":   self.seq_id,
            "ts":    int(time.time() * 1000),
            **axes,
            "flags": self._flags(),
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
        tel       = self.telemetry_payload()
        topic_tel = f"sensor/{self.node_id}/telemetry"
        client.publish(topic_tel, json.dumps(tel), qos=1)

        rms_mps2 = math.sqrt(tel["ax"] ** 2 + tel["ay"] ** 2 + tel["az"] ** 2)
        log.info("[%s] %s | rms=%.4f m/s² flags=0x%02x seq=%d",
                 self.node_id, self.state, rms_mps2, tel["flags"], tel["seq"])

        if self.state == SimState.ESTOP and self._state_tick == 1:
            topic_estop = f"sensor/{self.node_id}/estop"
            client.publish(topic_estop, json.dumps(self.estop_payload()), qos=1)
            log.warning("[%s] Published E-Stop event → %s", self.node_id, topic_estop)

        self.tick   += 1
        self.seq_id += 1
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
    parser.add_argument("--node",  default="node01",    help="Node ID")
    parser.add_argument("--rate",  default=2.0, type=float,
                        help="Publish rate in Hz (default: 2)")
    args = parser.parse_args()

    node   = MockESP32(node_id=args.node, publish_rate_hz=args.rate)
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
