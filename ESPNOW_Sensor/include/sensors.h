#ifndef SENSORS_H
#define SENSORS_H

struct SensorReadings {
    bool validBME;
    float temperature;
    float humidity;
    float pressure;

    bool validBH1750;
    float lux;

    float batteryVoltage;
    bool validBinary;
    bool binaryState; // e.g. Door
    bool validAnalog;
    float analogValue; // e.g. Soil
};

void initSensors();
SensorReadings readSensors();

#endif
