# Firmware tests

Run these commands from `firmware/`. Host tests do not require an attached ESP32.

```sh
pio test -e native -f test_buffer_manager
```

Eight tests cover lifecycle guards, FIFO eviction, pin relocation and wraparound,
capacity-one rejection, identity-checked commit and abandonment, a deque model
at capacities 1–7, and concurrent producers with a pinned consumer. Every record
in the producer/commit threaded test must be accounted for exactly once as
committed or evicted. A second threaded test races filling the buffer with
conditional abandonment; a NOT_FULL deferral preserves the pin and identity.

```sh
pio test -e native -f test_loss_journal
```

Ten journal tests cover input validation, exact adjacency/policy/boot boundaries,
out-of-order identities, span and time limits, millis and sequence wrap, bounded
queue overflow, abandonment outcomes, forwarding actual BufferManager outcomes,
and concurrent producers/consumer. One threaded test uses single-record ranges
so retained events plus the lost-event counter must account for all observations.
A second races open-range updates against timer closure and checks every range's
exact membership with enough capacity to avoid overflow. Abandonment semantics
are tested deterministically; no test exhausts all possible thread schedules.

```sh
pio test -e native -f test_delivery_lease
pio test -e native
```

The delivery suite exercises the production-used DeliveryLease controller with
the real buffer and journal, an injected clock/publisher, and buffer-failure
injection. It covers the R0/R1 stale-ACK invariant, overflow during inline
publish, matching/mismatched/unpublished ACKs, late ACKs after failed retries,
timeout retries, disconnect/reconnect readiness accounting, exponential failure
backoff, continuing slow probes, full-buffer abandonment attribution, mutation
failures, and millis wrap.

All 68 tests (8 buffer + 10 journal + 22 delivery + 23 runtime + 5 payload) are expected to
pass and run in CI. The original failing regression and LegacyDeliveryState seam remain in git
at `c3c22bf`. They were replaced, not marked ignored or expected-failure. The
regression still asserts that R0's duplicate ACK cannot release unpublished R1;
its test-only injection point is now inside inline publishing after acquisition.
Production callbacks cannot interleave there, but buffer producers can.

```sh
pio test -e native -f test_delivery_runtime
```

This suite exercises the **production** coordinator called by main.cpp, not a
parallel simulation of its logic. It uses real buffer/journal/lease code with
injected transport, subscribe, publish and clock inputs. It covers lost connect
edges, a surviving transport without a new event, failed subscriptions and
blocking setup, readiness accounting, late ACKs, producer-paced delivery, 150 ms ACK latency,
NORMAL catch-up during continuous 2 Hz production, minimum start-to-start spacing,
timer wrap, fail-closed mutation errors, and rate-limited reporting. Production
timing defaults come from the shared `delivery_config.h`, not duplicate literals.

The sustained-production regression runs three ten-minute scenarios with different
producer phases, 1/8/15/22 ms publish costs, varying ACK latency and loop jitter.
It first failed against the completion-based scheduler (56 unnecessary catch-up
entries in the first scenario), then passed with producer-paced delivery. Other
tests cover nonzero publish duration across millis wrap, long service gaps without
banked sends, and short-lived mode-entry reporting. No telemetry/sync cadence
tasks or simulated cadence hints remain.

Pressure-policy regressions cover a ten-minute bridge outage with 2 Hz production
(1,201 records retained, zero abandonments), continued bounded probes, recovery
without reconnect, and new MQTT sessions giving a capped pin a fresh paced attempt.
Repeated sessions cannot reset cooldowns or counts. Full-buffer reconnect tests
exercise both local failures and a full ACK window on success, with ACK precedence
at the abandonment boundary. The old implementation failed the outage test with
40 abandonments and the capped reconnect test by abandoning before a fresh attempt.
The lease suite also checks the slow-probe countdown across millis rollover and
a new session without losing or resetting the remaining delay.

```sh
pio run -e esp32s3-n16r8
```

This compiles the actual ESP32 implementation, but does not flash or execute it.
`default_envs = esp32s3-n16r8` also makes bare `pio run` target the firmware.
Bare `pio test` selects the board and may attempt an upload; always specify
`-e native` for host tests. CI runs both the firmware build and native tests.

## Verification limits

- Native tests use host allocation and `std::mutex`, not PSRAM or FreeRTOS.
- The producer/commit threaded test allows protected overflow to interleave with commit, but
  does not force that schedule or assert that it occurred. A separate pressure
  test races producer fill with abandonment; neither test exhausts all schedules.
