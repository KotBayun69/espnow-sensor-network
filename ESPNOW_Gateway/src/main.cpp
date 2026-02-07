#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <map>
#include <vector>
#include <vector>

#include "CommonUtils.h"
#include "protocol.h"

// Forward declarations or early declarations
bool otaMode = false;
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool isOTAUpdating = false;
bool otaStatusSent = false;

// Global log helper
void log(const String& msg, bool newline = true) {
    logToBoth(msg, newline, telnetClient);
}

// Device tracking maps
std::map<String, String> deviceNames;
std::map<String, std::vector<uint8_t>> deviceMacs;
std::map<String, bool> stayAwakeState;

void loadKnownDevices() {
    if (LittleFS.exists("/known_devices.json")) {
        File f = LittleFS.open("/known_devices.json", "r");
        if (f) {
            StaticJsonDocument<1024> doc;
            DeserializationError error = deserializeJson(doc, f);
            if (!error) {
                JsonObject obj = doc.as<JsonObject>();
                for (JsonPair kv : obj) {
                    deviceNames[kv.key().c_str()] = kv.value().as<String>();
                }
                Serial.println("Loaded known devices from LittleFS");
            }
            f.close();
        }
    }
}

void saveKnownDevices() {
    File f = LittleFS.open("/known_devices.json", "w");
    if (f) {
        StaticJsonDocument<1024> doc;
        for (auto const& [mac, name] : deviceNames) {
            doc[mac] = name;
        }
        serializeJson(doc, f);
        f.close();
        Serial.println("Saved known devices to LittleFS");
    }
}

void processCommand(String line); // Forward declaration

SoftwareSerial swSerial(D6, D5); // RX = D6, TX = D5

// --- Buffer Implementation ---
struct QueueItem {
    uint8_t mac[6];
    uint8_t data[250];
    uint8_t len;
};

const int BUFFER_SIZE = 32;
QueueItem msgBuffer[BUFFER_SIZE];
volatile int head = 0;
volatile int tail = 0;

void enqueueMessage(const uint8_t* mac, const uint8_t* data, uint8_t len) {
    int nextHead = (head + 1) % BUFFER_SIZE;
    if (nextHead != tail) {
        memcpy(msgBuffer[head].mac, mac, 6);
        memcpy(msgBuffer[head].data, data, len);
        msgBuffer[head].len = len;
        head = nextHead;
    }
}

void onDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
    if (len == 0 || len > 250) return;
    uint8_t type = incomingData[0];
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (type == MSG_CONFIG && len >= sizeof(ConfigMessage)) {
        ConfigMessage config;
        memcpy(&config, incomingData, sizeof(ConfigMessage));
        String devName = String(config.deviceName);
        deviceNames[macStr] = devName;
        std::vector<uint8_t> macVec(mac, mac + 6);
        deviceMacs[devName] = macVec;

        if (stayAwakeState.count(devName) && stayAwakeState[devName]) {
            CmdMessage cmd;
            cmd.type = MSG_CMD;
            cmd.cmdType = CMD_OTA;
            cmd.value = true;
            esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
            esp_now_send(mac, (uint8_t *)&cmd, sizeof(CmdMessage));
            stayAwakeState[devName] = false; 
        }
    }
    
    if (type == MSG_DATA && len >= sizeof(DataMessage)) {
        // Also check if we need to wake up device on DATA message (in case CONFIG was lost)
        String devName = "";
        if (deviceNames.count(macStr)) {
            devName = deviceNames[macStr];
        }

        if (devName.length() > 0 && stayAwakeState.count(devName) && stayAwakeState[devName]) {
            CmdMessage cmd;
            cmd.type = MSG_CMD;
            cmd.cmdType = CMD_OTA;
            cmd.value = true;
            esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
            esp_now_send(mac, (uint8_t *)&cmd, sizeof(CmdMessage));
            stayAwakeState[devName] = false; 
            Serial.printf("Async OTA command sent to %s (via DATA)\n", devName.c_str());
        }
    }
    enqueueMessage(mac, incomingData, len);
}

