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
