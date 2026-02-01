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

// LD2410 Definitions
#define LD2410_BAUD 256000
const int LD2410_RX_PIN = 20; // Connect to Sensor TX
const int LD2410_TX_PIN = 21; // Connect to Sensor RX
const int LD2410_OUT_PIN = 10;

// Generic definitions
const int BATTERY_PIN = 0; // A0 on C3? Verify.
const int DOOR_PIN = 1; 
const int SOIL_PIN = 2;
// const int MOTION_PIN = 3; // REMOVED (Replaced by LD2410_OUT_PIN)

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
        pinMode(LD2410_OUT_PIN, INPUT);
        // Initialize UART for LD2410
        Serial1.begin(LD2410_BAUD, SERIAL_8N1, LD2410_RX_PIN, LD2410_TX_PIN);
    }

    if (isBatteryPowered) {
        pinMode(BATTERY_PIN, INPUT);
    }
}

SensorReadings readSensors(uint8_t deviceType, bool isBatteryPowered, bool includeEnviro) {
    SensorReadings readings;
    readings.deviceType = deviceType;
    readings.batteryVoltage = 0.0;

    // Read battery only if powered by one
    if (isBatteryPowered) {
        int battRaw = analogRead(BATTERY_PIN);
        readings.batteryVoltage = (battRaw / 4095.0) * 3.3 * 2; // Example divider
    }

    if (deviceType == DEV_PLANT) {
        if (includeEnviro) {
            readings.data.plant.temperature = bme.readTemperature();
            readings.data.plant.humidity = bme.readHumidity();
            readings.data.plant.pressure = bme.readPressure() / 100.0F;
            readings.data.plant.lux = lightMeter.readLightLevel();
            readings.data.plant.soilMoisture = analogRead(SOIL_PIN) / 4095.0 * 100.0;
        }
    } 
    else if (deviceType == DEV_ENVIRO_MOTION) {
        if (includeEnviro) {
            readings.data.enviro.temperature = bme.readTemperature();
            readings.data.enviro.humidity = bme.readHumidity();
            readings.data.enviro.pressure = bme.readPressure() / 100.0F;
            readings.data.enviro.lux = lightMeter.readLightLevel();
        } else {
            // Fill with safe defaults or 0. Caller should handle state.
            readings.data.enviro.temperature = 0;
            readings.data.enviro.humidity = 0;
            readings.data.enviro.pressure = 0;
            readings.data.enviro.lux = 0;
        }
        
        readings.data.enviro.motionDetected = digitalRead(LD2410_OUT_PIN);
        readings.data.enviro.distance = 0.0; 

        // Try to read distance from Serial1
        // We wait up to 150ms for a frame (Sensor sends every ~100ms)
        unsigned long startWait = millis();
        uint8_t buffer[50];
        int bufPos = 0;
        bool frameFound = false;

        while (millis() - startWait < 150 && !frameFound) {
            if (Serial1.available()) {
                uint8_t b = Serial1.read();
                
                // Header F4 F3 F2 F1
                if (bufPos < 4) {
                    if ((bufPos == 0 && b == 0xF4) ||
                        (bufPos == 1 && b == 0xF3) ||
                        (bufPos == 2 && b == 0xF2) ||
                        (bufPos == 3 && b == 0xF1)) {
                        buffer[bufPos++] = b;
                    } else {
                        bufPos = 0;
                        if (b == 0xF4) buffer[bufPos++] = b;
                    }
                } else {
                    buffer[bufPos++] = b;
                    // Check length once we have 6 bytes (Header + Len)
                    if (bufPos >= 6) {
                         int dataLen = buffer[4] + (buffer[5] << 8);
                         int totalFrameLen = 4 + 2 + dataLen + 4;
                         
                         if (bufPos >= totalFrameLen) {
                            if (buffer[totalFrameLen-4] == 0xF8 && 
                                buffer[totalFrameLen-3] == 0xF7 &&
                                buffer[totalFrameLen-2] == 0xF6 &&
                                buffer[totalFrameLen-1] == 0xF5) {
                                
                                // Valid Frame
                                uint16_t movDist = buffer[6 + 3] | (buffer[6 + 4] << 8);
                                uint16_t statDist = buffer[6 + 6] | (buffer[6 + 7] << 8);
                                
                                // Prioritize moving distance, fall back to stationary if moving is 0 but presence is true
                                if (movDist > 0) readings.data.enviro.distance = movDist;
                                else readings.data.enviro.distance = statDist;
                                
                                frameFound = true;
                            }
                            bufPos = 0;
                         }
                    }
                    if (bufPos >= 49) bufPos = 0;
                }
            } else {
                delay(1); // Yield
            }
        }
    }
    else if (deviceType == DEV_BINARY) {
        readings.data.binary.state = digitalRead(DOOR_PIN);
    }

    return readings;
}
