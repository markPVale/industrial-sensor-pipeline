# Store-and-Forward Status

Last updated: 2026-10-04

## Delivery-lease implementation update

Buffer pin operations, bounded RAM LossJournal, and DeliveryLease are implemented.
`connectionTask` exclusively pins, publishes, retries, commits matching ACKs, and
performs counted abandonment and owns pacing. The cadence-only telemetry/sync
tasks have been removed.
The R0/R1 stale-ACK invariant now passes against the production-used controller;
delivery no longer uses split peek/pop. Both producers journal overflow losses.
There is no diagnostic publisher or bridge reconciliation yet, and no new
hardware validation. Historical results below do not validate the new controller.

A pin survives disconnect and overflow (second-oldest eviction, or newest
rejection at capacity one). Five publish calls now switch to slow probes at a
30-second completion-to-next-attempt interval; they do not cap attempts or discard
records while space remains. Initial failed publishes back off at 1/2/4/8 seconds,
and successful attempts receive a 3-second ACK window. Abandonment requires actual
full-buffer pressure and a resolved attempt; fullness and matching-pin release
are one atomic operation. Observed connected time is diagnostic only. New MQTT
sessions owe a fresh paced attempt before abandonment, without resetting counters
or cooldowns. Matching eligible ACKs win in the same iteration. Abandonment after
publishing means delivery is unknown, not necessarily lost. These defaults are
software-tested, not hardware-tuned.

The previous five-call/15-second rule discarded 40 records in a simulated ten-minute
bridge outage with free buffer space. The regression now retains all 1,201 produced
records and drains them when the bridge recovers without any MQTT reconnect.
Repeatedly interrupted sessions can still delay pressure abandonment indefinitely;
fair retries do not promise bounded drain progress under connection churn.

The 2026-10-04 integration fixes add DeliveryRuntime: missing connect events no
longer strand BUFFERING; failed subscriptions retry with bounded backoff while
the pin is preserved; NORMAL backlogs enter SYNCING; ACK resolution does not
discard an eligible send. A simulated 150 ms ACK round trip permits 150 ms sends
in SYNCING rather than 200 ms. A one-time lost ACK catches up under continuous
2 Hz production. These are software-model results, not measured hardware rates.

A follow-up regression exposed healthy-mode flapping when a 500 ms cooldown
started at publish completion. NORMAL now follows production; both modes use a
100 ms minimum between new-record publish starts, without saved timer ticks.
Ten-minute simulations exercise variable publish costs, ACK latency, producer
phase and loop jitter. Retries remain governed by lease timeout/backoff, not this
new-record limiter. Queue depth is now four for the E-Stop-only publish queue.

The device now emits cumulative delivery/subscription counters to serial every
five seconds. Repeated internal validation failures keep the pin and use the
same rate-limited reporting, rather than printing every loop. Subscription
readiness means the request was written, not broker SUBACK confirmation. MQTT
diagnostic publishing and checker reconciliation remain pending. Mode entries
and subscription outcomes are additionally logged after runtime service, so
short-lived transitions do not disappear between five-second summaries.

The journal retains at most 64 closed events plus one open range. Once full, it
drops new closed events and increments `diagnostic_events_lost`; counters retain
magnitude but not the missing identities. Journal events and counters are
RAM-only and are lost on reboot. A boot boundary explains a sequence reset,
not an earlier gap. In particular, loss of an after-publish abandonment event
can leave a genuinely missing record unexplained, even if an abandonment count
was observed. See delivery-lease design §8.1 and §9 D2 for reconciliation limits.

See [the delivery-lease design](delivery-lease-design.md) and
[roadmap](roadmap.md) for remaining diagnostics and hardware validation work.

## Current Status

The firmware has a PSRAM-backed store-and-forward path:

```text
NORMAL -> BUFFERING -> SYNCING -> NORMAL
```

Records are produced at about 2 Hz and stored in the PSRAM ring buffer before
being published over MQTT.

During a broker outage test on 2026-06-07, before the app-level ACK path was
implemented, `integrity_check.py` found a sequence gap after Mosquitto was
stopped and restarted:

```text
seq 5531 -> 5594
```

That showed the previous implementation removed records from the PSRAM buffer
too early.

