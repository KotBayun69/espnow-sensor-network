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

bool otaMode = false;
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool isOTAUpdating = false;
bool otaStatusSent = false;

// MQTT Configuration
char mqtt_server[40];
char mqtt_port[6] = "1883";
char mqtt_user[32];
char mqtt_pass[32];

WiFiClient espClient;
PubSubClient client(espClient);

void logToBoth(const String& msg, bool newline = true) {
    Serial.print(msg);
    if (newline) Serial.println();
    
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(msg);
        if (newline) telnetClient.println();
    }
}

void loadConfig() {
    if (LittleFS.begin()) {
        if (LittleFS.exists("/config.json")) {
            File configFile = LittleFS.open("/config.json", "r");
            if (configFile) {
                size_t size = configFile.size();
                std::unique_ptr<char[]> buf(new char[size]);
                configFile.readBytes(buf.get(), size);
                StaticJsonDocument<512> json;
                DeserializationError error = deserializeJson(json, buf.get());
                if (!error) {
                    strcpy(mqtt_server, json["mqtt_server"]);
                    strcpy(mqtt_port, json["mqtt_port"]);
                    strcpy(mqtt_user, json["mqtt_user"]);
                    strcpy(mqtt_pass, json["mqtt_pass"]);
                }
            }
        }
    }
}

void saveConfig() {
    StaticJsonDocument<512> json;
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_pass"] = mqtt_pass;

    File configFile = LittleFS.open("/config.json", "w");
    if (configFile) {
        serializeJson(json, configFile);
        configFile.close();
    }
}

void processCommand(String line); // Forward declaration

void reconnectMqtt() {
    if (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        String clientId = "ESPNOW-Gateway-" + String(ESP.getChipId(), HEX);
        if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println("connected");
            client.subscribe("esphome/control");
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            delay(5000);
        }
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }
    
    if (String(topic) == "esphome/control") {
        if (msg.indexOf("ota off") >= 0 || msg.indexOf("\"ota\":\"off\"") >= 0) {
            logToBoth("Exiting OTA mode via MQTT request...");
            otaMode = false;
            ESP.restart(); // Cleanest way to reset
        } else {
            // Pass other commands to existing processor
            processCommand(msg);
        }
    }
}

SoftwareSerial swSerial(D6, D5); // RX = D6, TX = D5

// Device name tracking: map MAC address to device name
std::map<String, String> deviceNames;
// MAC tracking: map device name to MAC address bytes for commanding
std::map<String, std::vector<uint8_t>> deviceMacs;
// OTA request tracking: store device names that should stay awake
std::map<String, bool> stayAwakeState;

#define MSG_CONFIG 1
#define MSG_DATA   2
#define MSG_ACK    3
#define MSG_CMD    4

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

typedef struct __attribute__((packed)) struct_cmd_message {
    uint8_t type;      // MSG_CMD (always 4)
    uint8_t cmdType;   // Which command (CMD_OTA, CMD_RESTART, etc.)
    bool value;        // Command state (true/false, on/off)
} CmdMessage;

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
    uint16_t motionTimeout;
} EnviroMotionConfig;

typedef struct __attribute__((packed)) {
    uint16_t sleepInterval;
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
    bool motionDetected;
    float distance;
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
    bool isBatteryPowered;
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

// --- Buffer Implementation ---
struct QueueItem {
    uint8_t mac[6];
    uint8_t data[250]; // ESP-NOW max payload is 250 bytes
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
    } else {
        // Buffer overflow - drop packet
    }
}
// -----------------------------

void onDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
    if (len == 0 || len > 250) return;
    uint8_t type = incomingData[0];
    
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 1. Send ACK
    // Only ACK DATA if we actually know the device name (otherwise it won't be processed correctly)
    if (type == MSG_CONFIG || (type == MSG_DATA && deviceNames.count(macStr))) {
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
        } else {
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
            
            // One-shot logic: Clear the flag after sending so the device can return to sleep/normal mode
            stayAwakeState[devName] = false;
            Serial.printf("Sent OTA command to %s and cleared flag\n", devName.c_str());
        }
    }

    // 2. Enqueue for Processing
    enqueueMessage(mac, incomingData, len);
}

