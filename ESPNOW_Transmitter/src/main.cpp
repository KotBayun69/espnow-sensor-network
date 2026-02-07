#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <map>
#include <vector>

#include "CommonUtils.h"
#include "protocol.h"

// Forward declarations
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool isOTAUpdating = false;

// Global log helper
void log(const String& msg, bool newline = true) {
    logToBoth(msg, newline, telnetClient);
}

MqttConfig mqtt_cfg;
const char* mqtt_topic_base = "espnow"; 

// Track discovered devices
std::map<String, bool> discoveredDevices;

WiFiClient espClient;
PubSubClient client(espClient);
SoftwareSerial swSerial(D6, D5); // RX = D6, TX = D5
bool shouldSaveConfig = false;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void loadConfig() {
  if (!loadBaseConfig(mqtt_cfg, "/config.json")) {
    strlcpy(mqtt_cfg.server, "192.168.1.101", 40);
  }
}

void saveConfig() {
  saveBaseConfig(mqtt_cfg, "/config.json");
}


void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';
    
    String topicStr = String(topic);
    if (topicStr.startsWith("espnow/") && topicStr.endsWith("/control")) {
        int firstSlash = topicStr.indexOf('/');
        int lastSlash = topicStr.lastIndexOf('/');
        String topicDeviceName = topicStr.substring(firstSlash + 1, lastSlash);
        
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, message);
        if (!error) {
        if (topicDeviceName != "gateway") {
            doc["device"] = topicDeviceName;
        } else {
            doc.remove("device");
        }
            String relayedJson;
            serializeJson(doc, relayedJson);
            log("Control for [" + topicDeviceName + "]: " + relayedJson);
            swSerial.println(relayedJson);
            
            if (doc["cmd"] == "send_config") {
                String slugName = slugify(topicDeviceName);
                log("Clearing discovery cache for: " + slugName);
                discoveredDevices.erase(slugName);
            }

            if (topicDeviceName == "transmitter") {
               if (doc["cmd"] == "restart") {
                   log("RESTART requested");
                   delay(100); ESP.restart();
               }
               if (doc["cmd"] == "ota") {
                   log("OTA Starting...");
                   ArduinoOTA.begin();
                   StaticJsonDocument<128> sDoc;
                   sDoc["connection"] = WiFi.localIP().toString();
                   sDoc["status"] = "ota";
                   char buf[128]; serializeJson(sDoc, buf);
                   client.publish("espnow/transmitter/state", buf);
               }
            }
        }
    }
}

void reconnect() {
    static unsigned long lastReconnectAttempt = 0;
    unsigned long now = millis();
    if (now - lastReconnectAttempt < 5000) return;
    lastReconnectAttempt = now;

    log("Attempting MQTT connection to " + String(mqtt_cfg.server));
    client.setServer(mqtt_cfg.server, mqtt_cfg.port);
    // Use fixed Client ID based on MAC to ensure session takeover
    String clientId = "ESPNOW-Transmitter-" + WiFi.macAddress();
    clientId.replace(":", "");
    
    // LWT: Topic, QoS, Retain, Payload
    if (client.connect(clientId.c_str(), mqtt_cfg.user, mqtt_cfg.pass, 
                       "espnow/transmitter/state", 1, true, "{\"status\":\"offline\"}")) {
        log("✓ connected");
        client.subscribe("espnow/+/control");
        StaticJsonDocument<128> doc;
        doc["connection"] = WiFi.localIP().toString();
        doc["status"] = "online"; 
        char buffer[128];
        serializeJson(doc, buffer);
        if (client.publish("espnow/transmitter/state", buffer, true)) {
            log("State published: ONLINE");
        } else {
            log("State publish failed (ONLINE)");
        }
        lastReconnectAttempt = 0; // Reset timer on success
    } else {
        static int mqttFailures = 0;
        mqttFailures++;
        log("✗ failed, rc=" + String(client.state()) + " (" + String(mqttFailures) + "/3)");
        if (mqttFailures >= 3) {
            log("Too many failures. Starting Config Portal...");
            startMqttConfigPortal(mqtt_cfg, "ESPNOW-Transmitter");
            mqttFailures = 0;
        }
    }
}