void processBuffer() {
    while (head != tail) {
        QueueItem& item = msgBuffer[tail];
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
                 item.mac[0], item.mac[1], item.mac[2], item.mac[3], item.mac[4], item.mac[5]);

        uint8_t type = item.data[0];
        StaticJsonDocument<512> doc;
        doc["mac"] = macStr;

        if (type == MSG_CONFIG && item.len >= sizeof(ConfigMessage)) {
            ConfigMessage config;
            memcpy(&config, item.data, sizeof(ConfigMessage));
            bool alreadyKnown = deviceNames.count(macStr);
            if (!alreadyKnown) {
                deviceNames[macStr] = config.deviceName;
                saveKnownDevices();
            }
            // Always forward config so Transmitter can send discovery
            doc["type"] = "CONFIG";
            doc["deviceName"] = config.deviceName;
            doc["sensorFlags"] = config.sensorFlags;
            doc["sleepInterval"] = config.sleepInterval;
        } 
        else if (type == MSG_DATA && item.len >= sizeof(DataMessage)) {
            DataMessage data;
            memcpy(&data, item.data, sizeof(DataMessage));
            doc["type"] = "DATA";
            doc["deviceName"] = deviceNames.count(macStr) ? deviceNames[macStr] : "unknown";
            doc["sensorFlags"] = data.sensorFlags;
            doc["batteryVoltage"] = data.batteryVoltage;

            if (data.sensorFlags & SENSOR_FLAG_BME) {
                doc["temperature"] = data.bme.temperature;
                doc["humidity"] = data.bme.humidity;
                doc["pressure"] = data.bme.pressure;
            }
            if (data.sensorFlags & SENSOR_FLAG_LUX) {
                doc["lux"] = data.lux.lux;
            }
            if (data.sensorFlags & SENSOR_FLAG_SOIL) {
                doc["soil"] = data.soil.moisture;
            }
            if (data.sensorFlags & SENSOR_FLAG_BINARY) {
                doc["binaryState"] = data.binary.state;
            }
        }

        if (doc.containsKey("type")) {
            String json;
            serializeJson(doc, json);
            log("Gateway -> Transmitter: " + json);
            swSerial.println(json);
            delay(150); // Give transmitter time to process and avoid serial churn
        }
        tail = (tail + 1) % BUFFER_SIZE;
    }
}

// Convert command name to command type
uint8_t getCmdType(const char* cmdName) {
    String cmd = "CMD_" + String(cmdName);
    cmd.toUpperCase();
    if (cmd == "CMD_OTA") return CMD_OTA;
    if (cmd == "CMD_RESTART") return CMD_RESTART;
    if (cmd == "CMD_UPDATE") return CMD_UPDATE;
    if (cmd == "CMD_FLUSH") return CMD_FLUSH;
    if (cmd == "CMD_CONFIG" || cmd == "CMD_SEND_CONFIG") return CMD_CONFIG;
    if (cmd == "CMD_CALIBRATE") return CMD_OTA; // Reuse OTA mode for calibration
    return 0;
}

