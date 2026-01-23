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

void initSensors() {
    Wire.begin();

    // Initialize BME280
    if (!bme.begin(0x76)) {
         // Try 0x77 if 0x76 fails
         if (!bme.begin(0x77)) {
             Serial.println("Could not find a valid BME280 sensor, check wiring!");
         }
    }

    // Initialize BH1750
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
        Serial.println("Error initializing BH1750");
    }

    // Initialize other pins
    pinMode(DOOR_PIN, INPUT_PULLUP);
    pinMode(BATTERY_PIN, INPUT);
    pinMode(SOIL_PIN, INPUT);
}

SensorReadings readSensors() {
    SensorReadings readings = {false, 0, 0, 0, false, 0, 0, false, 0};

    // Read BME280
    readings.temperature = bme.readTemperature();
    readings.humidity = bme.readHumidity();
    readings.pressure = bme.readPressure() / 100.0F; // hPa
    
    if (!isnan(readings.temperature) && !isnan(readings.humidity)) {
        readings.validBME = true;
    }

    // Read BH1750
    float lux = lightMeter.readLightLevel();
    if (lux >= 0) {
        readings.lux = lux;
        readings.validBH1750 = true;
    }

    // Read Battery (Analog)
    // ESP32 ADC: 0-4095. Voltage divider needed usually.
    // Assuming simple mapping for demo
    int battRaw = analogRead(BATTERY_PIN);
    readings.batteryVoltage = (battRaw / 4095.0) * 3.3 * 2; // Example divider

    // Read Door (Binary)
    readings.binaryState = digitalRead(DOOR_PIN);
    readings.validBinary = true;

    // Read Soil (Analog)
    readings.analogValue = analogRead(SOIL_PIN) / 4095.0 * 100; // Percentage?
    readings.validAnalog = true;

    return readings;
}