## Shortcut Fix Implemented — Superseded

> **Superseded by "App-Level ACK Implemented" below.** This section records the
> first attempt, which moved the commit point to `publish()` success. That is no
> longer the commit point. It was replaced by app-level ACKs, and subsequently
> the delivery lease described above. Kept for history, not current behavior.

The firmware source moved the buffer commit point closer to the actual MQTT
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

## App-Level ACK Implemented — Historical, Superseded by Delivery Lease

The procedure below describes the pre-lease firmware. It does not describe
current pin ownership, disconnect behavior or counted abandonment. See the
implementation update above for current behavior; the bridge ACK contract remains.

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

The ACK-gated path has a recorded passing hardware outage result, but its raw
evidence was not retained and the repeat test below remains open. This is also
not an unbounded guaranteed-delivery system: delivery is constrained by the
finite PSRAM buffer, and duplicate delivery is still possible when an ACK is
lost after a successful InfluxDB write.

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

Duplicate delivery is tolerated, but it is not modeled as a first-class storage
invariant yet. In the normal path, InfluxDB should collapse duplicate writes
because retries reuse the same measurement, `node_id` tag, and device timestamp.
However, `boot_id` and `sequence_id` are fields rather than tags, so there is no
explicit uniqueness key on `boot_id + sequence_id`.

The integrity checker now dedupes rows on `boot_id + sequence_id` before
validation and reports duplicates separately (see "Checker-Side Deduplication").
That resolves the validation ambiguity but not the underlying write: two rows
still exist in InfluxDB whenever a retry lands at a different timestamp.

The firmware itself does not cause that drift — `timestamp_ms` is captured once
at window-end and stored in the PSRAM record, so a retry re-serializes the same
value. The reachable case is the bridge's fallback: it only uses the firmware
timestamp when `ts > 1_000_000_000_000`, so a record buffered before NTP sync is
written at broker-arrival time, which differs on every delivery.

Hardening options:

```text
1. Add an explicit record-identity tag derived from boot_id + sequence_id.
   NOT DONE — and not planned. See below.
2. Make integrity_check.py dedupe by boot_id + sequence_id before validation.
   DONE — see "Checker-Side Deduplication".
```

A `boot_id + sequence_id` tag is not being added because it would create one
series per record and still would not collapse retries written at different
timestamps: an InfluxDB point's identity is measurement + tag set + field key +
timestamp, so adding tags makes points more distinct, not less. Checker-side
deduplication handles the validation ambiguity; fixing the pre-NTP timestamp
fallback remains separate work.

## Checker-Side Deduplication

`integrity_check.py` now deduplicates records on `(boot_id, sequence_id)` before
running the validation checks:

```text
- Records are keyed on (boot_id, sequence_id); _measurement is deliberately
  excluded, since telemetry and fault records share one identity stream and the
  same identity in both is itself a defect.
- The earliest stored _time is kept as canonical. Note that _time is the data
  timestamp, not the write time, so the kept row is canonical by convention
  rather than provably the original write.
- Duplicates are reported in their own section, split by severity:
    RETRY (same measurement)   -> WARN, does not fail the run.
    IDENTITY COLLISION (differing measurements) -> FAIL.
  The count appears in the summary either way.
- Sequence, timestamp, and fidelity checks all run on the deduplicated set, so
  a reported gap now means actual data loss. The old label was GAP/DUP because
  the check could not tell the two apart; it is now GAP.
```

A duplicate surfacing here means the same identity was written twice at
different timestamps and/or in different measurements.

The two cases are not equally benign. A same-measurement repeat is a tolerated
retry — at-least-once delivery permits it — and its likely cause is the bridge's
broker-arrival-time fallback for pre-NTP-sync records, whose differing
timestamps prevent InfluxDB from collapsing the write. A replayed record looks
identical.

The same identity in two measurements is a hard failure. The bridge routes on
payload flags (`write_telemetry` delegates to `write_sensor_fault` when
`flags & FAULT_FLAGS_MASK`), and a retry re-sends the same buffered record with
the same flags, so a retry cannot change measurement. A collision therefore
means a reused sequence number or a corrupted payload, not duplicate delivery.

## Throughput Tradeoff

