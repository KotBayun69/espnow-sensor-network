#include "sensors.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

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

#if defined(USE_SOIL_SENSOR)
    const int SOIL_PIN = 0; // GPIO 0 (A0) for ESP32-C3 Super Mini
    const int SOIL_POWER_PIN = 2; // Power pin for sensor
#endif

#if defined(USE_BINARY_SENSOR)
    const int DOOR_PIN = 1; 
#endif

#if IS_BATTERY_POWERED
    const int BATTERY_PIN = 1; // A0 on C3? Verify.
#endif



#if defined(USE_SOIL_SENSOR)
    struct SoilConfig {
        int min = 0;   // Dry/Air
        int max = 4095; // Wet/Water
    } soilConfig;
    
    void loadSoilConfig() {
        if (LittleFS.exists("/soil_config.json")) {
            File f = LittleFS.open("/soil_config.json", "r");
            if (f) {
                StaticJsonDocument<64> doc;
                deserializeJson(doc, f);
                soilConfig.min = doc["min"] | 0;
                soilConfig.max = doc["max"] | 4095;
                f.close();
            }
        }
    }
    
    void saveSoilConfig() {
        File f = LittleFS.open("/soil_config.json", "w");
        if (f) {
            StaticJsonDocument<64> doc;
            doc["min"] = soilConfig.min;
            doc["max"] = soilConfig.max;
            serializeJson(doc, f);
            f.close();
        }
    }

    void calibrateSoil(bool isWet) {
        digitalWrite(SOIL_POWER_PIN, HIGH);
        delay(50);
        analogRead(SOIL_PIN); // Discard
        long sum = 0;
        for(int i=0; i<5; i++) {
            sum += analogRead(SOIL_PIN);
            delay(10);
        }
        digitalWrite(SOIL_POWER_PIN, LOW);
        int avg = sum / 5;
        
        loadSoilConfig(); // Ensure latest
        if (isWet) {
            soilConfig.max = avg;
        } else {
            soilConfig.min = avg;
        }
        saveSoilConfig();
    }
#else
    void calibrateSoil(bool isWet) {}
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

    #if defined(USE_SOIL_SENSOR)
        pinMode(SOIL_POWER_PIN, OUTPUT);
        digitalWrite(SOIL_POWER_PIN, LOW); // Ensure off
        pinMode(SOIL_PIN, INPUT);
    #endif

    #if defined(USE_BINARY_SENSOR)
        pinMode(DOOR_PIN, INPUT_PULLUP);
    #endif


    #if IS_BATTERY_POWERED
        pinMode(BATTERY_PIN, INPUT);
    #endif
}

SensorReadings readSensors() {
    SensorReadings readings;
    memset(&readings, 0, sizeof(readings)); // Zero out
    readings.flags = 0;
    
    #ifdef USE_BME280
        readings.flags |= SENSOR_FLAG_BME;
    #endif
    #ifdef USE_BH1750
        readings.flags |= SENSOR_FLAG_LUX;
    #endif
    #ifdef USE_SOIL_SENSOR
        readings.flags |= SENSOR_FLAG_SOIL;
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
            readings.bme.temperature = bme.readTemperature();
            readings.bme.humidity = bme.readHumidity();
            readings.bme.pressure = bme.readPressure() / 100.0F;
    #endif

    #if defined(USE_BH1750)
             readings.lux.lux = lightMeter.readLightLevel();
    #endif

    #if defined(USE_SOIL_SENSOR)
             // Power up sensor
             digitalWrite(SOIL_POWER_PIN, HIGH);
             delay(50); // Warm up

             // Discard first reading
             analogRead(SOIL_PIN);
             
             // Take 5 readings and average
             long sum = 0;
             for(int i=0; i<5; i++) {
                 sum += analogRead(SOIL_PIN);
                 delay(10);
             }
             
             // Power down
             digitalWrite(SOIL_POWER_PIN, LOW);

             static bool configLoaded = false;
             if (!configLoaded) { loadSoilConfig(); configLoaded = true; }
             
             int raw = sum / 5;
             int pct = map(raw, soilConfig.min, soilConfig.max, 0, 100);
             pct = constrain(pct, 0, 100);
             readings.soil.moisture = (float)pct; 
    #endif
        
    #if defined(USE_BINARY_SENSOR)
        readings.binary.state = digitalRead(DOOR_PIN);
    #endif

    return readings;
}


