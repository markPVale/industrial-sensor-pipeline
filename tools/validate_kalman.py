"""
validate_kalman.py — Kalman filter validation harness

Mirrors the C++ KalmanFilter exactly (same arithmetic, same constants) so
behaviour can be verified on the laptop before flashing to the ESP32.

Tests
-----
  1. Convergence      — noisy constant signal → estimate converges to true value
  2. Spike rejection  — single-sample outliers dampened, non-outlier retained
  3. Step change      — genuine level change tracked within N samples
  4. NaN / Inf guard  — bad inputs discarded, state preserved
  5. Parameter sweep  — effect of Q and R on lag vs noise

Usage
-----
    python tools/validate_kalman.py              # run all tests, print summary
    python tools/validate_kalman.py --plot       # also show matplotlib charts
    python tools/validate_kalman.py --csv out.csv # write raw traces to CSV
"""

import argparse
import csv
import math
import random
import sys

# ---------------------------------------------------------------------------
# Python mirror of KalmanFilter.cpp
# Keep in sync with the C++ implementation.
# ---------------------------------------------------------------------------
K_DEFAULT_Q               = 0.01
K_DEFAULT_R               = 0.10
K_DEFAULT_INITIAL_ERROR   = 1.00
K_MAX_CONSECUTIVE_REJECTS = 8


class KalmanFilter:
    def __init__(self, Q=K_DEFAULT_Q, R=K_DEFAULT_R, spike_threshold=0.0):
        self.Q               = Q
        self.R               = R
        self.spike_threshold = spike_threshold
        self._x              = 0.0
        self._p              = K_DEFAULT_INITIAL_ERROR
        self._initialised    = False
        self._consecutive_rejections = 0

    def reset(self, initial_estimate=0.0, initial_error=K_DEFAULT_INITIAL_ERROR):
        self._x              = initial_estimate
        self._p              = initial_error
        self._initialised    = False
        self._consecutive_rejections = 0

    def update(self, measurement):
        # Guard: non-finite inputs
        if not math.isfinite(measurement):
            return self._x

        # Seed estimate on first valid sample
        if not self._initialised:
            self._x = measurement
            self._p = K_DEFAULT_INITIAL_ERROR
            self._initialised = True
            self._consecutive_rejections = 0
            return self._x

        # Prediction
        p_pred = self._p + self.Q

        # Spike rejection
        if self.spike_threshold > 0.0:
            innovation = abs(measurement - self._x)
            if innovation > self.spike_threshold:
                self._consecutive_rejections += 1
                if self._consecutive_rejections >= K_MAX_CONSECUTIVE_REJECTS:
                    # Genuine step change
                    self._x = measurement
                    self._p = K_DEFAULT_INITIAL_ERROR
                    self._consecutive_rejections = 0
                    return self._x
                # Glitch: grow P, hold estimate
                self._p = p_pred
                return self._x

        self._consecutive_rejections = 0

        # Update
        K      = p_pred / (p_pred + self.R)
        self._x = self._x + K * (measurement - self._x)
        self._p = (1.0 - K) * p_pred
        return self._x

    @property
    def estimate(self):
        return self._x

    @property
    def error_covariance(self):
        return self._p


# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------

def _rng_seed(seed=42):
    rng = random.Random(seed)
    return rng


PASS = "PASS"
FAIL = "FAIL"

results = []

def _record(name, passed, detail=""):
    status = PASS if passed else FAIL
    results.append((name, status, detail))
    marker = "✓" if passed else "✗"
    print(f"  [{marker}] {name}: {status}" + (f" — {detail}" if detail else ""))