void publishDiscoveryWithMac(const JsonVariantConst& config, const char* macAddress) {
    const char* deviceName = config["deviceName"];
    if (deviceName == nullptr) return;
    String slugName = slugify(deviceName);
    if (discoveredDevices.count(slugName)) return;
    
    int sensorFlags = config["sensorFlags"] | 0;
    int sleepInterval = config["sleepInterval"] | 15; 
    int expireAfter = (sleepInterval * 3) + 20;
    // Use MAC as unique ID source if available, otherwise fallback to deviceName
    String uniqueIdBase = String(deviceName);
    if (macAddress && strlen(macAddress) > 0) {
        uniqueIdBase = String(macAddress);
        uniqueIdBase.replace(":", "");
    }

    auto publishEntity = [&](const char* component, const char* entityKey, const char* name, 
                             const char* devClass, const char* unit, const char* valTpl, 
                             const char* statClass = "measurement") {
        
        DynamicJsonDocument doc(1024);
        String discoveryTopic = String("homeassistant/") + component + "/" + slugify(deviceName) + "/" + entityKey + "/config";
        
        doc["name"] = name; // Short name, HA prepends device name
        doc["stat_t"] = String(mqtt_topic_base) + "/" + slugify(deviceName) + "/state";
        doc["uniq_id"] = uniqueIdBase + "_" + entityKey;
        doc["val_tpl"] = valTpl;
        doc["exp_aft"] = expireAfter;
        if (devClass) doc["dev_cla"] = devClass;
        if (unit) doc["unit_of_meas"] = unit;
        if (statClass) doc["stat_cla"] = statClass;

        JsonObject device = doc.createNestedObject("dev");
        JsonArray identifiers = device.createNestedArray("ids");
        if (macAddress && strlen(macAddress) > 0) identifiers.add(macAddress);
        identifiers.add(deviceName); // Fallback/Secondary ID
        device["name"] = deviceName;
        device["mdl"] = "ESP-NOW Sensor";
        device["mf"] = "Antigravity";

        char buffer[1024];
        serializeJson(doc, buffer);
        if (client.publish(discoveryTopic.c_str(), buffer, true)) {
            log("✓ Published discovery: " + discoveryTopic);
        } else {
            log("✗ Failed to publish discovery: " + discoveryTopic + " (Payload too large?)");
        }
    };

    // Always publish Battery
    publishEntity("sensor", "battery", "Battery", "voltage", "V", "{{ value_json.batteryVoltage | round(2) }}");

    if (sensorFlags & SENSOR_FLAG_BME) {
        publishEntity("sensor", "temperature", "Temperature", "temperature", "°C", "{{ value_json.temperature | round(1) }}");
        publishEntity("sensor", "humidity", "Humidity", "humidity", "%", "{{ value_json.humidity | round(1) }}");
        publishEntity("sensor", "pressure", "Pressure", "pressure", "hPa", "{{ value_json.pressure | round(1) }}");
    }

    if (sensorFlags & SENSOR_FLAG_LUX) {
        publishEntity("sensor", "lux", "Illuminance", "illuminance", "lx", "{{ value_json.lux | round(1) }}");
    }

    if (sensorFlags & SENSOR_FLAG_SOIL) {
        publishEntity("sensor", "soil", "Soil Moisture", "moisture", "%", "{{ value_json.soil | round(1) }}");
    }

    if (sensorFlags & SENSOR_FLAG_BINARY) {
        // Binary Sensor
        // Note: component is "binary_sensor", stat_class is usually null for binary
        DynamicJsonDocument doc(1024);
        String entityKey = "binary";
        String discoveryTopic = String("homeassistant/binary_sensor/") + slugify(deviceName) + "/" + entityKey + "/config";
        
        doc["name"] = "Binary Sensor";
        doc["stat_t"] = String(mqtt_topic_base) + "/" + slugify(deviceName) + "/state";
        doc["uniq_id"] = uniqueIdBase + "_" + entityKey;
        doc["val_tpl"] = "{{ 'ON' if value_json.binaryState else 'OFF' }}";
        doc["exp_aft"] = expireAfter;
        // doc["dev_cla"] = "door"; // Optional, can let user customize in HA

        JsonObject device = doc.createNestedObject("dev");
        JsonArray identifiers = device.createNestedArray("ids");
        if (macAddress && strlen(macAddress) > 0) identifiers.add(macAddress);
        identifiers.add(deviceName);
        device["name"] = deviceName;
        device["mdl"] = "ESP-NOW Sensor";
        device["mf"] = "Antigravity";

        char buffer[1024];
        serializeJson(doc, buffer);
        if (client.publish(discoveryTopic.c_str(), buffer, true)) {
            log("✓ Published discovery: " + discoveryTopic);
        } else {
            log("✗ Failed to publish discovery: " + discoveryTopic);
        }
    }
    
    // Helper for Buttons
    auto publishButton = [&](const char* slug, const char* name, const char* cmdPayload, const char* icon = "mdi:gesture-tap-button") {
        DynamicJsonDocument doc(1024);
        String discoveryTopic = String("homeassistant/button/") + slugify(deviceName) + "/" + slug + "/config";
        
        doc["name"] = name;
        doc["cmd_t"] = String(mqtt_topic_base) + "/" + slugify(deviceName) + "/control";
        doc["pl_prs"] = cmdPayload;
        doc["uniq_id"] = uniqueIdBase + "_btn_" + slug;
        doc["ic"] = icon;
        doc["ret"] = false;

        JsonObject device = doc.createNestedObject("dev");
        JsonArray identifiers = device.createNestedArray("ids");
        if (macAddress && strlen(macAddress) > 0) identifiers.add(macAddress);
        identifiers.add(deviceName);
        device["name"] = deviceName;
        device["mdl"] = "ESP-NOW Sensor";
        device["mf"] = "Antigravity";

        char buffer[1024];
        serializeJson(doc, buffer);
        if (client.publish(discoveryTopic.c_str(), buffer, true)) {
            log("✓ Published button: " + String(name));
        } else {
            log("✗ Failed to publish button: " + String(name));
        }
    };

    // Standard Buttons
    publishButton("restart", "Restart Device", "{\"cmd\": \"restart\"}", "mdi:restart");
    publishButton("ota", "Wake Up / OTA", "{\"cmd\": \"ota\"}", "mdi:cloud-upload");

    if (sensorFlags & SENSOR_FLAG_SOIL) {
        publishButton("calibrate", "Calibrate Soil Sensor", "{\"cmd\": \"calibrate\"}", "mdi:water-percent");
    }
    
    discoveredDevices[slugName] = true;
}

