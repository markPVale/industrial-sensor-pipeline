# Store-and-Forward Status

Last updated: 2026-06-08

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

## Important Limitations

This is not true guaranteed delivery.

Current MQTT behavior is still QoS 0 style:

```text
publish() == true
```

means the local MQTT client accepted the publish attempt. It does not prove:

```text
broker acknowledged the record
bridge wrote it to InfluxDB
Grafana/MCP can query it later
```

The shortcut fixes the obvious "pop on enqueue" loss path, but it is not a
replacement for acknowledgement-based delivery.

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

## Validation Needed

Before claiming telemetry continuity in a demo:

```text
1. Flash the updated firmware.
2. Start Pi gateway stack.
3. Start mqtt_to_influx.py bridge.
4. Confirm live telemetry in Grafana.
5. Stop Mosquitto for 5-10 seconds.
6. Restart Mosquitto.
7. Wait for SYNCING drain to complete.
8. Run integrity_check.py --minutes 5.
9. Require sequence integrity PASS and data fidelity PASS.
```

If sequence integrity fails, do not claim continuity. Use the failure to show
that the integrity checker detects delivery gaps.

## Next Engineering Options

Near-term cleanup:

```text
Update syncTask comments/loop structure so SYNC_BATCH_SIZE no longer implies
20 records can be drained per cycle while the single in-flight guard is active.
```

Longer-term reliability options:

```text
Option A: MQTT QoS 1 + PUBACK tracking
  - Requires MQTT client support for QoS 1 acknowledgements.
  - Pop from PSRAM only after broker PUBACK.
  - Needs in-flight record tracking and duplicate handling.

Option B: App-level acknowledgement
  - Bridge sends an ACK only after writing to InfluxDB.
  - Proves the persistence point we actually care about.
  - Avoids replacing the MQTT client but requires an ACK topic/protocol.
```

Current recommendation:

```text
Validate the shortcut fix first.
Then decide between MQTT QoS 1 and app-level ACK based on the test result and
desired reliability guarantee.
```
