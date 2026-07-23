#pragma once

#include "EhZMeasurement.h"

// Fixed-size ring buffer of the last N parsed measurements, for the /values
// status page. Stores every valid measurement the parser produces,
// independent of the MQTT dead-band (so the page always shows the freshest
// readings even when publishing is throttled).
template <int N>
class MeasurementHistory {
public:
    struct Entry {
        unsigned long uptimeMs;
        double consumedEnergy;  // Wh
        double producedEnergy;  // Wh
        double currentPower;    // W
    };

    void push(const EhZMeasurement& m, unsigned long uptimeMs) {
        _entries[_head] = { uptimeMs, m.consumedEnergy, m.producedEnergy, m.currentPower };
        _head = (_head + 1) % N;
        if (_count < N) _count++;
    }

    int count() const { return _count; }

    // idxFromNewest == 0 is the most recently pushed entry.
    const Entry& get(int idxFromNewest) const {
        int pos = (_head - 1 - idxFromNewest + 2 * N) % N;
        return _entries[pos];
    }

private:
    Entry _entries[N] = {};
    int _head = 0;
    int _count = 0;
};
