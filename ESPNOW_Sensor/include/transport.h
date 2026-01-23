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

// Structures must match on both sides (Sensor and Gateway)
// Use packed attribute to ensure byte alignment

typedef struct __attribute__((packed)) struct_config_message {
    uint8_t type;
    uint8_t macAddr[6];
    char deviceName[32];
    bool hasBME;
    bool hasBH1750;
    bool hasBattery;
    bool hasBinary;
    bool hasAnalog;
} ConfigMessage;

typedef struct __attribute__((packed)) struct_data_message {
    uint8_t type;
    float temperature;
    float humidity;
    float pressure;
    float lux;
    float batteryVoltage;
    bool binaryState;
    float analogValue;
} DataMessage;

typedef struct __attribute__((packed)) struct_ack_message {
    uint8_t type;
} AckMessage;

// Command types for CMD messages
enum CmdType {
    CMD_OTA = 1,
    CMD_RESTART = 2
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

#endif
