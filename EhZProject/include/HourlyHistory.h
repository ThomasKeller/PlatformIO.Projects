#pragma once

// Fixed-size ring buffer of completed rolling-hourly aggregation windows
// (see updateHourlyAggregation() in main.cpp), for the /values status page.
// Kept independent of whether the window's MQTT publish actually succeeded,
// so the page still shows local history even without a broker connection.
template <int N>
class HourlyHistory {
public:
    struct Entry {
        unsigned long windowEndUptimeMs;
        double consumedDeltaKwh;
        double producedDeltaKwh;
        double avgPowerW;
        double minPowerW;
        double maxPowerW;
    };

    void push(const Entry& e) {
        _entries[_head] = e;
        _head = (_head + 1) % N;
        if (_count < N) _count++;
    }

    int count() const { return _count; }

    // idxFromNewest == 0 is the most recently completed window.
    const Entry& get(int idxFromNewest) const {
        int pos = (_head - 1 - idxFromNewest + 2 * N) % N;
        return _entries[pos];
    }

private:
    Entry _entries[N] = {};
    int _head = 0;
    int _count = 0;
};
