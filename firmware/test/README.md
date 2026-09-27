# Firmware tests

Run these commands from `firmware/`. Host tests do not require an attached ESP32.

```sh
pio test -e native -f test_buffer_manager
```

Seven tests cover lifecycle guards, FIFO eviction, pin relocation and wraparound,
capacity-one rejection, identity-checked commit and abandonment, a deque model
at capacities 1–7, and concurrent producers with a pinned consumer. Every record
in the threaded test must be accounted for exactly once as committed or evicted.

```sh
pio test -e native -f test_legacy_delivery
```

This suite deliberately fails the R0/R1 duplicate-ACK regression against the
production-used `LegacyDeliveryState`. Its matching-ACK control passes. The
local PlatformIO runner also reports SIGHUP after the failed assertion; the
assertion message is the evidence of the known bug. Do not suppress this failure
or treat the combined `pio test -e native` run as green. The regression must pass
when the delivery controller replaces the legacy state.

```sh
pio run -e esp32s3-n16r8
```

This compiles the actual ESP32 implementation, but does not flash or execute it.
`default_envs = esp32s3-n16r8` also makes bare `pio run` target the firmware.
Bare `pio test` selects the board and may attempt an upload; always specify
`-e native` for host tests. Native tests are not currently run by CI.

## Verification limits

- Native tests use host allocation and `std::mutex`, not PSRAM or FreeRTOS.
- The threaded test allows protected overflow to interleave with commit, but
  does not force that schedule or assert that it occurred. Concurrent abandonment
  is not tested; abandonment is exercised in the single-threaded tests.
- Passing buffer tests does not fix the live ACK path. Firmware does not acquire
  pins yet, and LossJournal / producer loss forwarding remain unimplemented.
- No on-device validation of these pin operations has been performed.

On 2026-09-27, the seven buffer tests passed, the legacy regression reproduced
the expected failure, and the ESP32 build succeeded (RAM 14.2%, flash 11.9%).
