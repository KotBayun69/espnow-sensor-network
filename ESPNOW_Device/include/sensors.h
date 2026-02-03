#ifndef SENSORS_H
#define SENSORS_H

#include "transport.h"

struct SensorReadings {
    uint8_t flags;
    float batteryVoltage;
    BMEData bme;
    LuxData lux;
    ADCData adc;
    BinaryData binary;
    // Helper unions for mapping to Protocol DataMessage if needed, 
    // OR we just map field-by-field in main.cpp. 
    // User requested "PlantData -> ADCData" which is confusing, but keeping it simple:
    // This struct now holds ALL potential data.
};

// DEVICE_TYPE check removed - feature flags drive logic.

// Define features based on device type
// Features are now defined in platformio.ini

void initSensors(); // No params needing configuration now, but let's keep it simple

// Default includeEnviro to true for backward compatibility
SensorReadings readSensors(bool includeEnviro = true);

#endif
