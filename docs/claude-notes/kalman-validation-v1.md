# Kalman Filter — Validation Results v1

_Harness: `tools/validate_kalman.py` — Python mirror of `firmware/lib/KalmanFilter/KalmanFilter.cpp`_

---

## Test Run Summary

**Result: 7/7 PASS**

| # | Test | Result | Key Metric |
|---|------|--------|------------|
| 1a | Convergence — filter tracks raw mean | ✅ PASS | mean_filtered=51.073, mean_raw=51.145 (delta=0.07) |
| 1b | Convergence — variance reduced | ✅ PASS | var_raw=19.57 → var_filtered=2.29 (8.5× reduction) |
| 2 | Spike rejection — single-sample outliers dampened | ✅ PASS | max residual at spike=0.0094g (threshold=0.5g) |
| 3 | Step change — genuine level change tracked | ✅ PASS | settled_mean=1.806, expected=1.800 |
| 4a | NaN/Inf discarded, state preserved | ✅ PASS | estimate unchanged after 3 bad inputs |
| 4b | Filter continues after bad inputs | ✅ PASS | normal update resumes immediately |
| 5 | Parameter sweep (informational) | ✅ PASS | see table below |

---

## Parameter Sweep Results

Conditions: 100 samples, true_value=0.3g, noise_std=0.05g. Stats over final 20 samples.

| Config | Final Mean | Final Variance | Interpretation |
|--------|-----------|----------------|----------------|
| Q=0.001, R=0.5 | 0.3027 | 0.000025 | Very smooth — good for slow drift, **dangerous lag for safety events** |
| Q=0.01, R=0.1 (default) | 0.3048 | 0.000464 | Balanced — tracks 100Hz vibration well |
| Q=0.1, R=0.1 | 0.3073 | 0.001143 | More responsive — higher noise pass-through |
| Q=0.5, R=0.01 | 0.3071 | 0.002021 | Very reactive — essentially raw with smoothing |

**Chosen defaults: Q=0.01, R=0.1.** Tune once real MPU-6050 data is available.

---

## Design Decisions Validated

### Convergence criterion (test 1 fix)
Initial test checked `abs(mean_filtered - true_value) < 0.5`. This failed because a Kalman filter cannot correct sample bias — with noise_std=5.0 and 50 tail samples, the raw mean can naturally drift 1.0+ from true. Corrected to `abs(mean_filtered - mean_raw) < 0.5`, which tests the right thing: the filter tracks the actual signal without lag.

### Spike rejection threshold (0.5g for vibration)
- NORMAL vibration RMS ≈ 0.30g ± 0.02g
- ANOMALY vibration RMS ≈ 1.80g
- Sensor glitch / EMI spike: up to 10g, single sample

At threshold=0.5g, single-sample glitches at 5g are rejected (residual=0.009g). Genuine anomaly ramp-up (multiple consecutive samples above 0.5g) triggers step-change detection after `kMaxConsecutiveRejects=8` rejections and re-initialises. This means a genuine anomaly is accepted within 8 samples = 80ms at 100Hz — fast enough for industrial vibration monitoring.

### kMaxConsecutiveRejects = 8
- 8 samples at 100Hz = 80ms before a sustained step is accepted
- Single glitch: rejected indefinitely (returns current estimate)
- Genuine step change: accepted at 80ms — acceptable lag for vibration anomaly detection
- **Not acceptable for safety interlock** — the E-Stop path bypasses the filter entirely (raw GPIO interrupt)

### NaN / Inf handling
- Inputs checked with `isfinite()` before any arithmetic
- Bad inputs do not advance P (no uncertainty growth from no-data)
- Filter resumes normal operation on next finite input — no reset required

---

## Limitations & Known Tuning Gaps

| Gap | Impact | Resolution |
|----|--------|-----------|
| Q and R chosen empirically | May be suboptimal for real MPU-6050 | Capture stationary sensor data, compute variance → set R; tune Q to taste |
| spike_threshold=0.5g chosen for vibration axis | Not validated for gyro axes (rad/s units) | Set separate thresholds per axis once real data available |
| Flatline detection not implemented in filter | Sensor dropout looks like perfect reading | Detect at task level: if variance of last N samples < epsilon, flag sensor fault |
| No unit tests for C++ directly | C++ relies on Python mirror being correct | Consider Unity test framework for ESP32 when hardware arrives |

---

## Recommended Next Tuning Steps (on hardware)

1. Mount ESP32 on stationary surface, log 60s of raw MPU-6050 accel data at 100Hz
2. Compute variance per axis → set `R` to measured variance
3. Introduce known vibration source, observe filter lag → tune `Q` up if too slow
4. Test spike rejection with EMI source (motor power-on nearby) → tune `spike_threshold`
5. Validate 80ms anomaly detection latency against real anomaly profile
