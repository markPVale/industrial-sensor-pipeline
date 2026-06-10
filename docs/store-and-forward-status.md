# Store-and-Forward Status

Last updated: 2026-06-09

## Current Status

The firmware has a PSRAM-backed store-and-forward path:

```text
NORMAL -> BUFFERING -> SYNCING -> NORMAL
```

Records are produced at about 2 Hz and stored in the PSRAM ring buffer before
being published over MQTT.

During a broker outage test on 2026-06-07, `integrity_check.py` found a sequence
gap after Mosquitto was stopped and restarted:

```text
seq 5531 -> 5594
```

That showed the previous implementation removed records from the PSRAM buffer
too early.

## Shortcut Fix Implemented

The firmware source now moves the buffer commit point closer to the actual MQTT
publish attempt.

Old behavior:

```text
telemetryTask/syncTask:
  peek oldest record from g_buffer
  serialize payload
  enqueue MQTT message into g_publishQueue
  pop record from g_buffer immediately
```

New behavior:

```text
telemetryTask/syncTask:
  peek oldest record from g_buffer
  serialize payload
  enqueue MQTT message with boot_id + sequence_id
  leave record in g_buffer

connectionTask:
  receive queued MQTT message
  call publish()
  if publish succeeds and message identity matches the current buffer head:
    pop record from g_buffer
  if publish fails:
    leave record in g_buffer for retry
```

The new `g_bufferRecordInFlight` guard allows one buffered telemetry record to
be in flight at a time. This avoids enqueueing the same oldest buffered record
repeatedly while `connectionTask` has not yet committed or rejected it.

Build status:

```text
pio run: SUCCESS
```

## App-Level ACK Implemented

The firmware and bridge now include an app-level acknowledgement path.

ACK topic:

```text
sensor/node01/ack
```

Bridge behavior:

```text
1. Receive telemetry or fault record.
2. Write the record synchronously to InfluxDB.
3. After the write succeeds, publish:
   {"boot":<boot_id>,"seq":<sequence_id>}
   to sensor/node01/ack at QoS 1.
```

Firmware behavior:

```text
1. Subscribe to sensor/node01/ack at QoS 1 after each MQTT connect.
2. Send one buffered record at a time.
3. Keep the record in PSRAM after publish().
4. Pop the buffer only after a matching ACK arrives.
5. If no ACK arrives within MQTT_ACK_TIMEOUT_MS, clear in-flight state and
   retry the same buffer head.
6. On disconnect, clear in-flight state and retry after reconnect.
```

Normal telemetry and sensor fault records are both ACKed because they share the
same `boot_id + sequence_id` stream. E-Stop event messages are not part of the
telemetry buffer and are not covered by this ACK path.

Implementation status:

```text
python3 -m py_compile gateway/bridge/mqtt_to_influx.py gateway/bridge/integrity_check.py: PASS
pio run: SUCCESS
```

## Important Limitations

This is not true guaranteed delivery until hardware validation passes.

The firmware publish leg still uses QoS 0 style behavior:

```text
publish() == true
```

means the local MQTT client accepted the publish attempt. The new app-level ACK
path adds a stronger commit point:

```text
bridge wrote the record to InfluxDB
bridge published ACK
firmware received matching ACK
```

If the ACK is dropped, firmware retries after `MQTT_ACK_TIMEOUT_MS`.

## Throughput Tradeoff

The shortcut reduces SYNCING drain throughput.

Previous intended drain:

```text
SYNC_BATCH_SIZE = 20 records per 100 ms
about 200 records/sec
```

Current effective drain:

```text
1 buffered record in flight at a time
about 1 record per 100 ms
about 10 records/sec
```

At 2 Hz sampling, a short outage should still catch up quickly:

```text
10 second outage ~= 20 buffered records
10 records/sec drain ~= 2-3 seconds to catch up
```

Longer outages will visibly take longer to drain.

## Validation Result — 2026-06-09

Controlled outage test run after flashing the shortcut fix.

Baseline (pre-outage):

```text
integrity_check.py --minutes 2: all checks PASS
```

Outage procedure:

```text
- Stopped Mosquitto
- Restarted Mosquitto
- ESP32 serial confirmed: BUFFERING → reconnect → SYNCING → ~60 records drained → NORMAL
```

Post-outage result:

```text
integrity_check.py --minutes 5: FAIL
  seq 623 -> 628  (~4 missing records, 2500ms timestamp gap)
```

Comparison:

```text
Before shortcut fix:  ~63 records lost
After shortcut fix:   ~4 records lost
```

Conclusion: the shortcut fix measurably reduces loss but does not eliminate
it. The remaining gap is consistent with QoS 0 semantics — `publish()` returned
true for those records but they did not reach InfluxDB. The likely failure points
are the TCP/broker boundary or the broker-to-bridge leg (bridge subscribes at
QoS 0 with clean_session=True and may not have resubscribed before the drain
messages were published).

Do not claim "no data loss" or "continuity preserved" through broker outage
with the current firmware. Use the integrity checker as a live demo tool to
show the gap exists and to verify when a stronger fix closes it.

## Remaining Engineering Options

```text
Option A: MQTT QoS 1 + PUBACK tracking
  - Requires replacing PubSubClient (does not support QoS 1 publish acks).
  - Pop from PSRAM only after broker PUBACK.
  - Needs in-flight record tracking and duplicate handling downstream.
  - Only guarantees broker custody, not InfluxDB persistence.
  - LOE: ~7-12 days (medium-high risk — client library swap).

Option B: App-level acknowledgement
  - Implemented in firmware and bridge.
  - Needs hardware outage validation.
```

Current recommendation:

```text
Flash and validate the app-level ACK implementation on hardware.
Acceptance gate:
  - Baseline integrity PASS.
  - Controlled Mosquitto outage.
  - Reconnect and drain.
  - integrity_check.py sequence integrity PASS.
  - data fidelity PASS.
```