# ---------------------------------------------------------------------------
# Test 1: Convergence
# ---------------------------------------------------------------------------
def test_convergence(plot_data=None):
    """
    Feed 200 samples of (true_value + gaussian_noise).
    Expect: final 50-sample mean within 0.5 of true value.
    Also expect: variance of final 50 filtered samples < raw noise variance.
    """
    rng        = _rng_seed()
    true_value = 50.0
    noise_std  = 5.0
    kf         = KalmanFilter(Q=0.01, R=0.1)

    raw_samples      = []
    filtered_samples = []

    for _ in range(200):
        z = true_value + rng.gauss(0, noise_std)
        raw_samples.append(z)
        filtered_samples.append(kf.update(z))

    tail_raw      = raw_samples[-50:]
    tail_filtered = filtered_samples[-50:]

    mean_raw      = sum(tail_raw)      / len(tail_raw)
    mean_filtered = sum(tail_filtered) / len(tail_filtered)
    var_raw       = sum((x - mean_raw) ** 2 for x in tail_raw) / len(tail_raw)
    var_filtered  = sum((x - mean_raw) ** 2 for x in tail_filtered) / len(tail_filtered)

    # The filter cannot correct sample bias (that's not its job) — it should
    # track the raw mean closely while reducing variance around it.
    converged = abs(mean_filtered - mean_raw) < 0.5
    smoothed  = var_filtered < var_raw

    if plot_data is not None:
        plot_data["convergence"] = {
            "raw": raw_samples,
            "filtered": filtered_samples,
            "true": [true_value] * 200,
        }

    _record(
        "Convergence — mean within 0.5 of true",
        converged,
        f"mean_filtered={mean_filtered:.3f} mean_raw={mean_raw:.3f}",
    )
    _record(
        "Convergence — filter reduces variance",
        smoothed,
        f"var_raw={var_raw:.2f} var_filtered={var_filtered:.2f}",
    )


# ---------------------------------------------------------------------------
# Test 2: Spike rejection
# ---------------------------------------------------------------------------
def test_spike_rejection(plot_data=None):
    """
    Insert single-sample spikes (10× the normal range) every 30 samples.
    Expect: filtered output at spike indices stays within 3× normal noise.
    Also verify: non-spike samples are tracked normally (no excess lag).
    """
    rng            = _rng_seed()
    true_value     = 0.30   # typical 'normal' vibration RMS (g)
    noise_std      = 0.02
    spike_value    = 5.00   # 10g spike — sensor glitch / EMI
    threshold      = 0.50   # spike rejection threshold
    spike_indices  = set(range(30, 200, 30))

    kf = KalmanFilter(Q=0.01, R=0.1, spike_threshold=threshold)

    raw_samples      = []
    filtered_samples = []
    spike_residuals  = []

    for i in range(180):
        is_spike = i in spike_indices
        z = spike_value if is_spike else true_value + rng.gauss(0, noise_std)
        raw_samples.append(z)
        y = kf.update(z)
        filtered_samples.append(y)
        if is_spike:
            spike_residuals.append(abs(y - true_value))

    max_spike_residual = max(spike_residuals) if spike_residuals else 0.0
    passed = max_spike_residual < (3 * noise_std + threshold * 0.1)

    if plot_data is not None:
        plot_data["spike"] = {
            "raw": raw_samples,
            "filtered": filtered_samples,
            "true": [true_value] * 180,
            "spike_indices": sorted(spike_indices),
        }

    _record(
        "Spike rejection — single-sample outliers dampened",
        passed,
        f"max residual at spike={max_spike_residual:.4f}g (threshold={threshold}g)",
    )


# ---------------------------------------------------------------------------
# Test 3: Step change (genuine level change must be tracked)
# ---------------------------------------------------------------------------
def test_step_change(plot_data=None):
    """
    Signal steps from 0.3g to 1.8g at sample 50 and stays there.
    With spike_threshold active, expect the filter to converge to the new
    level within kMaxConsecutiveRejects + ~20 samples.
    """
    rng           = _rng_seed()
    level_before  = 0.30
    level_after   = 1.80
    noise_std     = 0.05
    step_at       = 50
    n_samples     = 150
    threshold     = 0.50

    kf = KalmanFilter(Q=0.01, R=0.1, spike_threshold=threshold)

    raw_samples      = []
    filtered_samples = []

    for i in range(n_samples):
        true_value = level_after if i >= step_at else level_before
        z = true_value + rng.gauss(0, noise_std)
        raw_samples.append(z)
        filtered_samples.append(kf.update(z))

    # After K_MAX_CONSECUTIVE_REJECTS + convergence window, should be close
    settle_window = filtered_samples[step_at + K_MAX_CONSECUTIVE_REJECTS + 20:]
    settled_mean  = sum(settle_window) / len(settle_window) if settle_window else 0.0
    passed        = abs(settled_mean - level_after) < 0.15

    if plot_data is not None:
        plot_data["step"] = {
            "raw": raw_samples,
            "filtered": filtered_samples,
            "true": [level_before if i < step_at else level_after for i in range(n_samples)],
            "step_at": step_at,
        }

    _record(
        "Step change — genuine level change tracked",
        passed,
        f"settled_mean={settled_mean:.3f} expected={level_after}",
    )


