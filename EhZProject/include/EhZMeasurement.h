#pragma once

struct EhZMeasurement {
    double consumedEnergy = 0.0;  // Wh, OBIS 1.8.0
    double producedEnergy = 0.0;  // Wh, OBIS 2.8.0
    double currentPower    = 0.0;  // W,  OBIS 16.7.0 (optional, defaults to 0.0 if absent)
    bool   valid           = false;
};