- Firmware now uses the controller and pin operations. These tests do not run
  main.cpp's FreeRTOS tasks, MQTT callbacks, actual socket reconnects, or setup
  failure handling. The coordinator's scheduling and readiness policy is tested,
  but those platform call sites are inspected/compiled, not exercised by a
  hardware integration test. Diagnostic publishing is still pending; serial
  summaries now expose counters every five seconds (not a fixed logging cost).
- No on-device validation of these pin operations has been performed.

On 2026-09-27, the seven buffer tests passed, the legacy regression reproduced
the expected failure, and the ESP32 build succeeded (RAM 14.2%, flash 11.9%).

On 2026-09-29, the ten journal tests and seven buffer tests passed together;
the ESP32 build succeeded (RAM 15.2%, flash 11.9%). No device was flashed.

The subsequent delivery-controller landing passed all 35 native tests, including
18 delivery tests and the now-green R0/R1 invariant. Its ESP32 build also passed
(RAM 49,788 bytes / 15.2%; flash 782,737 bytes / 11.9%). No hardware validation or
threshold tuning is implied by these software tests; the checklist remains open.

On 2026-10-04, all 47 tests passed and the ESP32 build succeeded (RAM 49,892 bytes /
15.2%; flash 783,477 bytes / 12.0%). No device was flashed. The previous 35-test
pass did not cover real cadence/subscription orchestration; this landing adds
that regression coverage without claiming hardware verification.

The subsequent 2026-10-04 pacing cleanup passed all 51 native tests and the ESP32
build (static RAM 49,876 bytes / 15.2%; flash 783,157 bytes / 12.0%). Removing two
4096-byte stacks and 36 publish-queue slots eliminates 22,052 bytes of requested
runtime allocation, plus task/event-group overhead. Those allocations are not
included in PlatformIO's static RAM percentage; actual free heap and remaining
task stack margins still need device measurements. No device was flashed.

The pressure-gated abandonment revision passed all 57 native tests and the ESP32
build (static RAM 49,884 bytes / 15.2%; flash 783,233 bytes / 12.0%). The regression
was observed failing before the policy change: 40 abandonments during a ten-minute
bridge outage, plus abandonment before a capped record's fresh reconnect attempt.
Both now pass. Software validation does not prove real broker/bridge behavior,
FreeRTOS scheduling or physical link failure detection; no device was flashed.

The nullable-payload revision adds `test_telemetry_payload` (normal formatting,
NaN/positive and negative infinity on all seven values, mixed valid/invalid,
extreme finite values, truncation and exact-fit boundaries). Runtime tests add
full-buffer recovery without a new session after an interrupted ACK wait,
pressure polling cadence, immediate ACK processing and pressure-timer rollover.
Run bridge tests with its requirements installed:
`python gateway/bridge/test_mqtt_to_influx.py` from the repo root; these use the
real Influx Point serializer and mocked writes/MQTT, not a live database.
Dashboard nullable-value checks run with `npm test` from `dashboard/`.

Validated on 2026-10-04: 65 native tests, 12 bridge tests, 11 integrity-checker
tests, nine dashboard helper assertions and dashboard TypeScript checking passed.
The ESP32 build succeeded (static RAM 49,884 bytes / 15.2%; flash 783,357 bytes /
12.0%). No device was flashed and no live broker/database was used. Deploy the
bridge before nullable-axis firmware. Full-window coverage analysis remains
open in the roadmap, as does the separate permanent-rejection protocol design.

The 2026-10-05 ownership regression was first observed failing when extended
past the 30-second retry boundary. The latch now blocks retries until matching
pin validation succeeds. Tests cover 60 seconds of injected failures, reconnect
churn, a genuinely missing/replaced pin, explicit recovery, and ACK recovery
during the validation cooldown. MCP tests exercise the production tools with
injected database rows/clock, including all-invalid and partially valid records,
missing window RMS, fault priority and no-data responses. These do not execute
Flux against a live database or replace the hardware validation checklist.

Verification for that revision: all 68 native and 24 MCP tests passed; MCP and
ESP32 builds succeeded (static RAM 49,884 bytes; flash 783,505 bytes). No device
was flashed. Full-window coverage analysis and loss-diagnostic publishing remain
separate unfinished work.
