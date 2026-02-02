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
#if defined(USE_LD2410)
    #define LD2410_BAUD 256000
    const int LD2410_RX_PIN = 20; // Connect to Sensor TX
    const int LD2410_TX_PIN = 21; // Connect to Sensor RX
    const int LD2410_OUT_PIN = 10;
#endif

#if defined(USE_SOIL_SENSOR)
    const int SOIL_PIN = 2; // Verify pin
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

    #if defined(USE_SOIL_SENSOR)
        pinMode(SOIL_PIN, INPUT);
    #endif

    #if defined(USE_BINARY_SENSOR)
        pinMode(DOOR_PIN, INPUT_PULLUP);
    #endif

    #if defined(USE_LD2410)
        pinMode(LD2410_OUT_PIN, INPUT);
        // Initialize UART for LD2410
        Serial1.begin(LD2410_BAUD, SERIAL_8N1, LD2410_RX_PIN, LD2410_TX_PIN);
    #endif

    #if IS_BATTERY_POWERED
        pinMode(BATTERY_PIN, INPUT);
    #endif
}

SensorReadings readSensors(bool includeEnviro) {
    SensorReadings readings;
    memset(&readings, 0, sizeof(readings)); // Zero out
    readings.deviceType = DEVICE_TYPE;
    
    #if IS_BATTERY_POWERED
        int battRaw = analogRead(BATTERY_PIN);
        // Example divider: (battRaw / 4095.0) * 3.3 * 2
        readings.batteryVoltage = (battRaw / 4095.0) * 3.3 * 2; 
    #else
        readings.batteryVoltage = 0.0;
    #endif

    #if defined(USE_BME280) && defined(USE_BH1750)
        if (includeEnviro) {
            readings.data.enviro.temperature = bme.readTemperature(); // plant and enviro share structs somewhat or fields
            // NOTE: PlantData and EnviroMotionData have same first 4 fields (temp, hum, pres, lux)
            // We need to match the struct type based on DEVICE_TYPE
            
            #if DEVICE_TYPE == 1 // DEV_PLANT
                readings.data.plant.temperature = bme.readTemperature();
                readings.data.plant.humidity = bme.readHumidity();
                readings.data.plant.pressure = bme.readPressure() / 100.0F;
                readings.data.plant.lux = lightMeter.readLightLevel();
            #elif DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
                readings.data.enviro.temperature = bme.readTemperature();
                readings.data.enviro.humidity = bme.readHumidity();
                readings.data.enviro.pressure = bme.readPressure() / 100.0F;
                readings.data.enviro.lux = lightMeter.readLightLevel();
            #endif
        }
    #endif

    #if defined(USE_SOIL_SENSOR)
        if (includeEnviro) {
             readings.data.plant.soilMoisture = analogRead(SOIL_PIN) / 4095.0 * 100.0;
        }
    #endif
        
    #if defined(USE_LD2410)
        // Just read the binary output pin from the LD2410
        // The sensor internal logic handles "Occupied" vs "Empty"
        readings.data.enviro.motionDetected = digitalRead(LD2410_OUT_PIN);
    #endif

    #if defined(USE_BINARY_SENSOR)
        readings.data.binary.state = digitalRead(DOOR_PIN);
    #endif

    return readings;
}

