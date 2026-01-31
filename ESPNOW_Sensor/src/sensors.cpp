#include "sensors.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

// BME280 and BH1750 use default I2C pins (SDA=D2, SCL=D1 on ESP8266 or similar on ESP32)
// For ESP32 C3 Mini, verify pins if needed.

Adafruit_BME280 bme;
BH1750 lightMeter;

// Mock pins for other sensors (adjust as needed for ESP32 C3)
// const int BATTERY_PIN = A0; // Analog
// const int DOOR_PIN = D2;    // Digital input
// const int SOIL_PIN = A1;    // Analog

// ESP32 ADC is different.
const int BATTERY_PIN = 0; // A0 on C3? Verify.
const int DOOR_PIN = 1; 
const int SOIL_PIN = 2;
const int MOTION_PIN = 3; // Placeholder for LD2410 out or Serial pins

void initSensors(uint8_t deviceType, bool isBatteryPowered) {
    Wire.begin();

    if (deviceType == DEV_PLANT || deviceType == DEV_ENVIRO_MOTION) {
        // Initialize BME280
        if (!bme.begin(0x76)) {
            if (!bme.begin(0x77)) {
                Serial.println("Could not find a valid BME280 sensor!");
            }
        }
        // Initialize BH1750
        if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
            Serial.println("Error initializing BH1750");
        }
    }

    if (deviceType == DEV_PLANT) {
        pinMode(SOIL_PIN, INPUT);
    }

    if (deviceType == DEV_BINARY) {
        pinMode(DOOR_PIN, INPUT_PULLUP);
    }

    if (deviceType == DEV_ENVIRO_MOTION) {
        pinMode(MOTION_PIN, INPUT); // Digital out from LD2410
    }

    if (isBatteryPowered) {
        pinMode(BATTERY_PIN, INPUT);
    }
}

SensorReadings readSensors(uint8_t deviceType, bool isBatteryPowered) {
    SensorReadings readings;
    readings.deviceType = deviceType;
    readings.batteryVoltage = 0.0;

    // Read battery only if powered by one
    if (isBatteryPowered) {
        int battRaw = analogRead(BATTERY_PIN);
        readings.batteryVoltage = (battRaw / 4095.0) * 3.3 * 2; // Example divider
    }

    if (deviceType == DEV_PLANT) {
        readings.data.plant.temperature = bme.readTemperature();
        readings.data.plant.humidity = bme.readHumidity();
        readings.data.plant.pressure = bme.readPressure() / 100.0F;
        readings.data.plant.lux = lightMeter.readLightLevel();
        readings.data.plant.soilMoisture = analogRead(SOIL_PIN) / 4095.0 * 100.0;
    } 
    else if (deviceType == DEV_ENVIRO_MOTION) {
        readings.data.enviro.temperature = bme.readTemperature();
        readings.data.enviro.humidity = bme.readHumidity();
        readings.data.enviro.pressure = bme.readPressure() / 100.0F;
        readings.data.enviro.lux = lightMeter.readLightLevel();
        readings.data.enviro.motionDetected = digitalRead(MOTION_PIN);
        readings.data.enviro.distance = 0.0; // Placeholder for UART reading
    }
    else if (deviceType == DEV_BINARY) {
        readings.data.binary.state = digitalRead(DOOR_PIN);
    }

    return readings;
}
