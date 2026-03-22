#pragma once

#include <Arduino.h>

// =============================================================================
// KalmanFilter — 1D scalar discrete Kalman filter
//
// Intended use
// ------------
//   One instance per IMU axis. The sensor pipeline creates six:
//     KalmanFilter kf_ax, kf_ay, kf_az, kf_gx, kf_gy, kf_gz;
//   Each is updated once per 100 Hz sample tick inside filterTask.
//
// Tuning parameters
// -----------------
//   Q — process noise covariance.
//       Models how much the true signal is expected to change between samples.
//       Higher Q → filter trusts new measurements more → faster tracking, more
//       noise pass-through.
//       Lower  Q → filter trusts the model more → smoother, more lag.
//
//   R — measurement noise covariance.
//       Models how noisy the sensor is. Set this to the observed variance of
//       the sensor output when the physical quantity is stationary.
//       Higher R → filter trusts measurements less → smoother, slower.
//       Lower  R → filter trusts measurements more → faster, noisier.
//
//   Starting defaults (Q=0.01, R=0.1) work well for MPU-6050 vibration data
//   at 100 Hz. Tune against real hardware once available.
//
// Spike / failure handling
// ------------------------
//   spike_threshold > 0 activates spike rejection:
//     If |measurement - estimate| > spike_threshold, the sample is suspicious.
//     - The prediction step still runs (error covariance P grows), increasing
//       uncertainty so the filter remains ready to track a real change.
//     - The update step is skipped and the current estimate is returned.
//     - If MAX_CONSECUTIVE_REJECTIONS suspicious samples arrive in a row, the
//       filter concludes this is a genuine step change (not a glitch) and
//       re-initialises to the new level.
//
//   NaN / Inf inputs are silently dropped — the current estimate is returned
//   and state is not modified.
//
// Thread safety
// -------------
//   NOT thread-safe. Each instance is owned by a single task (filterTask).
//   Do not share instances across tasks.
// =============================================================================

class KalmanFilter {
public:
    // Default tuning — override via setQ() / setR() or the constructor.
    static constexpr float kDefaultQ              = 0.01f;
    static constexpr float kDefaultR              = 0.10f;
    static constexpr float kDefaultInitialError   = 1.00f;  // P₀
    static constexpr int   kMaxConsecutiveRejects = 8;      // detect genuine step change

    // -------------------------------------------------------------------------
    // Construction & reset
    // -------------------------------------------------------------------------

    // Construct with tuning parameters.
    // spike_threshold = 0.0f disables spike rejection entirely.
    explicit KalmanFilter(float Q               = kDefaultQ,
                          float R               = kDefaultR,
                          float spike_threshold = 0.0f);

    // Reset estimate and error covariance to initial conditions.
    // Call this if the sensor is known to have been disconnected or re-ranged.
    // initial_estimate: best guess at the true value before any measurement.
    // initial_error:    initial P (uncertainty). Larger = trust first real
    //                   measurement faster.
    void reset(float initial_estimate = 0.0f,
               float initial_error   = kDefaultInitialError);

    // -------------------------------------------------------------------------
    // Core update — call once per sample
    // -------------------------------------------------------------------------

    // Feed one raw measurement. Returns the filtered estimate.
    // Handles NaN, Inf, and (if configured) spike rejection.
    float update(float measurement);

    // -------------------------------------------------------------------------
    // State inspection
    // -------------------------------------------------------------------------

    float estimate()        const { return _x; }   // current filtered value
    float errorCovariance() const { return _p; }   // current P (uncertainty)
    int   rejectCount()     const { return _consecutive_rejections; }
    bool  isInitialised()   const { return _initialised; }

    // -------------------------------------------------------------------------
    // Runtime tuning — safe to call between updates
    // -------------------------------------------------------------------------

    void setQ(float Q)                       { _Q = Q; }
    void setR(float R)                       { _R = R; }
    void setSpikeThreshold(float threshold)  { _spike_threshold = threshold; }

    float getQ()              const { return _Q; }
    float getR()              const { return _R; }
    float getSpikeThreshold() const { return _spike_threshold; }

private:
    // Tuning
    float _Q;
    float _R;
    float _spike_threshold;   // 0 = disabled

    // State
    float _x;                 // current estimate
    float _p;                 // estimate error covariance
    bool  _initialised;       // false until first valid measurement arrives

    // Spike tracking
    int   _consecutive_rejections;
};
