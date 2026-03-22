"""
mqtt_to_influx.py — MQTT → InfluxDB Bridge
Subscribes to sensor MQTT topics and writes records to InfluxDB v2.

Usage:
    python mqtt_to_influx.py

Environment variables (override defaults for non-local deployments):
    MQTT_HOST       MQTT broker host       (default: localhost)
    MQTT_PORT       MQTT broker port       (default: 1883)
    INFLUX_URL      InfluxDB URL           (default: http://localhost:8086)
    INFLUX_TOKEN    InfluxDB API token     (default: dev-token-change-in-production)
    INFLUX_ORG      InfluxDB organisation  (default: industrial)
    INFLUX_BUCKET   InfluxDB bucket        (default: sensors)
"""

import json
import logging
import math
import os
import signal
import sys

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
MQTT_HOST     = os.getenv("MQTT_HOST",    "localhost")
MQTT_PORT     = int(os.getenv("MQTT_PORT", "1883"))
INFLUX_URL    = os.getenv("INFLUX_URL",   "http://localhost:8086")
INFLUX_TOKEN  = os.getenv("INFLUX_TOKEN", "dev-token-change-in-production")
INFLUX_ORG    = os.getenv("INFLUX_ORG",   "industrial")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET","sensors")

TOPIC_TELEMETRY = "sensor/+/telemetry"
TOPIC_ESTOP     = "sensor/+/estop"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# InfluxDB client
# ---------------------------------------------------------------------------
influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = influx_client.write_api(write_options=SYNCHRONOUS)


def write_telemetry(node_id: str, payload: dict) -> None:
    """
    Write a telemetry record to InfluxDB.

    Payload shape follows docs/telemetry-schema.md.
    vibration_rms is not transmitted by firmware — it is derived here as
    sqrt(ax² + ay² + az²) for use by Grafana panels and MCP queries.
    """
    ax = float(payload.get("ax", 0.0))
    ay = float(payload.get("ay", 0.0))
    az = float(payload.get("az", 0.0))
    vibration_rms = math.sqrt(ax ** 2 + ay ** 2 + az ** 2)

    flags = int(payload.get("flags", 0))

    point = (
        Point("vibration")
        .tag("node_id", node_id)
        # Per-axis filtered values (firmware units)
        .field("ax",           ax)
        .field("ay",           ay)
        .field("az",           az)
        .field("gx",           float(payload.get("gx", 0.0)))
        .field("gy",           float(payload.get("gy", 0.0)))
        .field("gz",           float(payload.get("gz", 0.0)))
        # Derived metric: accel vector magnitude in m/s²
        .field("vibration_rms", vibration_rms)
        # Record identity
        .field("boot_id",      int(payload.get("boot", 0)))
        .field("sequence_id",  int(payload.get("seq",  0)))
        .field("flags",        flags)
    )
    # Prefer edge-side timestamp ("ts") over broker-arrival time.
    if "ts" in payload:
        point = point.time(int(payload["ts"]), WritePrecision.MS)

    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    log.debug("Written telemetry for %s: rms=%.4f m/s² flags=0x%02x seq=%d",
              node_id, vibration_rms, flags, payload.get("seq", -1))


def write_estop(node_id: str, payload: dict) -> None:
    """Write an E-Stop event to InfluxDB."""
    point = (
        Point("estop_event")
        .tag("node_id", node_id)
        .field("triggered", int(payload.get("triggered", 1)))
        .field("reason", str(payload.get("reason", "unknown")))
    )
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    log.warning("E-Stop event written for node %s", node_id)


# ---------------------------------------------------------------------------
# MQTT callbacks
# ---------------------------------------------------------------------------
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        log.info("Connected to MQTT broker at %s:%d", MQTT_HOST, MQTT_PORT)
        client.subscribe(TOPIC_TELEMETRY)
        client.subscribe(TOPIC_ESTOP)
        log.info("Subscribed to: %s, %s", TOPIC_TELEMETRY, TOPIC_ESTOP)
    else:
        log.error("MQTT connection failed — return code %d", rc)


def on_message(client, userdata, msg):
    topic_parts = msg.topic.split("/")
    if len(topic_parts) < 3:
        log.warning("Unexpected topic format: %s", msg.topic)
        return

    node_id = topic_parts[1]
    msg_type = topic_parts[2]

    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        log.error("Failed to parse payload on %s: %s", msg.topic, e)
        return

    try:
        if msg_type == "telemetry":
            write_telemetry(node_id, payload)
        elif msg_type == "estop":
            write_estop(node_id, payload)
        else:
            log.debug("Unhandled message type '%s' on topic %s", msg_type, msg.topic)
    except Exception as e:
        log.error("Failed to write to InfluxDB: %s", e)


def on_disconnect(client, userdata, rc):
    if rc != 0:
        log.warning("Unexpected MQTT disconnect (rc=%d). Will auto-reconnect.", rc)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    client = mqtt.Client(client_id="mqtt-influx-bridge")
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    def shutdown(sig, frame):
        log.info("Shutting down bridge...")
        client.disconnect()
        influx_client.close()
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    log.info("Connecting to MQTT broker at %s:%d ...", MQTT_HOST, MQTT_PORT)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    main()
