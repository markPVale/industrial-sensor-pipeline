// =============================================================================
// KalmanFilter.cpp
// =============================================================================

#include "KalmanFilter.h"
#include <math.h>

// =============================================================================
// Construction & reset
// =============================================================================

KalmanFilter::KalmanFilter(float Q, float R, float spike_threshold)
    : _Q(Q),
      _R(R),
      _spike_threshold(spike_threshold),
      _x(0.0f),
      _p(kDefaultInitialError),
      _initialised(false),
      _consecutive_rejections(0)
{}

void KalmanFilter::reset(float initial_estimate, float initial_error) {
    _x                      = initial_estimate;
    _p                      = initial_error;
    _initialised            = false;   // force re-init on next valid sample
    _consecutive_rejections = 0;
}

// =============================================================================
// Core update
// =============================================================================

float KalmanFilter::update(float measurement) {

    // -------------------------------------------------------------------------
    // Guard: reject non-finite inputs
    // -------------------------------------------------------------------------
    if (!isfinite(measurement)) {
        // Return the current best estimate unchanged.
        // P is not updated — a bad sample provides no information about
        // process uncertainty, so we don't grow P here.
        return _x;
    }

    // -------------------------------------------------------------------------
    // Initialisation: seed the estimate with the first valid measurement
    // rather than starting at zero, which would cause a large initial error.
    // -------------------------------------------------------------------------
    if (!_initialised) {
        _x           = measurement;
        _p           = kDefaultInitialError;
        _initialised = true;
        _consecutive_rejections = 0;
        return _x;
    }

    // -------------------------------------------------------------------------
    // Prediction step
    // For a constant-value model (no control input, no state transition):
    //   x_pred = x          (estimate doesn't change between steps)
    //   P_pred = P + Q      (uncertainty grows with time)
    // -------------------------------------------------------------------------
    const float p_pred = _p + _Q;

    // -------------------------------------------------------------------------
    // Spike rejection (optional, active when _spike_threshold > 0)
    // -------------------------------------------------------------------------
    if (_spike_threshold > 0.0f) {
        const float innovation = fabsf(measurement - _x);

        if (innovation > _spike_threshold) {
            // This sample looks like a glitch — check if we've seen enough
            // consecutive outliers to conclude it's actually a real step change.
            _consecutive_rejections++;

            if (_consecutive_rejections >= kMaxConsecutiveRejects) {
                // Genuine step change detected.
                // Re-initialise to the new level so the filter tracks it.
                _x                      = measurement;
                _p                      = kDefaultInitialError;
                _consecutive_rejections = 0;
                return _x;
            }

            // Still treating it as a glitch:
            // - Advance P so uncertainty grows (filter stays ready to adapt).
            // - Don't update the estimate.
            _p = p_pred;
            return _x;
        }
    }

    // Measurement is within acceptable range — reset the rejection counter.
    _consecutive_rejections = 0;

    // -------------------------------------------------------------------------
    // Update step
    //   K = P_pred / (P_pred + R)      Kalman gain
    //   x = x_pred + K * (z - x_pred) Corrected estimate
    //   P = (1 - K) * P_pred           Updated error covariance
    // -------------------------------------------------------------------------
    const float K = p_pred / (p_pred + _R);
    _x = _x + K * (measurement - _x);
    _p = (1.0f - K) * p_pred;

    return _x;
}
