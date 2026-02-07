#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

// Message Types
#define MSG_CONFIG 1
#define MSG_DATA   2
#define MSG_ACK    3
#define MSG_CMD    4

// Sensor Flags (Bitmask)
#define SENSOR_FLAG_BME    (1 << 0) // 1
#define SENSOR_FLAG_LUX    (1 << 1) // 2
#define SENSOR_FLAG_SOIL    (1 << 2) // 4
#define SENSOR_FLAG_BINARY (1 << 3) // 8

// --- Config Sub-structures ---
// All configs currently just use sleepInterval. 
// If specific sensors need config later, we can add them like BMEConfig etc.

// --- Data Sub-structures ---
typedef struct __attribute__((packed)) {
    float temperature;
    float humidity;
    float pressure;
} BMEData;

typedef struct __attribute__((packed)) {
    float lux;
} LuxData;

typedef struct __attribute__((packed)) {
    bool state;
} BinaryData;

typedef struct __attribute__((packed)) {
    float moisture;
} SoilData;

// --- Main Messages ---

typedef struct __attribute__((packed)) struct_config_message {
    uint8_t type;         // MSG_CONFIG
    uint8_t sensorFlags;  // Active sensors bitmask
    uint8_t macAddr[6];
    char deviceName[32];
    uint16_t sleepInterval;
} ConfigMessage;

typedef struct __attribute__((packed)) struct_data_message {
    uint8_t type;         // MSG_DATA
    uint8_t sensorFlags;  // Active sensors bitmask
    float batteryVoltage;
    BMEData bme;
    LuxData lux;
    SoilData soil;
    BinaryData binary;
} DataMessage;

typedef struct __attribute__((packed)) struct_ack_message {
    uint8_t type;         // MSG_ACK
} AckMessage;

// Command types for CMD messages
enum CmdType {
    CMD_OTA = 1,
    CMD_RESTART = 2,
    CMD_UPDATE = 3,
    CMD_FLUSH = 4,
    CMD_CONFIG = 5
    // Future: CMD_SLEEP_DURATION, etc.
};

typedef struct __attribute__((packed)) struct_cmd_message {
    uint8_t type;      // MSG_CMD (always 4)
    uint8_t cmdType;   // Which command (CMD_OTA, CMD_RESTART, etc.)
    bool value;        // Command state (true/false, on/off)
} CmdMessage;

// --- Shared Utilities ---
inline String slugify(String name) {
    name.replace(" ", "_");
    name.toLowerCase();
    return name;
}

#endif
