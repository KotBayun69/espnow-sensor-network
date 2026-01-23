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

bool inMaintenanceMode = false;
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool isOTAUpdating = false;

void logToBoth(const String& msg, bool newline = true) {
    Serial.print(msg);
    if (newline) Serial.println();
    
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(msg);
        if (newline) telnetClient.println();
    }
}

SoftwareSerial swSerial(D6, D5); // RX = D6, TX = D5

// Device name tracking: map MAC address to device name
std::map<String, String> deviceNames;
// MAC tracking: map device name to MAC address bytes for commanding
std::map<String, std::vector<uint8_t>> deviceMacs;
// OTA request tracking: store device names that should stay awake
std::map<String, bool> stayAwakeState;

enum MessageType {
    MSG_CONFIG = 1,
    MSG_DATA = 2,
    MSG_ACK = 3,
    MSG_CMD = 4
};


// Command types for CMD messages
enum CmdType {
    CMD_OTA = 1,
    CMD_RESTART = 2
};

// Command name to type mapping
std::map<String, uint8_t> cmdMap = {
    {"CMD_OTA", CMD_OTA},
    {"CMD_RESTART", CMD_RESTART}
};

// Convert command name to command type
uint8_t getCmdType(const char* cmdName) {
    String cmd = "CMD_" + String(cmdName);
    cmd.toUpperCase();
    
    if (cmdMap.count(cmd)) {
        return cmdMap[cmd];
    }
    return 0; // Unknown command
}


typedef struct __attribute__((packed)) struct_ack_message {
    uint8_t type;
} AckMessage;

typedef struct __attribute__((packed)) struct_cmd_message {
    uint8_t type;      // MSG_CMD (always 4)
    uint8_t cmdType;   // Which command (CMD_OTA, CMD_RESTART, etc.)
    bool value;        // Command state (true/false, on/off)
} CmdMessage;



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

void onDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
    if (len == 0) return;
    uint8_t type = incomingData[0];
    
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 1. Send ACK
    if (type == MSG_CONFIG || type == MSG_DATA) {
        AckMessage ack;
        ack.type = MSG_ACK;
        String devName = "";

        if (type == MSG_CONFIG && len >= sizeof(ConfigMessage)) {
            ConfigMessage config;
            memcpy(&config, incomingData, sizeof(ConfigMessage));
            devName = String(config.deviceName);
            deviceNames[macStr] = devName; // Learn/Refresh name
            
            // Store MAC for this device name
            std::vector<uint8_t> macVec(mac, mac + 6);
            deviceMacs[devName] = macVec;
        } else if (deviceNames.count(macStr)) {
            devName = deviceNames[macStr];
        }

        // Send ACK
        esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
        esp_now_send(mac, (uint8_t *)&ack, sizeof(AckMessage));
        
        // 2. Send CMD if OTA is requested
        if (devName != "" && stayAwakeState.count(devName) && stayAwakeState[devName]) {
            CmdMessage cmd;
            cmd.type = MSG_CMD;
            cmd.cmdType = CMD_OTA;
            cmd.value = true;
            
            delay(10); // Small delay between messages
            esp_now_send(mac, (uint8_t *)&cmd, sizeof(CmdMessage));
            
            Serial.printf("Sent CMD: OTA=ON for device '%s'\n", devName.c_str());
            // Note: Don't clear the flag yet - sensor will send multiple DATA messages while awake
        }
    }




    // 2. Data Processing and Logging
    StaticJsonDocument<512> doc;
    doc["mac"] = macStr;

    if (type == MSG_CONFIG && len >= sizeof(ConfigMessage)) {
        ConfigMessage config;
        memcpy(&config, incomingData, sizeof(ConfigMessage));
        
        Serial.printf("Registered device: %s -> %s\n", macStr, config.deviceName);
        
        doc["type"] = "CONFIG";
        doc["deviceName"] = config.deviceName;
        doc["hasBME"] = config.hasBME;
        doc["hasBH1750"] = config.hasBH1750;
        doc["hasBattery"] = config.hasBattery;
        doc["hasBinary"] = config.hasBinary;
        doc["hasAnalog"] = config.hasAnalog;
    } 
    else if (type == MSG_DATA && len >= sizeof(DataMessage)) {
        DataMessage data;
        memcpy(&data, incomingData, sizeof(DataMessage));
        
        doc["type"] = "DATA";
        if (deviceNames.count(macStr)) {
            doc["deviceName"] = deviceNames[macStr];
        } else {
            doc["deviceName"] = "unknown";
            // If unknown, we might want to tell the sensor to re-identify 
            // but for now just log it.
        }
        
        doc["temperature"] = data.temperature;
        doc["humidity"] = data.humidity;
        doc["pressure"] = data.pressure;
        doc["lux"] = data.lux;
        doc["batteryVoltage"] = data.batteryVoltage;
        doc["binaryState"] = data.binaryState;
        doc["analogValue"] = data.analogValue;
    } else if (type != MSG_ACK) {
        Serial.printf("Unknown message type %d from %s\n", type, macStr);
        return;
    }

    if (doc.containsKey("type")) {
        String json;
        serializeJson(doc, json);
        logToBoth(json);
        serializeJson(doc, swSerial);
        swSerial.println();
    }
}

void setup() {
    Serial.begin(115200);
    swSerial.begin(9600);
    Serial.println();
    Serial.println("ESP-NOW Gateway Starting...");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Use a fixed channel
    uint8_t channel = 1;
    wifi_set_channel(channel);
    Serial.printf("WiFi Channel: %d\n", wifi_get_channel());

    if (esp_now_init() != 0) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }

    Serial.print("Gateway MAC Address: ");
    Serial.println(WiFi.macAddress());

    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("Ready to receive messages.");
}

