#pragma once

// Time-weighted (zero-order-hold) average of an instantaneous value across
// repeated sample() calls: each new sample closes out the previous value's
// hold time, so the result reflects how long each reading was actually in
// effect rather than treating every sample as equally significant
// (telegrams don't arrive at perfectly even intervals).
class TimeWeightedAverage {
public:
    // Records a new instantaneous sample and returns the average of all
    // samples seen since the last reset()/construction.
    double sample(unsigned long nowMs, double value) {
        if (_hasLast) {
            unsigned long dt = nowMs - _lastSampleMs;
            _sumMs   += (double)dt * _lastValue;
            _totalMs += dt;
        }
        _lastValue    = value;
        _lastSampleMs = nowMs;
        _hasLast      = true;
        return _totalMs > 0 ? (_sumMs / _totalMs) : value;
    }

    // Starts a new averaging window, seeded with the given value (typically
    // the latest instantaneous sample) so a subsequent sample() has an
    // immediate previous value to hold over.
    void reset(unsigned long nowMs, double value) {
        _sumMs        = 0.0;
        _totalMs      = 0;
        _lastValue    = value;
        _lastSampleMs = nowMs;
        _hasLast      = true;
    }

private:
    double        _sumMs        = 0.0;
    unsigned long _totalMs      = 0;
    double        _lastValue    = 0.0;
    unsigned long _lastSampleMs = 0;
    bool          _hasLast      = false;
};
