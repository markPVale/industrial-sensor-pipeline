# Engineering Roadmap

Last reviewed: **2026-09-25**

This is the canonical list of open engineering work for the repository. Component
documents remain authoritative for design details and test procedures, but new
work should be represented here so priorities and status can be reviewed in one
place.

Status convention:

- `[ ]` — open
- `[x]` — complete
- **Now** — correctness or validation work in the current delivery sequence
- **Next** — planned after the current reliability work
- **Later** — valuable, but not committed to the current sequence

## Now

### ACK-safe delivery lease

Status: design complete; implementation in progress.

Detailed design and test matrix:
[`delivery-lease-design.md`](delivery-lease-design.md). Implementation order is
intentional because the early tests must reproduce the current race before the
state model is replaced.

- [x] Check the PubSubClient transport-buffer allocation, add the required
  compile-time size assertion, and surface allocation failure.
- [x] Add a native firmware test harness and a behavior-preserving seam around
  the legacy ACK state; reproduce the R0/R1 stale-ACK race as a failing test.
- [ ] Add the `BufferManager` pin API, second-oldest eviction, `PushOutcome`,
  counters, and bounded thread-safe `LossJournal`.
- [ ] Move delivery ownership into a single lease controller in
  `connectionTask`; reduce `telemetryTask` and `syncTask` to cadence signals.
- [ ] Add loss-range and lease-abandonment diagnostics, cumulative counters,
  bridge routing, exact gap attribution, and abandonment reconciliation.
- [ ] Tune abandonment thresholds during hardware validation.

### Evidence-backed store-and-forward validation

Status: the historical test is recorded, but its raw evidence was not retained.
Run this after the delivery-lease and diagnostic work above.

- [ ] Complete the hardware checklist in
  [`store-and-forward-status.md`](store-and-forward-status.md), under
  “TODO — Repeat ACK-Gated Hardware Validation.”
- [ ] Commit raw output under `docs/validation/` and update the recorded result.

### Coverage-aware anomaly results

Status: correctness gap in the current MCP tool. An empty anomaly query currently
conflates healthy telemetry with absent or incomplete telemetry.

- [ ] Query ordinary telemetry coverage independently of flagged anomaly and
  fault events.
- [ ] Evaluate coverage at the start and end of the requested window, current
  freshness, and unexplained internal timestamp or identity gaps.
- [ ] Return an explicit outcome: `anomalies_found`, `no_anomalies`, or
  `insufficient_data`.
- [ ] Permit `no_anomalies` only when the requested interval has adequate
  coverage; offline and partially observed intervals remain inconclusive.
- [ ] Test empty, partial, stale, complete-normal, anomaly-present, delayed
  store-and-forward, and InfluxDB-failure cases.

### Pre-NTP retry timestamps

- [ ] Fix the bridge fallback that timestamps pre-NTP records at broker arrival.
  A retry can currently acquire a different timestamp and create a second
  InfluxDB point. Checker-side deduplication by `boot_id + sequence_id` is already
  complete, but it does not prevent the duplicate write.

### Sensor data plausibility

Status: correctness gap. Sensor health is checked only by a WHO_AM_I register
probe (`isMpuHealthy()`), which proves the chip answers on I2C, not that it is
producing measurements. A sensor that answers WHO_AM_I but outputs frozen,
repeated, zero, biased, or garbage data is published as healthy:

- `Adafruit_MPU6050::getEvent()` always returns `true` and ignores the result
  of its 14-byte burst read, so a failed read converts uninitialised stack
  bytes into accel and gyro values.
- An MPU-6050 that browns out or resets independently of the ESP32 powers up
  with `PWR_MGMT_1.SLEEP` set: it still answers `0x68`, but its data registers
  stop updating.
- Frozen at-rest output keeps `wrms` near the healthy ~10.2 m/s², and all-zero
  output drives it to ~0; the anomaly check is upper-bound only (`> 12.5`), so
  neither is flagged. Previously noted as a known gap in
  [`kalman-validation-v1.md`](claude-notes/kalman-validation-v1.md).

