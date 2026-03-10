#pragma once

// Dead-band / rate limiter ported from DeadBand.cs
// - Suppresses publishes closer than timeDeadBandMs apart.
// - Forces a publish when valuesEqualDeadBandMs has elapsed even if the
//   value has not changed.
class DeadBand {
public:
    unsigned long timeDeadBandMs        = 15000UL;   // 15 s
    unsigned long valuesEqualDeadBandMs = 600000UL;  // 10 min

    DeadBand() : _lastPublishMs(0), _lastForceMs(0), _lastValue(0.0),
                 _hasValue(false) {}

    // Call with the current millis() and the new sensor value.
    // Returns true when the value should be published; outDiff is set to
    // (newValue - lastPublishedValue).
    bool addValue(unsigned long nowMs, double value, double& outDiff) {
        outDiff = value - _lastValue;

        // Enforce minimum time between any two publishes.
        if (_hasValue && (nowMs - _lastPublishMs) < timeDeadBandMs) {
            return false;
        }

        bool valueChanged = !_hasValue || (value != _lastValue);
        bool forceByTime  = _hasValue &&
                            ((nowMs - _lastForceMs) >= valuesEqualDeadBandMs);

        if (!valueChanged && !forceByTime) {
            return false;
        }

        _lastValue     = value;
        _lastPublishMs = nowMs;
        if (!_hasValue || forceByTime) {
            _lastForceMs = nowMs;
        }
        _hasValue = true;
        return true;
    }

private:
    unsigned long _lastPublishMs;
    unsigned long _lastForceMs;
    double        _lastValue;
    bool          _hasValue;
};
