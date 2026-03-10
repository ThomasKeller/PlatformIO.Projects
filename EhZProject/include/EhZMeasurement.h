#pragma once

struct EhZMeasurement {
    double consumedEnergy1 = 0.0;  // Wh, OBIS 1.8.0
    double producedEnergy1 = 0.0;  // Wh, OBIS 2.8.0
    double consumedEnergy2 = 0.0;  // Wh, OBIS 1.8.1
    double producedEnergy2 = 0.0;  // Wh, OBIS 2.8.1
    double currentPower    = 0.0;  // W,  OBIS 16.7.0
    bool   valid           = false;
};
