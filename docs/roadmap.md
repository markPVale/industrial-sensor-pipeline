# Engineering Roadmap

Last reviewed: **2026-09-12**

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

Status: design complete; implementation open.

Detailed design and test matrix:
[`delivery-lease-design.md`](delivery-lease-design.md). Implementation order is
intentional because the early tests must reproduce the current race before the
state model is replaced.

- [ ] Check the PubSubClient transport-buffer allocation, add the required
  compile-time size assertion, and surface allocation failure.
- [ ] Add a native firmware test harness and a behavior-preserving seam around
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
- [ ] Revisit durable diagnostic journaling if RAM-only loss events create
  unexplained gaps during hardware validation; batch NVS writes to control flash
  wear.

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