void processCommand(String line) {
     if (line.length() > 0) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, line);
            if (!error) {
                String targetDevice = doc.containsKey("device") ? doc["device"].as<String>() : "gateway";
                String slugTarget = slugify(targetDevice);
                String actualPrettyName = "";
                for (auto const& [mac, name] : deviceNames) {
                    if (slugify(name) == slugTarget) {
                        actualPrettyName = name;
                        break;
                    }
                }
                if (slugTarget == "gateway") actualPrettyName = "gateway";

                if (doc.containsKey("cmd")) {
                    const char* cmdName = doc["cmd"];
                    uint8_t cmdType = getCmdType(cmdName);
                    if (cmdType != 0) {
                        if (actualPrettyName == "gateway") {
                            if (cmdType == CMD_RESTART) {
                                log("Gateway RESTART requested...");
                                delay(100); ESP.restart();
                            } else if (cmdType == CMD_OTA) {
                                log("Gateway entering OTA mode via CMD...");
                                otaMode = true;
                                otaStatusSent = false;
                            } else if (cmdType == CMD_FLUSH) {
                                log("Gateway: Flushing known devices list...");
                                deviceNames.clear();
                                deviceMacs.clear();
                                saveKnownDevices();
                                log("Gateway: Devices list flushed.");
                            }
                        } else if (actualPrettyName != "" && deviceMacs.count(actualPrettyName)) {
                            if (cmdType == CMD_OTA) {
                                stayAwakeState[actualPrettyName] = true;
                                log("Gateway: Queued OTA/Calibrate for " + actualPrettyName);
                            } else {
                                CmdMessage cmd;
                                cmd.type = MSG_CMD;
                                cmd.cmdType = cmdType;
                                cmd.value = true;
                                std::vector<uint8_t>& macVec = deviceMacs[actualPrettyName];
                                uint8_t mac_addr[6];
                                std::copy(macVec.begin(), macVec.end(), mac_addr);
                                esp_now_add_peer(mac_addr, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
                                esp_now_send(mac_addr, (uint8_t*)&cmd, sizeof(CmdMessage));
                            }
                        }
                    }
                    return; 
                }
            }
        }
}

void setup() {
    Serial.begin(115200);
    swSerial.begin(9600);
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
    }
    loadKnownDevices();
    WiFi.mode(WIFI_STA);
    wifi_set_channel(1);
    if (esp_now_init() != 0) return;
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    delay(100);
    StaticJsonDocument<128> bootDoc;
    bootDoc["device"] = "gateway";
    bootDoc["status"] = "online"; 
    // Add connection info? In normal mode it's just ESP-NOW link to Transmitter
    // but maybe version or something. "online" is enough.
    String bootJson; serializeJson(bootDoc, bootJson);
    swSerial.println(bootJson);
}

void loop() {
    processBuffer();
    if (otaMode) {
        ArduinoOTA.handle();
    }

    if (swSerial.available() || Serial.available()) {
        Stream& input = swSerial.available() ? static_cast<Stream&>(swSerial) : static_cast<Stream&>(Serial);
        String line = input.readStringUntil('\n');
        processCommand(line);
    }

    if (otaMode) {
        static bool wifiInit = false;
        if (!wifiInit) {
            WiFiManager wm;
            wm.setDebugOutput(false);
            if (wm.autoConnect("ESP-NOW-GATEWAY-OTA")) {
                // Connected
            }
            wifiInit = true;
        }

        if (WiFi.status() == WL_CONNECTED) {
            static bool servicesStarted = false;
            if (!servicesStarted) {
                ArduinoOTA.setHostname("espnow-gateway");
                ArduinoOTA.onStart([]() { isOTAUpdating = true; log("OTA Starting..."); });
                ArduinoOTA.onEnd([]() { isOTAUpdating = false; log("OTA Complete!"); });
                ArduinoOTA.begin();
                telnetServer.begin();
                log("OTA Ready.");
                servicesStarted = true;
            }
            
            if (telnetServer.hasClient()) {
                WiFiClient nC = telnetServer.accept();
                if (!telnetClient || !telnetClient.connected()) {
                    if (telnetClient) telnetClient.stop();
                    telnetClient = nC;
                    log("Connected Telnet");
                } else nC.stop();
            }

             if (!otaStatusSent) {
                  StaticJsonDocument<128> sDoc;
                  sDoc["device"] = "gateway"; // Restore for routing
                  sDoc["status"] = "ota";
                  sDoc["connection"] = WiFi.localIP().toString();
                  String json; serializeJson(sDoc, json);
                  log(json); swSerial.println(json);
                  otaStatusSent = true;
            }
        }
    }

    // --- Heartbeat with Device Watchdog ---
    // Gateway sends a heartbeat every 30s so Transmitter knows it's alive.
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 30000) {
        lastHeartbeat = millis();
        StaticJsonDocument<64> doc;
        doc["type"] = "HEARTBEAT";
        doc["device"] = "gateway";
        String json; serializeJson(doc, json);
        swSerial.println(json);
        // log("Sent Heartbeat"); // Quiet to avoid spam
    }
}
