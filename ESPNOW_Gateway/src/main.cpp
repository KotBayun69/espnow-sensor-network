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
#include <PubSubClient.h>

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

MqttConfig mqtt_cfg;
WiFiClient espClient;
PubSubClient client(espClient);

// Device tracking maps
std::map<String, String> deviceNames;
std::map<String, std::vector<uint8_t>> deviceMacs;
std::map<String, bool> stayAwakeState;

void loadConfig() {
    if (!loadBaseConfig(mqtt_cfg, "/config.json")) {
        Serial.println("Using default MQTT settings");
    }
}

void saveConfig() {
    saveBaseConfig(mqtt_cfg, "/config.json");
}

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

void reconnectMqtt() {
    if (!client.connected()) {
        log("Attempting MQTT connection to " + String(mqtt_cfg.server) + "...");
        client.setServer(mqtt_cfg.server, mqtt_cfg.port);
        String clientId = "ESPNOW-Gateway-" + String(ESP.getChipId(), HEX);
        if (client.connect(clientId.c_str(), mqtt_cfg.user, mqtt_cfg.pass)) {
            log("✓ MQTT connected");
            client.subscribe("espnow/gateway/control");
        } else {
            log("✗ MQTT failed, rc=" + String(client.state()));
            delay(5000);
        }
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }
    
    if (String(topic) == "espnow/gateway/control") {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, msg);
        if (!error) {
            doc["device"] = "gateway"; 
            String normalized;
            serializeJson(doc, normalized);
            processCommand(normalized);
        }
    }
}

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
            Serial.printf("Async OTA command sent to %s\n", devName.c_str());
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
            if (data.sensorFlags & SENSOR_FLAG_ADC) {
                doc["adc"] = data.adc.adcValue;
            }
            if (data.sensorFlags & SENSOR_FLAG_BINARY) {
                doc["binaryState"] = data.binary.state;
            }
        }

        if (doc.containsKey("type")) {
            String json;
            serializeJson(doc, json);
            log(json);
            swSerial.println(json);
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
                            }
                        } else if (actualPrettyName != "" && deviceMacs.count(actualPrettyName)) {
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
                    return; 
                }

                if (doc.containsKey("ota")) {
                    const char* val = doc["ota"];
                    bool otaOn = (strcmp(val, "on") == 0);
                    if (actualPrettyName == "gateway") {
                        if (otaOn) { 
                            otaMode = true; 
                            otaStatusSent = false; 
                        } else { 
                            if (client.connected()) {
                                StaticJsonDocument<128> sDoc;
                                sDoc["status"] = "restarting"; // or "offline"
                                String json; serializeJson(sDoc, json);
                                client.publish("espnow/gateway/state", json.c_str());
                                delay(200); // Give time to flush
                            }
                            // Also send to transmitter just in case
                            StaticJsonDocument<128> tDoc;
                            tDoc["device"] = "gateway";
                            tDoc["status"] = "restarting";
                            String tJson; serializeJson(tDoc, tJson);
                            swSerial.println(tJson);

                            delay(100); 
                            ESP.restart(); 
                        }
                    } else if (actualPrettyName != "") {
                        stayAwakeState[actualPrettyName] = otaOn;
                    }
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
    loadConfig();
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
        client.loop();
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
            WiFiManagerParameter c_server("server", "mqtt server", mqtt_cfg.server, 40);
            char pStr[6]; itoa(mqtt_cfg.port, pStr, 10);
            WiFiManagerParameter c_port("port", "mqtt port", pStr, 6);
            WiFiManagerParameter c_user("user", "mqtt user", mqtt_cfg.user, 32);
            WiFiManagerParameter c_pass("pass", "mqtt pass", mqtt_cfg.pass, 32);
            wm.addParameter(&c_server); wm.addParameter(&c_port);
            wm.addParameter(&c_user); wm.addParameter(&c_pass);

            if (wm.autoConnect("ESP-NOW-GATEWAY-OTA")) {
                strlcpy(mqtt_cfg.server, c_server.getValue(), 40);
                mqtt_cfg.port = atoi(c_port.getValue());
                strlcpy(mqtt_cfg.user, c_user.getValue(), 32);
                strlcpy(mqtt_cfg.pass, c_pass.getValue(), 32);
                saveConfig();
            }
            wifiInit = true;
        }

        if (WiFi.status() == WL_CONNECTED) {
            static bool servicesStarted = false;
            if (!servicesStarted) {
                client.setServer(mqtt_cfg.server, mqtt_cfg.port);
                client.setCallback(callback);
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
            if (!client.connected()) reconnectMqtt();

             if (!otaStatusSent) {
                  StaticJsonDocument<128> sDoc;
                  sDoc["device"] = "gateway"; // Restore for routing
                  sDoc["status"] = "ota";
                  sDoc["connection"] = WiFi.localIP().toString();
                  String json; serializeJson(sDoc, json);
                 log(json); swSerial.println(json);
                 if (client.connected()) client.publish("espnow/gateway/state", json.c_str());
                 otaStatusSent = true;
            }
        }
    }
}
