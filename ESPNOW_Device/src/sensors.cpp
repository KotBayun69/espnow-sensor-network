#include "sensors.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

// BME280 and BH1750 use default I2C pins (SDA=D2, SCL=D1 on ESP8266 or similar on ESP32)
// For ESP32 C3 Mini, verify pins if needed.

#if defined(USE_BME280)
Adafruit_BME280 bme;
#endif

#if defined(USE_BH1750)
BH1750 lightMeter;
#endif

// Pin Definitions
// USE_LD2410 removed

#if defined(USE_ADC_SENSOR)
    const int ADC_PIN = 2; // Verify pin
#endif

#if defined(USE_BINARY_SENSOR)
    const int DOOR_PIN = 1; 
#endif

#if IS_BATTERY_POWERED
    const int BATTERY_PIN = 0; // A0 on C3? Verify.
#endif


void initSensors() {
    Wire.begin();

    #if defined(USE_BME280)
        // Initialize BME280
        if (!bme.begin(0x76)) {
            if (!bme.begin(0x77)) {
                Serial.println("Could not find a valid BME280 sensor!");
            }
        }
    #endif

    #if defined(USE_BH1750)
        // Initialize BH1750
        if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
            Serial.println("Error initializing BH1750");
        }
    #endif

    #if defined(USE_ADC_SENSOR)
        pinMode(ADC_PIN, INPUT);
    #endif

    #if defined(USE_BINARY_SENSOR)
        pinMode(DOOR_PIN, INPUT_PULLUP);
    #endif


    #if IS_BATTERY_POWERED
        pinMode(BATTERY_PIN, INPUT);
    #endif
}

SensorReadings readSensors(bool includeEnviro) {
    SensorReadings readings;
    memset(&readings, 0, sizeof(readings)); // Zero out
    readings.flags = 0;
    
    #ifdef USE_BME280
        readings.flags |= SENSOR_FLAG_BME;
    #endif
    #ifdef USE_BH1750
        readings.flags |= SENSOR_FLAG_LUX;
    #endif
    #ifdef USE_ADC_SENSOR
        readings.flags |= SENSOR_FLAG_ADC;
    #endif
    #ifdef USE_BINARY_SENSOR
        readings.flags |= SENSOR_FLAG_BINARY;
    #endif
    
    #if IS_BATTERY_POWERED
        int battRaw = analogRead(BATTERY_PIN);
        // Example divider: (battRaw / 4095.0) * 3.3 * 2
        readings.batteryVoltage = (battRaw / 4095.0) * 3.3 * 2; 
    #else
        readings.batteryVoltage = 0.0;
    #endif

    #if defined(USE_BME280)
        if (includeEnviro) {
            readings.bme.temperature = bme.readTemperature();
            readings.bme.humidity = bme.readHumidity();
            readings.bme.pressure = bme.readPressure() / 100.0F;
        }
    #endif

    #if defined(USE_BH1750)
        if (includeEnviro) {
             readings.lux.lux = lightMeter.readLightLevel();
        }
    #endif

    #if defined(USE_ADC_SENSOR)
        if (includeEnviro) {
             readings.adc.adcValue = analogRead(ADC_PIN) / 4095.0 * 100.0;
        }
    #endif
        

    #if defined(USE_BINARY_SENSOR)
        readings.binary.state = digitalRead(DOOR_PIN);
    #endif

    return readings;
}