void loop() {
    if (inMaintenanceMode) {
        ArduinoOTA.handle();
        if (isOTAUpdating) return;
    }

    // Handle commands from Transmitter via swSerial or Serial
    if (swSerial.available() || Serial.available()) {
        Stream& input = swSerial.available() ? static_cast<Stream&>(swSerial) : static_cast<Stream&>(Serial);
        String line = input.readStringUntil('\n');
        if (line.length() > 0) {
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, line);
            if (!error && doc.containsKey("device")) {
                const char* devName = doc["device"];
                
                // Process commands dynamically
                for (JsonPair kv : doc.as<JsonObject>()) {
                    const char* key = kv.key().c_str();
                    
                    // Skip device field
                    if (strcmp(key, "device") == 0) continue;
                    
                    // Check if it's the gateway's own command
                    if (strcmp(devName, "gateway") == 0) {
                        // Gateway OTA logic remains special case
                        if (strcmp(key, "ota") == 0) {
                            const char* value = kv.value().as<const char*>();
                            if (strcmp(value, "on") == 0) {
                                logToBoth("Gateway entering MAINTENANCE/OTA mode...");
                                inMaintenanceMode = true;
                            } else {
                                logToBoth("Gateway exiting MAINTENANCE mode (rebooting)...");
                                delay(100);
                                ESP.restart();
                            }
                        }
                        continue; // Gateway doesn't relay to sensors
                    }
                    
                    // Try to map to command type for sensors
                    uint8_t cmdType = getCmdType(key);
                    
                    if (cmdType != 0) {
                        const char* valueStr = kv.value().as<const char*>();
                        bool cmdValue = (strcmp(valueStr, "on") == 0);
                        
                        // Store OTA state for CMD message sending
                        if (cmdType == CMD_OTA) {
                            stayAwakeState[devName] = cmdValue;
                            logToBoth("OTA FLAG " + String(cmdValue ? "SET" : "CLEARED") + ": Device '" + String(devName) + "'");
                        }
                        
                        // Send CMD to device if we have its MAC
                        if (deviceMacs.count(devName)) {
                            CmdMessage cmd;
                            cmd.type = MSG_CMD;
                            cmd.cmdType = cmdType;
                            cmd.value = cmdValue;
                            
                            std::vector<uint8_t>& macVec = deviceMacs[devName];
                            uint8_t mac[6];
                            std::copy(macVec.begin(), macVec.end(), mac);
                            
                            esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
                            esp_now_send(mac, (uint8_t*)&cmd, sizeof(CmdMessage));
                            
                            Serial.printf("Sent CMD to %s: type=%d, value=%s\n", 
                                        devName, cmdType, cmdValue ? "ON" : "OFF");
                        } else {
                            Serial.printf("Device %s not registered yet, command queued\n", devName);
                        }
                    }
                }
            } else if (error) {
                logToBoth("JSON parse error on swSerial/Serial: " + String(error.c_str()));
            }
        }
    }

    if (inMaintenanceMode) {
        static bool wifiInit = false;
        if (!wifiInit) {
            WiFiManager wm;
            wm.setDebugOutput(false);
            // Set a short timeout for the config portal if needed, 
            // but for maintenance mode we probably want it to stay open until configured.
            logToBoth("Gateway: Starting Config Portal 'ESP-NOW-GATEWAY-OTA'...");
            if (!wm.autoConnect("ESP-NOW-GATEWAY-OTA")) {
                logToBoth("Failed to connect or timed out. Staying in maintenance mode.");
            } else {
                logToBoth("WiFi Connected! IP: " + WiFi.localIP().toString());
            }
            wifiInit = true;
        }

        if (WiFi.status() == WL_CONNECTED) {
            static bool servicesStarted = false;
            if (!servicesStarted) {
                ArduinoOTA.setHostname("espnow-gateway");

                ArduinoOTA.onStart([]() {
                    isOTAUpdating = true;
                    logToBoth("\nOTA Update Starting...");
                });
                ArduinoOTA.onEnd([]() {
                    isOTAUpdating = false;
                    logToBoth("\nOTA Update Complete!");
                });
                ArduinoOTA.onError([](ota_error_t error) {
                    isOTAUpdating = false;
                    String msg = "OTA Error[" + String(error) + "]: ";
                    if (error == OTA_AUTH_ERROR) msg += "Auth Failed";
                    else if (error == OTA_BEGIN_ERROR) msg += "Begin Failed";
                    else if (error == OTA_CONNECT_ERROR) msg += "Connect Failed";
                    else if (error == OTA_RECEIVE_ERROR) msg += "Receive Failed";
                    else if (error == OTA_END_ERROR) msg += "End Failed";
                    logToBoth(msg);
                });

                ArduinoOTA.setPort(8266);
                ArduinoOTA.begin();
                telnetServer.begin();
                logToBoth("OTA Ready. IP: " + WiFi.localIP().toString());
                servicesStarted = true;
            }
            
            // Handle Telnet
            if (telnetServer.hasClient()) {
                WiFiClient newClient = telnetServer.accept();
                if (!telnetClient || !telnetClient.connected()) {
                    if (telnetClient) telnetClient.stop();
                    telnetClient = newClient;
                    telnetClient.println("\n--- Connected to ESP-NOW Gateway Telnet Log ---");
                } else {
                    newClient.stop();
                }
            }
        }
    }
}