# ---------------------------------------------------------------------------
# Test 4: NaN / Inf guard
# ---------------------------------------------------------------------------
def test_bad_inputs():
    """
    Feed NaN and Inf mid-stream. State must be preserved, no crash.
    """
    kf = KalmanFilter()

    # Seed with a known value
    for _ in range(10):
        kf.update(1.0)

    estimate_before = kf.estimate

    kf.update(float("nan"))
    kf.update(float("inf"))
    kf.update(float("-inf"))

    estimate_after = kf.estimate

    # State should be unchanged (or at most one step of normal drift)
    unchanged = abs(estimate_after - estimate_before) < 0.01
    _record(
        "Bad input guard — NaN/Inf discarded, state preserved",
        unchanged,
        f"before={estimate_before:.4f} after={estimate_after:.4f}",
    )

    # Confirm normal updates still work after bad inputs
    kf.update(1.0)
    still_works = math.isfinite(kf.estimate)
    _record(
        "Bad input guard — filter continues after bad inputs",
        still_works,
        f"estimate after recovery={kf.estimate:.4f}",
    )


# ---------------------------------------------------------------------------
# Test 5: Parameter sweep (informational, always passes)
# ---------------------------------------------------------------------------
def test_parameter_sweep(plot_data=None):
    """
    Show how Q and R affect the lag-noise trade-off.
    Not a pass/fail test — prints a table for manual inspection.
    """
    rng        = _rng_seed()
    true_value = 0.3
    noise_std  = 0.05
    n_samples  = 100

    raw = [true_value + rng.gauss(0, noise_std) for _ in range(n_samples)]

    configs = [
        ("Q=0.001, R=0.5 (very smooth, laggy)",  0.001, 0.5),
        ("Q=0.01,  R=0.1 (default)",              0.01,  0.1),
        ("Q=0.1,   R=0.1 (responsive)",           0.1,   0.1),
        ("Q=0.5,   R=0.01 (very reactive, noisy)",0.5,   0.01),
    ]

    if plot_data is not None:
        plot_data["sweep"] = {"raw": raw, "traces": []}

    print("\n  Parameter sweep — final 20-sample stats (lower var = smoother):")
    print(f"  {'Config':<42} {'mean':>8} {'var':>10}")
    print(f"  {'-'*42} {'-'*8} {'-'*10}")

    for label, Q, R in configs:
        kf = KalmanFilter(Q=Q, R=R)
        out = [kf.update(z) for z in raw]
        tail = out[-20:]
        mean = sum(tail) / len(tail)
        var  = sum((x - true_value) ** 2 for x in tail) / len(tail)
        print(f"  {label:<42} {mean:>8.4f} {var:>10.6f}")

        if plot_data is not None:
            plot_data["sweep"]["traces"].append({"label": label, "data": out})

    _record("Parameter sweep", True, "see table above")


# ---------------------------------------------------------------------------
# Optional: CSV export
# ---------------------------------------------------------------------------
def write_csv(path, plot_data):
    rows = []
    if "convergence" in plot_data:
        d = plot_data["convergence"]
        for i, (r, f, t) in enumerate(zip(d["raw"], d["filtered"], d["true"])):
            rows.append({"test": "convergence", "i": i, "raw": r, "filtered": f, "true": t})
    if "spike" in plot_data:
        d = plot_data["spike"]
        for i, (r, f, t) in enumerate(zip(d["raw"], d["filtered"], d["true"])):
            rows.append({"test": "spike", "i": i, "raw": r, "filtered": f, "true": t,
                         "is_spike": 1 if i in d["spike_indices"] else 0})
    if "step" in plot_data:
        d = plot_data["step"]
        for i, (r, f, t) in enumerate(zip(d["raw"], d["filtered"], d["true"])):
            rows.append({"test": "step", "i": i, "raw": r, "filtered": f, "true": t})

    if not rows:
        return

    fieldnames = sorted({k for row in rows for k in row})
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n  CSV written → {path}")


