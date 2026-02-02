#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// Message Types
#define MSG_CONFIG 1
#define MSG_DATA   2
#define MSG_ACK    3
#define MSG_CMD    4

// Structures must match on both sides (Device and Gateway)
// Use packed attribute to ensure byte alignment

// Device Types
enum DeviceType : uint8_t {
    DEV_PLANT = 1,         // BME + Light + Soil
    DEV_ENVIRO_MOTION = 2, // BME + Light + LD2410
    DEV_BINARY = 3         // Door/Binary sensor
};

// --- Config Sub-structures ---
typedef struct __attribute__((packed)) {
    uint16_t sleepInterval;
} PlantConfig;

typedef struct __attribute__((packed)) {
    uint16_t sleepInterval;
    uint16_t motionTimeout; // Specific to LD2410 or similar
} EnviroMotionConfig;

typedef struct __attribute__((packed)) {
    uint16_t sleepInterval; // might be 0 for always-on/interrupt-based
} BinaryConfig;

// --- Data Sub-structures ---
typedef struct __attribute__((packed)) {
    float temperature;
    float humidity;
    float pressure;
    float lux;
    float soilMoisture;
} PlantData;

typedef struct __attribute__((packed)) {
    float temperature;
    float humidity;
    float pressure;
    float lux;
    bool motionDetected; // effectively "Occupancy"
} EnviroMotionData;

typedef struct __attribute__((packed)) {
    bool state;
} BinaryData;

// --- Main Messages ---

typedef struct __attribute__((packed)) struct_config_message {
    uint8_t type;         // MSG_CONFIG
    uint8_t deviceType;   // DeviceType enum
    uint8_t macAddr[6];
    char deviceName[32];
    bool isBatteryPowered; // Flag to indicate if device should sleep
    union {
        PlantConfig plant;
        EnviroMotionConfig enviro;
        BinaryConfig binary;
    } config;
} ConfigMessage;

typedef struct __attribute__((packed)) struct_data_message {
    uint8_t type;         // MSG_DATA
    uint8_t deviceType;   // DeviceType enum
    float batteryVoltage;
    union {
        PlantData plant;
        EnviroMotionData enviro;
        BinaryData binary;
    } data;
} DataMessage;

typedef struct __attribute__((packed)) struct_ack_message {
    uint8_t type;         // MSG_ACK
} AckMessage;

// Command types for CMD messages
enum CmdType {
    CMD_OTA = 1,
    CMD_RESTART = 2,
    CMD_UPDATE = 3
    // Future: CMD_CONFIG, CMD_SLEEP_DURATION, etc.
};

typedef struct __attribute__((packed)) struct_cmd_message {
    uint8_t type;      // MSG_CMD (always 4)
    uint8_t cmdType;   // Which command (CMD_OTA, CMD_RESTART, etc.)
    bool value;        // Command state (true/false, on/off)
} CmdMessage;

void initTransport();
bool sendConfigMessage(ConfigMessage msg);
bool sendDataMessage(DataMessage msg);
bool isOtaRequested();  // Check if OTA mode was requested via CMD
void clearOtaRequest(); // Clear the flag
bool isUpdateRequested(); 
void clearUpdateRequest();
bool hasAckBeenReceived(); // Check if MSG_ACK was received
void clearAckFlag();       // Reset the ACK flag

#endif