Sending one record at a time reduces SYNCING drain throughput. Under the
ACK-gated path the drain rate is bounded by the publish → InfluxDB write → ACK
round trip, not by the loop delay alone.

Previously intended drain:

```text
SYNC_BATCH_SIZE = 20 records per 100 ms
about 200 records/sec
```

The unused `SYNC_BATCH_SIZE` setting has been removed. Multi-in-flight delivery
requires a separate design; the current lease protects one record at a time.

Healthy drain, ACKs arriving well inside `DELIVERY_MIN_PUBLISH_INTERVAL_MS`:

```text
1 buffered record in flight at a time
about 1 record per 100 ms
about 10 records/sec
```

At 2 Hz sampling, a short outage catches up quickly in that regime:

```text
10 second outage ~= 20 buffered records
10 records/sec drain ~= 2-3 seconds to catch up
```

Degraded drain, ACKs being lost — each retry waits for the timeout
(`MQTT_ACK_TIMEOUT_MS = 3000`, `firmware/include/config.h:109`):

```text
1 record per 3000 ms
about 0.33 records/sec
```

That is below the 2 Hz production rate, so under sustained ACK loss the PSRAM
buffer grows during SYNCING rather than draining. It only recovers when ACKs
resume; if they do not, the buffer fills and the oldest records are overwritten.
Longer outages take proportionally longer to drain even in the healthy regime.

## Historical Validation — Shortcut Fix — 2026-06-09

This controlled outage test was run after flashing the shortcut fix but before
validating the app-level ACK implementation.

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

This result showed that the shortcut fix alone could not support a no-data-loss
claim. It is retained here as historical evidence of the failure that motivated
the app-level ACK path.

## ACK-Gated Hardware Validation — 2026-06-24

The app-level ACK implementation has a recorded Raspberry Pi 5 + ESP32-S3 test
result with the bridge ACKing each record only after its InfluxDB write
completed.

Recorded results:

```text
Baseline run:
  234 records
  0 sequence gaps
  timestamp monotonicity: PASS
  data fidelity: PASS

Controlled Mosquitto outage/restart:
  570 records
  0 sequence gaps
  timestamp monotonicity: PASS
  data fidelity: PASS

ACK path:
  Matching sensor/node01/ack observed after each InfluxDB write
```

Evidence note: the raw terminal output or screenshot from this run is not
present in the repository. The values above are the recorded validation result,
not a reproducible artifact. Future hardware validation runs should commit the
raw `integrity_check.py` output alongside the summarized result.

This recorded passing run supersedes the June 9 shortcut-fix result for the
current ACK-gated implementation, but it is pending an evidence-backed repeat.
Even if reproduced, it validates only the controlled outage scenario and tested
buffer depth; it does not remove the finite-buffer and duplicate-delivery
limitations described above.

## TODO — Repeat ACK-Gated Hardware Validation

- [ ] Record the firmware commit, gateway commit, hardware versions, InfluxDB
  version, broker version, configured buffer size, and test date.
- [ ] Capture a baseline `integrity_check.py` run with sequence integrity,
  timestamp monotonicity, and data fidelity all passing.
- [ ] Stop Mosquitto for a recorded duration while the ESP32 continues sampling.
- [ ] Restart Mosquitto and capture the ESP32 transition from `BUFFERING` through
  `SYNCING` back to `NORMAL`.
- [ ] Run `integrity_check.py` over a window covering the full baseline, outage,
  reconnect, and drain; require zero sequence gaps and data fidelity PASS.
- [ ] Capture ACK-topic evidence showing matching `boot_id + sequence_id` values
  after successful InfluxDB writes.
- [ ] Commit the raw terminal output under
  `docs/validation/store-and-forward-YYYY-MM-DD.txt`, then update the summarized
  result and mark this TODO complete.

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
  - A 2026-06-24 outage run is recorded as passing, without retained raw output.
  - Evidence-backed hardware re-validation is still open.
```

Current recommendation:

```text
Keep the ACK-gated implementation and repeat the outage test after changes to
the firmware, bridge, broker configuration, or telemetry schema. Preserve raw
integrity-check output for future validation runs, and harden duplicate identity
before treating delivery as a storage-level exactly-once guarantee.
```