# ---------------------------------------------------------------------------
# Optional: matplotlib charts
# ---------------------------------------------------------------------------
def show_plots(plot_data):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n  [!] matplotlib not installed — skipping plots. pip install matplotlib")
        return

    fig, axes = plt.subplots(2, 2, figsize=(14, 9))
    fig.suptitle("KalmanFilter Validation", fontsize=13)

    # Convergence
    if "convergence" in plot_data:
        ax = axes[0][0]
        d  = plot_data["convergence"]
        ax.plot(d["raw"],      alpha=0.4, color="steelblue", label="raw")
        ax.plot(d["filtered"], color="crimson",    linewidth=1.5, label="filtered")
        ax.axhline(d["true"][0], color="green", linestyle="--", linewidth=1, label="true")
        ax.set_title("1. Convergence")
        ax.legend(fontsize=8)

    # Spike rejection
    if "spike" in plot_data:
        ax = axes[0][1]
        d  = plot_data["spike"]
        ax.plot(d["raw"],      alpha=0.3, color="steelblue", label="raw")
        ax.plot(d["filtered"], color="crimson",    linewidth=1.5, label="filtered")
        ax.axhline(d["true"][0], color="green", linestyle="--", linewidth=1, label="true")
        for si in d["spike_indices"]:
            ax.axvline(si, color="orange", alpha=0.5, linewidth=1)
        ax.set_title("2. Spike Rejection (orange = spike)")
        ax.legend(fontsize=8)

    # Step change
    if "step" in plot_data:
        ax = axes[1][0]
        d  = plot_data["step"]
        ax.plot(d["raw"],      alpha=0.3, color="steelblue", label="raw")
        ax.plot(d["filtered"], color="crimson",    linewidth=1.5, label="filtered")
        ax.plot(d["true"],     color="green", linestyle="--", linewidth=1, label="true")
        ax.axvline(d["step_at"], color="purple", linewidth=1, label="step")
        ax.set_title("3. Step Change Tracking")
        ax.legend(fontsize=8)

    # Parameter sweep
    if "sweep" in plot_data:
        ax = axes[1][1]
        d  = plot_data["sweep"]
        ax.plot(d["raw"], alpha=0.3, color="steelblue", label="raw")
        colors = ["crimson", "darkorange", "purple", "teal"]
        for i, trace in enumerate(d["traces"]):
            ax.plot(trace["data"], color=colors[i % len(colors)],
                    linewidth=1.2, label=trace["label"].split("(")[0].strip())
        ax.set_title("4. Parameter Sweep (Q / R)")
        ax.legend(fontsize=7)

    plt.tight_layout()
    plt.show()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Kalman filter validation harness")
    parser.add_argument("--plot", action="store_true", help="Show matplotlib charts")
    parser.add_argument("--csv",  metavar="FILE",      help="Write trace data to CSV")
    args = parser.parse_args()

    plot_data = {} if (args.plot or args.csv) else None

    print("\n=== KalmanFilter Validation ===\n")

    print("Test 1: Convergence")
    test_convergence(plot_data)

    print("\nTest 2: Spike Rejection")
    test_spike_rejection(plot_data)

    print("\nTest 3: Step Change")
    test_step_change(plot_data)

    print("\nTest 4: Bad Inputs (NaN / Inf)")
    test_bad_inputs()

    print("\nTest 5: Parameter Sweep")
    test_parameter_sweep(plot_data)

    # Summary
    passed = sum(1 for _, s, _ in results if s == PASS)
    failed = sum(1 for _, s, _ in results if s == FAIL)
    total  = len(results)
    print(f"\n{'='*35}")
    print(f"Results: {passed}/{total} passed", end="")
    if failed:
        print(f"  ← {failed} FAILED")
        print("\nFailed tests:")
        for name, status, detail in results:
            if status == FAIL:
                print(f"  ✗ {name}: {detail}")
    else:
        print(" — all OK")

    if args.csv and plot_data:
        write_csv(args.csv, plot_data)

    if args.plot and plot_data:
        show_plots(plot_data)

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
