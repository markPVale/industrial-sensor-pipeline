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

STATUS_SENSOR_FAULT             = 0x10
STATUS_DEGRADED_REBOOT_REQUIRED = 0x20
STATUS_SENSOR_UNAVAILABLE       = 0x40
FAULT_FLAGS_MASK = STATUS_SENSOR_FAULT | STATUS_DEGRADED_REBOOT_REQUIRED | STATUS_SENSOR_UNAVAILABLE

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


def publish_ack(client: mqtt.Client, node_id: str, payload: dict) -> None:
    """ACK a telemetry/fault record after it has been written to InfluxDB."""
    ack = json.dumps(
        {"boot": int(payload.get("boot", 0)), "seq": int(payload.get("seq", 0))},
        separators=(",", ":"),
    )
    info = client.publish(f"sensor/{node_id}/ack", ack, qos=1)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        raise RuntimeError(f"failed to publish ACK rc={info.rc}")
    log.debug("ACK published for node %s boot=%s seq=%s",
              node_id, payload.get("boot", 0), payload.get("seq", 0))


def write_sensor_fault(client: mqtt.Client, node_id: str, payload: dict) -> None:
    """Write a sensor fault record (I2C dropout) to the sensor_faults measurement."""
    # whoami_raw: firmware encodes the raw WHO_AM_I register byte in ax during
    # fault records (0xFF = bus not responding). Store under a clear name.
    point = (
        Point("sensor_faults")
        .tag("node_id", node_id)
        .field("boot_id",     int(payload.get("boot", 0)))
        .field("sequence_id", int(payload.get("seq",  0)))
        .field("flags",       int(payload.get("flags", STATUS_SENSOR_FAULT)))
        .field("reason",      "i2c_dropout")
    )
    whoami = finite_measurement(payload.get("ax"))
    if whoami is not None:
        point.field("whoami_raw", int(whoami))
    else:
        point.field("unavailable_fields", "ax")
    ts = int(payload.get("ts", 0))
    if ts > 1_000_000_000_000:
        point = point.time(ts, WritePrecision.MS)
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    publish_ack(client, node_id, payload)
    log.warning("Sensor fault written for node %s seq=%d", node_id, payload.get("seq", -1))


def finite_measurement(value):
    """Unavailable is not zero. Reject wrong types; tolerate null/nonfinite."""
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("measurement must be numeric or null")
    value = float(value)
    return value if math.isfinite(value) else None


def write_telemetry(client: mqtt.Client, node_id: str, payload: dict) -> None:
    """
    Write a telemetry record to InfluxDB.

    Payload shape follows docs/telemetry-schema.md.
    vibration_rms is derived here as sqrt(ax² + ay² + az²) for legacy Grafana
    panels. window_rms is the firmware rolling RMS used by anomaly detection.
    Records with STATUS_SENSOR_FAULT are routed to sensor_faults instead.
    """
    # Never invent an identity for evidence or ACK a coercible/wrong identity.
    for key in ("boot", "seq"):
        value = payload.get(key)
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise ValueError(f"invalid {key} identity")
    if int(payload.get("flags", 0)) & FAULT_FLAGS_MASK:
        write_sensor_fault(client, node_id, payload)
        return

    values = {key: finite_measurement(payload.get(key))
              for key in ("ax", "ay", "az", "gx", "gy", "gz", "wrms")}
    axes = [values[key] for key in ("ax", "ay", "az")]
    # hypot avoids intermediate overflow. Still omit an unrepresentable result.
    magnitude = math.hypot(*axes) if all(v is not None for v in axes) else None
    if magnitude is not None and not math.isfinite(magnitude):
        magnitude = None
    # Legacy payloads WITHOUT wrms keep their old fallback. Explicit null does
    # not claim a measured window RMS and must remain absent in the database.
    window_rms = values["wrms"] if "wrms" in payload else magnitude

    flags = int(payload.get("flags", 0))

    point = (
        Point("vibration")
        .tag("node_id", node_id)
        # Record identity
        .field("boot_id",      int(payload.get("boot", 0)))
        .field("sequence_id",  int(payload.get("seq",  0)))
        .field("flags",        flags)
    )
    for key in ("ax", "ay", "az", "gx", "gy", "gz"):
        if values[key] is not None:
            point.field(key, values[key])
    if magnitude is not None:
        point.field("vibration_rms", magnitude)
    if window_rms is not None:
        point.field("window_rms", window_rms)
    unavailable = [key for key, value in values.items() if value is None]
    point.field("data_complete", not unavailable)
    if unavailable:
        point.field("unavailable_fields", ",".join(unavailable))
    if all(value is None for value in values.values()):
        # Persist identifiable invalid-data evidence, not fabricated readings.
        # Keep the measurement deterministic across retries and preserve flags.
        point.field("reason", "invalid_telemetry")
    # Use firmware timestamp when it looks like a real epoch (> year 2001 in ms).
    # Falls back to broker-arrival time (InfluxDB default) during pre-NTP-sync window.
    ts = int(payload.get("ts", 0))
    if ts > 1_000_000_000_000:
        point = point.time(ts, WritePrecision.MS)

    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    publish_ack(client, node_id, payload)
    if unavailable:
        log.warning("Incomplete telemetry persisted for %s boot=%s seq=%s fields=%s",
                    node_id, payload["boot"], payload["seq"], ",".join(unavailable))
    log.debug("Written telemetry for %s: wrms=%s m/s² flags=0x%02x seq=%d",
              node_id, window_rms, flags, payload.get("seq", -1))


def write_estop(node_id: str, payload: dict) -> None:
    """Write an E-Stop event to InfluxDB."""
    point = (
        Point("estop_event")
        .tag("node_id", node_id)
        .field("triggered", int(payload.get("triggered", 1)))
        .field("reason", str(payload.get("reason", "unknown")))
    )
    ts = int(payload.get("ts", 0))
    if ts > 1_000_000_000_000:
        point = point.time(ts, WritePrecision.MS)
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
            write_telemetry(client, node_id, payload)
        elif msg_type == "estop":
            write_estop(node_id, payload)
        else:
            log.debug("Unhandled message type '%s' on topic %s", msg_type, msg.topic)
    except Exception as e:
        log.error("Failed to process MQTT message on %s: %s", msg.topic, e)


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