- [ ] Replace `getEvent()` with a direct burst read that checks the returned
  byte count, and treat a short read as a failed health check.
- [ ] Extend the health probe to check `PWR_MGMT_1.SLEEP` and the `INT_STATUS`
  data-ready bit, so an MPU that has self-reset or stopped sampling is
  detected; recovery via the existing re-init path should clear the sleep case.
- [ ] Detect stuck output: N consecutive raw samples identical on all six axes.
  Sensor noise makes this effectively impossible on a live sensor; measure the
  at-rest distribution to confirm before choosing N.
- [ ] Add per-window raw plausibility checks: variance below a measured floor
  (flatline or zero output) and mean accel magnitude outside a plausible band
  around 1 g (zero output or gross bias). Slow bias drift remains the gateway's
  calibration-drift item under Next.
- [ ] Route plausibility failures into the existing fault escalation
  (`0x20` → `0x40`) with a reason code in the fault record, following the
  existing convention of encoding WHO_AM_I in `ax`, rather than consuming the
  last free status bit. Coordinate with the transient-detection `0x80` decision
  and update [`telemetry-schema.md`](telemetry-schema.md), the bridge, and MCP
  fault decoding.
- [ ] Test on hardware: force the MPU into sleep over I2C while the ESP32 keeps
  running, and confirm detection, the reason code, and recovery; confirm no
  false positives across an at-rest and normal-operation soak.

## Next

### Configuration and operations

- [ ] Load Wi-Fi credentials, broker address, and client identity from NVS or a
  gateway-managed configuration path.
- [ ] Put the MCP server under a service manager or the gateway Compose stack so
  it survives Raspberry Pi restarts.
- [ ] Add OTA firmware updates and device identity/provisioning support.

### Buffering and diagnostics

- [ ] Add priority-tiered store-and-forward so routine telemetry is evicted
  before sensor-fault and interlock records.
- [ ] Emit a boot diagnostic containing the new `boot_id`, ESP reset reason,
  startup timestamp, and reset classification (power-on, brownout,
  intentional/software, watchdog, or other). Persist and expose it through the
  gateway so power interruptions are visible. This provides observability only;
  it does not recover telemetry lost from volatile PSRAM or cover time when the
  sensor had no power.
- [ ] Revisit durable diagnostic journaling if RAM-only loss events create
  unexplained gaps during hardware validation; batch NVS writes to control flash
  wear.

### Liveness and failure classification

Status: gap. Network loss and ESP32 failure are indistinguishable today: both
stop telemetry, and `get_sensor_health` reports `OFFLINE` after 30 s. There is no
MQTT Last Will, no heartbeat, and no boot diagnostic. While a node is silent the
cause cannot be determined from the gateway in principle; it is resolved only
when the node returns — same `boot_id` with a backfilled gap indicates a network
outage, a new `boot_id` with a reset reason indicates an ESP32 reset. The boot
diagnostic item above is therefore a prerequisite and should be scheduled with
this work.

- [ ] Configure an MQTT Last Will on a retained `sensor/<node>/status` topic,
  and publish a retained `online` message on each connect, so the broker records
  link loss within ~1.5 × keepalive (~22 s). This also covers an ESP32 that
  hangs without resetting.
- [ ] Persist status transitions in the bridge so outage start and end times
  are queryable after the fact.
- [ ] Add explicit health outcomes to the MCP tool: `sensor_implausible`,
  `link_lost` (cause unknown), `network_outage` (backfilled, same `boot_id`),
  and `node_reset` (with the reset reason). Align with the coverage-aware
  anomaly outcomes under Now.
- [ ] Test each case: broker firewall outage (existing iptables procedure),
  ESP32 power interruption, forced watchdog reset, and forced MPU sleep.

### Transient detection

