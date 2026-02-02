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

// Feature flags based on DEVICE_TYPE
// (DEVICE_TYPE is defined in platformio.ini for each env)

#ifndef DEVICE_TYPE
#error "DEVICE_TYPE must be defined in build flags"
#endif

// Define features based on device type
#if DEVICE_TYPE == 1 // DEV_PLANT
    #define USE_BME280
    #define USE_BH1750
    #define USE_SOIL_SENSOR
#elif DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
    #define USE_BME280
    #define USE_BH1750
    #define USE_LD2410
#elif DEVICE_TYPE == 3 // DEV_BINARY
    #define USE_BINARY_SENSOR
#endif

void initSensors(); // No params needing configuration now, but let's keep it simple

// Default includeEnviro to true for backward compatibility
SensorReadings readSensors(bool includeEnviro = true);

#endif
