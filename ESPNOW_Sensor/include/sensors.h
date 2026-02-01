#ifndef SENSORS_H
#define SENSORS_H

#include "transport.h"

struct SensorReadings {
    uint8_t deviceType;
    float batteryVoltage;
    union {
        PlantData plant;
        EnviroMotionData enviro;
        BinaryData binary;
    } data;
};

void initSensors(uint8_t deviceType, bool isBatteryPowered);
// Default includeEnviro to true for backward compatibility
SensorReadings readSensors(uint8_t deviceType, bool isBatteryPowered, bool includeEnviro = true);

#endif