Status: detection gap. Anomaly detection sees only Kalman-filtered data, and
accel spike rejection discards up to 7 consecutive samples (≤ 70 ms at 100 Hz)
more than 0.5g from the estimate. A short impact below the ±8g clip threshold
therefore never reaches the window RMS and is not flagged. The only raw-sample
check today is clip detection in `sensorTask`. Keep this work behind the
delivery-lease native harness because it changes `TelemetryRecord` and benefits
from testing the per-window calculations as pure code.

**Phase A — observe without adding a status flag:**

- [ ] Make each accelerometer filter report whether its most recent measurement
  update was actually skipped as a spike. The eighth consecutive outlier, which
  reinitialises and is accepted as a step change, is not a rejection. Count a
  sample once when any accel axis was rejected, giving an explicit
  `0..FILTER_WINDOW_SIZE` per-window value; do not derive it from lifetime or
  consecutive counters. Store it as `uint16_t` so the field is not coupled to
  the current 50-sample window.
- [ ] Add a pure, native-tested window-statistics component that tracks both the
  peak calibrated-but-unfiltered acceleration magnitude and peak innovation
  magnitude (measurement minus the filter's pre-update estimate). The innovation
  metric is the primary transient candidate because gravity and sensor
  orientation can hide impacts in absolute vector magnitude. Test peak tracking,
  rejection semantics, and reset at a window boundary.
- [ ] Add the numeric peak and rejection fields to `TelemetryRecord`; verify its
  target layout with `static_assert(sizeof(TelemetryRecord) == expected_size)`
  and update the PSRAM-capacity calculation and comments.
- [ ] Serialize the fields into MQTT and check `snprintf()`'s return value so a
  payload that exceeds `MQTT_PAYLOAD_SIZE` is detected rather than silently
  truncated. Add a worst-case serialization test; the MQTT packet-buffer
  assertion proves transport capacity only, not JSON formatting capacity.
- [ ] Update the bridge, mock publisher, and
  [`telemetry-schema.md`](telemetry-schema.md). When older payloads omit the new
  fields, omit those fields from the InfluxDB point rather than writing zero.
  A Grafana tuning panel is useful but not required for ingestion.

**Phase B — classify only after measurement:**

- [ ] Collect labeled data at rest, during timed tap tests, and during
  representative normal machine operation across expected speed, load,
  mounting, and temperature conditions. Measure the gyro noise profile in the
  same session before enabling its currently disabled spike threshold.
- [ ] Select and validate a threshold against held-out normal-operation data;
  require no observed false positives during the hardware soak and confirm that
  deliberate short impacts remain visible.
- [ ] Decide whether classification must happen on-device before consuming the
  final `status_flags` bit (`0x80`). Numeric metrics can drive downstream alerts
  without spending that bit. If `STATUS_TRANSIENT` is added, update firmware,
  schema, MCP flag constants and decoding tests, `get_recent_anomalies`, health
  priority, and any dashboard flag handling. The bridge fault-routing mask must
  remain unchanged.

### MCP and analytics

- [ ] Add `get_vibration_trend` using server-side bucketed min/mean/max with a
  response bound based on bucket count.
- [ ] Add calibration-drift tracking and fleet-level sensor-health reports.

### Dashboard

- [ ] Replace the fixed `node01` page with dynamically discovered nodes.
- [ ] Replace the temporary SVG sparkline with an appropriate charting library.

## Later

- [ ] Add multi-node fleet support: dynamic node IDs, unique client identities,
  per-node ACK topics, and fleet dashboards.
- [ ] Add an on-device anomaly model or edge-ML path.
- [ ] Consider collapsing the cadence-only `telemetryTask` and `syncTask` into
  `connectionTask` after the delivery-lease work is stable.
- [ ] Consider the deferred queued-publish generation contract if telemetry is
  moved back through an asynchronous publish queue.
- [ ] Consider bounded multi-record in-flight delivery if single-record ACK
  throughput becomes a measured constraint.

## Explicitly not planned

- A `boot_id + sequence_id` InfluxDB tag: it would create a series per record and
  would not collapse records written at different timestamps.
- Replacing the application-level InfluxDB ACK with MQTT PUBACK alone: broker
  custody does not prove that the bridge persisted the record in InfluxDB.