void processBuffer() {
    while (head != tail) {
        // Read from tail
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
            
            Serial.printf("Registered device: %s -> %s (Type: %d)\n", macStr, config.deviceName, config.deviceType);
            
            doc["type"] = "CONFIG";
            doc["deviceName"] = config.deviceName;
            doc["deviceType"] = config.deviceType;
            doc["isBatteryPowered"] = config.isBatteryPowered;
            
            if (config.deviceType == DEV_PLANT) {
                doc["sleepInterval"] = config.config.plant.sleepInterval;
            } else if (config.deviceType == DEV_ENVIRO_MOTION) {
                doc["sleepInterval"] = config.config.enviro.sleepInterval;
                doc["motionTimeout"] = config.config.enviro.motionTimeout;
            } else if (config.deviceType == DEV_BINARY) {
                doc["sleepInterval"] = config.config.binary.sleepInterval;
            }
        } 
        else if (type == MSG_DATA && item.len >= sizeof(DataMessage)) {
            DataMessage data;
            memcpy(&data, item.data, sizeof(DataMessage));
            
            doc["type"] = "DATA";
            if (deviceNames.count(macStr)) {
                doc["deviceName"] = deviceNames[macStr];
            } else {
                doc["deviceName"] = "unknown";
            }
            doc["deviceType"] = data.deviceType;
            doc["batteryVoltage"] = data.batteryVoltage;

            if (data.deviceType == DEV_PLANT) {
                doc["temperature"] = data.data.plant.temperature;
                doc["humidity"] = data.data.plant.humidity;
                doc["pressure"] = data.data.plant.pressure;
                doc["lux"] = data.data.plant.lux;
                doc["soilMoisture"] = data.data.plant.soilMoisture;
            } else if (data.deviceType == DEV_ENVIRO_MOTION) {
                doc["temperature"] = data.data.enviro.temperature;
                doc["humidity"] = data.data.enviro.humidity;
                doc["pressure"] = data.data.enviro.pressure;
                doc["lux"] = data.data.enviro.lux;
                doc["motionDetected"] = data.data.enviro.motionDetected;
                doc["distance"] = data.data.enviro.distance;
            } else if (data.deviceType == DEV_BINARY) {
                doc["binaryState"] = data.data.binary.state;
            }
        }
 else if (type != MSG_ACK) {
            Serial.printf("Processing: Unknown message type %d from %s\n", type, macStr);
            // Advance tail and continue to next item
            tail = (tail + 1) % BUFFER_SIZE;
            continue; 
        }

        if (doc.containsKey("type")) {
            String json;
            serializeJson(doc, json);
            logToBoth(json);
            serializeJson(doc, swSerial);
            swSerial.println();
        }

        tail = (tail + 1) % BUFFER_SIZE;
    }
}

void setup() {
    Serial.begin(115200);
    swSerial.begin(9600);
    Serial.println();
    Serial.println("ESP-NOW Gateway Starting...");
    
    // Load config for MQTT
    loadConfig();

    WiFi.mode(WIFI_STA);

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

void processCommand(String line) {
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
                                logToBoth("Gateway entering OTA/MAINTENANCE mode...");
                                otaMode = true;
                                otaStatusSent = false;
                            } else {
                                logToBoth("Gateway exiting OTA mode (rebooting)...");
                                // Send status to Transmitter
                                StaticJsonDocument<128> statusDoc;
                                statusDoc["device"] = "gateway";
                                statusDoc["connection"] = "espnow";
                                statusDoc["status"] = "online";
                                String json;
                                serializeJson(statusDoc, json);
                                logToBoth(json);
                                swSerial.println(json); // Send to Transmitter
                                
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
                            cmd.cmdType = CMD_OTA;
                            cmd.value = cmdValue;
                            
                            std::vector<uint8_t>& macVec = deviceMacs[devName];
                            uint8_t mac_addr[6];
                            std::copy(macVec.begin(), macVec.end(), mac_addr);
                            
                            esp_now_add_peer(mac_addr, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
                            esp_now_send(mac_addr, (uint8_t*)&cmd, sizeof(CmdMessage));
                            
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

void loop() {
    // Process any incoming messages from the buffer
    processBuffer();

    if (otaMode) {
        ArduinoOTA.handle();
        client.loop(); // MQTT Loop
        if (isOTAUpdating) return;
    }

    // Handle commands from Transmitter via swSerial or Serial
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
            
            // Custom MQTT parameters
            WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
            WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
            WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqtt_user, 32);
            WiFiManagerParameter custom_mqtt_pass("pass", "mqtt pass", mqtt_pass, 32);

            wm.addParameter(&custom_mqtt_server);
            wm.addParameter(&custom_mqtt_port);
            wm.addParameter(&custom_mqtt_user);
            wm.addParameter(&custom_mqtt_pass);

            // Set save config callback
            wm.setSaveConfigCallback([]() {
                // We will save in the loop after param read, or just here. 
                // However, wm variables need to be copied back.
                // We'll rely on reading them back after autoConnect returns.
            });

            logToBoth("Gateway: Connecting to WiFi for OTA/MQTT...");
            
            // Only use config portal if autoconnect fails
            if (!wm.autoConnect("ESP-NOW-GATEWAY-OTA")) {
                logToBoth("Failed to connect or timed out. Staying in ota mode.");
            } else {
                logToBoth("WiFi Connected! IP: " + WiFi.localIP().toString());
                
                // Read updated parameters
                strcpy(mqtt_server, custom_mqtt_server.getValue());
                strcpy(mqtt_port, custom_mqtt_port.getValue());
                strcpy(mqtt_user, custom_mqtt_user.getValue());
                strcpy(mqtt_pass, custom_mqtt_pass.getValue());
                saveConfig();
            }
            wifiInit = true;
        }

        if (WiFi.status() == WL_CONNECTED) {
            static bool servicesStarted = false;
            if (!servicesStarted) {
                // Setup MQTT
                client.setServer(mqtt_server, atoi(mqtt_port));
                client.setCallback(callback);

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
            
            // Ensure MQTT is connected
            if (!client.connected()) {
                reconnectMqtt();
            }

            // Send deferred OTA status to Transmitter once connected
            if (!otaStatusSent) {
                 StaticJsonDocument<128> statusDoc;
                 statusDoc["device"] = "gateway";
                 statusDoc["connection"] = WiFi.localIP().toString();
                 statusDoc["status"] = "ota";
                 String json;
                 serializeJson(statusDoc, json);
                 logToBoth(json); 
                 swSerial.println(json);
                 otaStatusSent = true;
            }
        }
    }
}