void setup() {
    Serial.begin(115200);
    swSerial.begin(9600);
    if(!LittleFS.begin()){
        Serial.println("LittleFS mount failed");
    }
    loadConfig();

    WiFiManager wm;
    wm.setSaveConfigCallback(saveConfigCallback);
    // Use smaller timeout for auto-connect at boot
    wm.setConnectTimeout(20); 

    if (!wm.autoConnect("ESPNOW-Transmitter")) {
        log("Failed to connect via autoConnect. Starting Config Portal...");
        startMqttConfigPortal(mqtt_cfg, "ESPNOW-Transmitter");
    }

    client.setServer(mqtt_cfg.server, mqtt_cfg.port);
    client.setCallback(mqtt_callback);
    client.setBufferSize(2048); 

    ArduinoOTA.onStart([]() { isOTAUpdating = true; log("OTA Starting..."); });
    ArduinoOTA.onEnd([]() { isOTAUpdating = false; log("OTA Complete!"); });
    ArduinoOTA.begin();
    telnetServer.begin();
    log("Ready. IP: " + WiFi.localIP().toString());
}

void loop() {

    static unsigned long lastGatewayHeartbeat = 0;
    static bool gatewayOnline = false;
    
    ArduinoOTA.handle();
    if (isOTAUpdating) return;
    if (!client.connected()) reconnect();
    client.loop();

    if (telnetServer.hasClient()) {
        WiFiClient nC = telnetServer.accept();
        if (!telnetClient || !telnetClient.connected()) {
            if (telnetClient) telnetClient.stop();
            telnetClient = nC;
            log("Connected Telnet");
        } else nC.stop();
    }

    while (swSerial.available()) {
        char c = swSerial.read();
        static String inputBuffer = "";
        if (c == '\n') {
            if (inputBuffer.length() > 0) {
                DynamicJsonDocument doc(1280); // Slightly larger
                DeserializationError error = deserializeJson(doc, inputBuffer);
                if (!error) {
                    const char* type = doc["type"];
                    const char* deviceName = doc["deviceName"];
                    log("Transmitter: Received " + String(type ? type : "null") + " from Gateway for: " + String(deviceName ? deviceName : "null"));
                    
                    if (doc["type"] == "CONFIG" && deviceName) {
                        publishDiscoveryWithMac(doc, doc["mac"] | "");
                    } else if (doc["type"] == "HEARTBEAT") {
                        // Update watchdog
                        lastGatewayHeartbeat = millis();
                        if (!gatewayOnline) {
                            log("Gateway is ONLINE (Heartbeat)");
                            gatewayOnline = true;
                            // Publish online status
                            if (client.connected()) {
                                client.publish("espnow/gateway/state", "{\"status\":\"online\"}", true);
                            }
                        }
                    } else if (deviceName) {
                        // ... existing state/control handling ...
                        String topic = String(mqtt_topic_base) + "/" + slugify(deviceName) + "/state";
                        doc.remove("deviceName");
                        doc.remove("type");
                        doc.remove("mac");
                        String payload; serializeJson(doc, payload);
                        client.publish(topic.c_str(), payload.c_str());
                    } else if (doc["device"] == "gateway") {
                         doc.remove("device"); // Strip routing field
                         String payload; serializeJson(doc, payload);
                         client.publish("espnow/gateway/state", payload.c_str(), true); // Retain gateway status
                         
                         // Treat any gateway message as a heartbeat
                         lastGatewayHeartbeat = millis();
                         if (!gatewayOnline) gatewayOnline = true; 
                    }
                } else {
                    log("Transmitter: JSON Error: " + String(error.c_str()) + " in buffer: " + inputBuffer);
                }
                inputBuffer = "";
            }
        } else if (c != '\r') inputBuffer += c;
    }

    // --- Gateway Watchdog ---
    // Monitor heartbeat from Gateway
    // Variables moved to top of loop()

    // Check if we received a heartbeat recently (timeout: 70s > 2 missed heartbeats)
    if (millis() - lastGatewayHeartbeat > 70000) {
        if (gatewayOnline) {
            log("Gateway is OFFLINE (Watchdog)");
            gatewayOnline = false;
            // Publish offline status
             if (client.connected()) {
                client.publish("espnow/gateway/state", "{\"status\":\"offline\"}", true);
             }
        }
    }
}
