#ifndef COMMON_UTILS_H
#define COMMON_UTILS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

static bool _common_shouldSave = false;
inline void _common_saveCallback() { _common_shouldSave = true; }

/**
 * Common logging function that sends to Serial and Telnet if connected.
 */
inline void logToBoth(const String& msg, bool newline, WiFiClient& telnetClient) {
    Serial.print(msg);
    if (newline) Serial.println();
    
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(msg);
        if (newline) telnetClient.println();
    }
}

/**
 * Standardized MQTT configuration structure.
 */
struct MqttConfig {
    char server[40];
    char user[40];
    char pass[40];
    uint16_t port;
};

/**
 * Loads MQTT configuration from LittleFS.
 */
inline bool loadBaseConfig(MqttConfig& config, const char* filename = "/config.json") {
    if (LittleFS.exists(filename)) {
        File f = LittleFS.open(filename, "r");
        if (f) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, f);
            if (!error) {
                strlcpy(config.server, doc["server"] | "192.168.1.101", sizeof(config.server));
                strlcpy(config.user, doc["user"] | "", sizeof(config.user));
                strlcpy(config.pass, doc["pass"] | "", sizeof(config.pass));
                config.port = doc["port"] | 1883;
                f.close();
                return true;
            }
            f.close();
        }
    }
    return false;
}

/**
 * Saves MQTT configuration to LittleFS.
 */
inline bool saveBaseConfig(const MqttConfig& config, const char* filename = "/config.json") {
    File f = LittleFS.open(filename, "w");
    if (f) {
        StaticJsonDocument<512> doc;
        doc["server"] = config.server;
        doc["user"] = config.user;
        doc["pass"] = config.pass;
        doc["port"] = config.port;
        serializeJson(doc, f);
        f.close();
        return true;
    }
    return false;
}

/**
 * Starts the WiFiManager Config Portal for MQTT settings.
 * Includes fallback logic to restart if connection fails.
 */
inline void startMqttConfigPortal(MqttConfig& config, const char* apName) {
    _common_shouldSave = false;
    WiFiManager wm;
    wm.setSaveConfigCallback(_common_saveCallback);
    
    // Setup parameters
    WiFiManagerParameter c_server("server", "mqtt server", config.server, 40);
    char pStr[6]; itoa(config.port, pStr, 10);
    WiFiManagerParameter c_port("port", "mqtt port", pStr, 6);
    WiFiManagerParameter c_user("user", "mqtt user", config.user, 40);
    WiFiManagerParameter c_pass("pass", "mqtt pass", config.pass, 40);
    
    wm.addParameter(&c_server);
    wm.addParameter(&c_port);
    wm.addParameter(&c_user);
    wm.addParameter(&c_pass);

    if (!wm.startConfigPortal(apName)) {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        ESP.restart();
    }

    // Copy back
    strlcpy(config.server, c_server.getValue(), 40);
    config.port = atoi(c_port.getValue());
    strlcpy(config.user, c_user.getValue(), 40);
    strlcpy(config.pass, c_pass.getValue(), 40);
    
    if (_common_shouldSave) {
        saveBaseConfig(config);
    }
}

#endif
