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

// Device Types
enum DeviceType : uint8_t {
    DEV_PLANT = 1,         // BME + Light + Soil
    DEV_ENVIRO_MOTION = 2, // BME + Light + LD2410
    DEV_BINARY = 3         // Door/Binary sensor
};

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

// MQTT Configuration
const char* mqtt_server = "192.168.1.101";
const char* mqtt_topic_base = "espnow"; // Base topic for device-specific topics
char mqtt_user[40] = "";
char mqtt_pass[40] = "";

// Track discovered devices to avoid sending duplicate discovery messages
std::map<String, bool> discoveredDevices;

WiFiClient espClient;
PubSubClient client(espClient);

// SoftwareSerial pins to connect to Gateway:
// Gateway D5 (TX) -> Transmitter D6 (RX)
// Gateway D6 (RX) -> Transmitter D5 (TX)
SoftwareSerial swSerial(D6, D5); // RX = D6, TX = D5

bool shouldSaveConfig = false;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void loadConfig() {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        DynamicJsonDocument doc(512);
        if (!deserializeJson(doc, buf.get())) {
          strcpy(mqtt_user, doc["mqtt_user"] | "");
          strcpy(mqtt_pass, doc["mqtt_pass"] | "");
          Serial.println("Config loaded from file");
        }
        configFile.close();
      }
    }
  }
}

void saveConfig() {
  DynamicJsonDocument doc(512);
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_pass"] = mqtt_pass;
  File configFile = LittleFS.open("/config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("Config saved to file");
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';
    
    Serial.printf("Message arrived on topic [%s]: %s\n", topic, message);
    
    if (strcmp(topic, "espnow/control") == 0) {
        // Expected payload: {"device": "NAME", "ota": "on"|"off"} (or other commands)
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, message);
        if (!error) {
            logToBoth("Relaying control request to Gateway: " + String(message));
            serializeJson(doc, swSerial);
            swSerial.println();
            
            // Check if this is an OTA command for the Transmitter itself
            if (doc.containsKey("device") && strcmp(doc["device"], "transmitter") == 0) {
               if (doc.containsKey("ota") && strcmp(doc["ota"], "on") == 0) {
                   logToBoth("Transmitter OTA requested via MQTT");
                   ArduinoOTA.begin();
                   
                   // Publish OTA status immediately
                   StaticJsonDocument<128> statusDoc;
                   statusDoc["connection"] = WiFi.localIP().toString();
                   statusDoc["status"] = "ota";
                   char buffer[128];
                   serializeJson(statusDoc, buffer);
                   client.publish("espnow/transmitter/status", buffer);
               } else if (doc.containsKey("ota") && strcmp(doc["ota"], "off") == 0) {
                   logToBoth("Transmitter OTA OFF requested via MQTT");
                   // Publish online status
                   StaticJsonDocument<128> statusDoc;
                   statusDoc["connection"] = WiFi.localIP().toString();
                   statusDoc["status"] = "online";
                   char buffer[128];
                   serializeJson(statusDoc, buffer);
                   client.publish("espnow/transmitter/status", buffer);
               }
            }
        } else {
            logToBoth("JSON parse error on control topic: " + String(error.c_str()));
        }
    }
}

void reconnect() {
    static unsigned long lastReconnectAttempt = 0;
    unsigned long now = millis();
    
    // Only attempt reconnect every 5 seconds
    if (now - lastReconnectAttempt < 5000) return;
    lastReconnectAttempt = now;

    logToBoth("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect with credentials
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        logToBoth("connected");
        // Subscribe to control topics
        client.subscribe("espnow/control");
        logToBoth("Subscribed to espnow/control");
        
        // Publish online status
        StaticJsonDocument<128> doc;
        doc["connection"] = WiFi.localIP().toString();
        doc["status"] = "online"; 
        char buffer[128];
        serializeJson(doc, buffer);
        client.publish("espnow/transmitter/status", buffer);
        logToBoth("Published status: " + String(buffer));
    } else {
        logToBoth("failed, rc=" + String(client.state()) + " will try again in 5s");
    }
}

void setup() {
    Serial.begin(115200);
    
    // Software Serial to talk to Gateway
    // swSerial.begin(baud, config, rxPin, txPin, invert, bufferSize)
    delay(5000);
    swSerial.begin(9600, SWSERIAL_8N1, D6, D5, false, 512);
    
    // Load saved MQTT credentials
    loadConfig();
    
    // WiFiManager with custom parameters
    WiFiManagerParameter custom_mqtt_user("mqttuser", "MQTT User", mqtt_user, 40);
    WiFiManagerParameter custom_mqtt_pass("mqttpass", "MQTT Password", mqtt_pass, 40);
    
    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);
    
    // Optional: Uncomment to reset settings for testing
    // wifiManager.resetSettings();
    
    // Force config portal if MQTT credentials are missing
    bool forceConfig = (strlen(mqtt_user) == 0 || strlen(mqtt_pass) == 0);
    
    if (forceConfig) {
        Serial.println("MQTT credentials missing - starting config portal...");
        if (!wifiManager.startConfigPortal("Transmitter_AP")) {
            Serial.println("failed to connect and hit timeout");
            delay(3000);
            ESP.restart();
            delay(5000);
        }
    } else if (!wifiManager.autoConnect("Transmitter_AP")) {
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        //reset and try again, or maybe put it to deep sleep
        ESP.restart();
        delay(5000);
    }

    Serial.println("WiFi connected... :)");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Read updated parameters
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    
    // Save config if needed
    if (shouldSaveConfig) {
      saveConfig();
    }

    client.setServer(mqtt_server, 1883);
    client.setCallback(mqtt_callback);
    client.setBufferSize(2048); 

    // Initialize OTA
    ArduinoOTA.setPort(8266);
    ArduinoOTA.setHostname("ESPNOW_Transmitter");
    
    ArduinoOTA.onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else { // U_FS
        type = "filesystem";
      }
      Serial.println("Start updating " + type);
      isOTAUpdating = true;
      
      // Publish OTA status
      if (client.connected()) {
        StaticJsonDocument<128> doc;
        doc["connection"] = WiFi.localIP().toString();
        doc["status"] = "ota";
        char buffer[128];
        serializeJson(doc, buffer);
        client.publish("espnow/transmitter/status", buffer);
      }
    });
    
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
      isOTAUpdating = false;
      
      // Publish online status
      if (client.connected()) {
        StaticJsonDocument<128> doc;
        doc["connection"] = WiFi.localIP().toString();
        doc["status"] = "online";
        char buffer[128];
        serializeJson(doc, buffer);
        client.publish("espnow/transmitter/status", buffer);
      }
    });

    ArduinoOTA.setHostname("espnow-transmitter");
    
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

    ArduinoOTA.begin();

    // Initialize Telnet
    telnetServer.begin();
    telnetServer.setNoDelay(true);

    Serial.println("\nTransmitter starting...");
    Serial.printf("IP: %s, OTA and Telnet (Port 23) Ready.\n", WiFi.localIP().toString().c_str());
    Serial.print("MQTT User: "); Serial.println(mqtt_user);
    Serial.println("Waiting for data from Gateway on D6(RX)/D5(TX)...");
}

// Function to publish MQTT auto-discovery message for a device (using MAC for state topic)
void publishDiscoveryWithMac(const JsonVariantConst& config, const char* macAddress) {
    const char* deviceName = config["deviceName"];
    if (deviceName == nullptr) return;

    // Check if already discovered
    if (discoveredDevices.find(deviceName) != discoveredDevices.end()) {
        Serial.printf("Device %s already discovered, skipping\n", deviceName);
        return;
    }
    
    int deviceType = config["deviceType"] | 0;
    bool isBatteryPowered = config["isBatteryPowered"] | true;
    
    // Calculate dynamic expire_after
    int sleepInterval = config["sleepInterval"] | 15; 
    int expireAfter;
    if (isBatteryPowered) {
        // Allow for ~3 missed intervals before marking unavailable + buffer
        expireAfter = (sleepInterval * 3) + 20;
    } else {
        // Continuous mode: send data every 2s, so expire after 10s
        expireAfter = 10; 
    }
    
    String discoveryTopic = String("homeassistant/device/") + deviceName + "/config";
    
    // Build discovery payload with abbreviated keys
    DynamicJsonDocument doc(4096); 
    
    // Device information
    JsonObject device = doc.createNestedObject("dev");
    JsonArray identifiers = device.createNestedArray("ids");
    identifiers.add(deviceName);
    device["name"] = String(deviceName);
    device["mdl"] = "ESP-NOW Device";
    device["mf"] = "Sergey Inc.";
    
    JsonObject origin = doc.createNestedObject("o");
    origin["name"] = "ESP-NOW Gateway";
    origin["sw"] = "1.0";
    
    doc["stat_t"] = String(mqtt_topic_base) + "/" + macAddress + "/state";
    JsonObject components = doc.createNestedObject("cmps");
    
    if (isBatteryPowered) {
        // Battery sensor
        JsonObject battery = components.createNestedObject("battery");
        battery["p"] = "sensor";
        battery["uniq_id"] = String(deviceName) + "_battery";
        battery["name"] = "Battery";
        battery["dev_cla"] = "voltage";
        battery["stat_cla"] = "measurement";
        battery["unit_of_meas"] = "V";
        battery["val_tpl"] = "{{ value_json.batteryVoltage | round(2) }}";
        battery["exp_aft"] = expireAfter;
    }

    if (deviceType == DEV_PLANT || deviceType == DEV_ENVIRO_MOTION) {
        // Temperature sensor
        JsonObject temp = components.createNestedObject("temperature");
        temp["p"] = "sensor";
        temp["uniq_id"] = String(deviceName) + "_temperature";
        temp["name"] = "Temperature";
        temp["dev_cla"] = "temperature";
        temp["stat_cla"] = "measurement";
        temp["unit_of_meas"] = "\u00b0C";
        temp["val_tpl"] = "{{ value_json.temperature | round(1) }}";
        temp["exp_aft"] = expireAfter;
        
        // Humidity sensor
        JsonObject hum = components.createNestedObject("humidity");
        hum["p"] = "sensor";
        hum["uniq_id"] = String(deviceName) + "_humidity";
        hum["name"] = "Humidity";
        hum["dev_cla"] = "humidity";
        hum["stat_cla"] = "measurement";
        hum["unit_of_meas"] = "%";
        hum["val_tpl"] = "{{ value_json.humidity | round(1) }}";
        hum["exp_aft"] = expireAfter;
        
        // Light sensor
        JsonObject light = components.createNestedObject("light");
        light["p"] = "sensor";
        light["uniq_id"] = String(deviceName) + "_light";
        light["name"] = "Light";
        light["dev_cla"] = "illuminance";
        light["stat_cla"] = "measurement";
        light["unit_of_meas"] = "lx";
        light["val_tpl"] = "{{ value_json.lux | round(0) }}";
        light["exp_aft"] = expireAfter;
    }

    if (deviceType == DEV_PLANT) {
        // Soil Moisture sensor
        JsonObject soil = components.createNestedObject("soil");
        soil["p"] = "sensor";
        soil["uniq_id"] = String(deviceName) + "_soil";
        soil["name"] = "Soil Moisture";
        soil["stat_cla"] = "measurement";
        soil["unit_of_meas"] = "%";
        soil["val_tpl"] = "{{ value_json.soilMoisture | round(1) }}";
        soil["exp_aft"] = expireAfter;
    }

    if (deviceType == DEV_ENVIRO_MOTION) {
        // Motion sensor
        JsonObject motion = components.createNestedObject("motion");
        motion["p"] = "binary_sensor";
        motion["uniq_id"] = String(deviceName) + "_motion";
        motion["name"] = "Motion";
        motion["dev_cla"] = "motion";
        motion["val_tpl"] = "{{ 'ON' if value_json.motionDetected else 'OFF' }}";
        motion["exp_aft"] = expireAfter;

        // Distance sensor
        JsonObject dist = components.createNestedObject("distance");
        dist["p"] = "sensor";
        dist["uniq_id"] = String(deviceName) + "_distance";
        dist["name"] = "Distance";
        dist["stat_cla"] = "measurement";
        dist["unit_of_meas"] = "cm";
        dist["val_tpl"] = "{{ value_json.distance | round(1) }}";
        dist["exp_aft"] = expireAfter;
    }
    
    if (deviceType == DEV_BINARY) {
        // Binary sensor (e.g. Door)
        JsonObject binary = components.createNestedObject("door");
        binary["p"] = "binary_sensor";
        binary["uniq_id"] = String(deviceName) + "_door";
        binary["name"] = "Door";
        binary["dev_cla"] = "door";
        binary["val_tpl"] = "{{ 'ON' if value_json.binaryState else 'OFF' }}";
        binary["exp_aft"] = expireAfter;
    }
    
    // Serialize and publish
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("Publishing discovery for device: %s (MAC: %s)\n", deviceName, macAddress);
    Serial.printf("Discovery topic: %s\n", discoveryTopic.c_str());
    Serial.printf("Payload size: %d bytes\n", payload.length());
    
    if (client.publish(discoveryTopic.c_str(), payload.c_str(), true)) { // retained = true
        Serial.println("Discovery message published successfully");
        discoveredDevices[deviceName] = true;
    } else {
        Serial.println("Failed to publish discovery message");
        Serial.printf("MQTT buffer size: %d\n", client.getBufferSize());
    }
}

String inputBuffer = "";


void loop() {
    ArduinoOTA.handle();
    if (isOTAUpdating) return; // Give full priority to OTA

    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    // Handle Telnet Clients
    if (telnetServer.hasClient()) {
        WiFiClient newClient = telnetServer.accept();
        if (!telnetClient || !telnetClient.connected()) {
            if (telnetClient) telnetClient.stop();
            telnetClient = newClient;
            telnetClient.println("\n--- Connected to ESP-NOW Transmitter Telnet Log ---");
        } else {
            newClient.stop(); // Reject additional clients
        }
    }

    // Capture Serial output and send to Telnet
    // This is a basic bridge. In a full implementation, we'd wrap Serial or use a buffer.
    // For now, we'll manually print important messages to telnetClient as well.

    // Read from Software Serial
    while (swSerial.available()) {
        char c = swSerial.read();
        if (c == '\n') {
            // End of message
            if (inputBuffer.length() > 0) {
                logToBoth("Received from Gateway: " + inputBuffer);

                // Parse JSON to extract MAC address and type
                DynamicJsonDocument doc(512);
                DeserializationError error = deserializeJson(doc, inputBuffer);
                
                if (!error) {
                    const char* macAddress = doc["mac"];
                    const char* deviceName = doc["deviceName"];
                    const char* type = doc["type"];
                    
                    if (doc.containsKey("device") && strcmp(doc["device"], "gateway") == 0) {
                        // Gateway Status Message
                        String topic = String(mqtt_topic_base) + "/gateway/status";
                        
                        // Create clean payload without "device" field
                        StaticJsonDocument<128> cleanDoc;
                        cleanDoc["connection"] = doc["connection"];
                        cleanDoc["status"] = doc["status"];
                        char cleanBuffer[128];
                        serializeJson(cleanDoc, cleanBuffer);
                        
                        if (client.publish(topic.c_str(), cleanBuffer)) {
                             logToBoth("Published Gateway status to: " + topic);
                        } else {
                             logToBoth("Failed to publish Gateway status");
                        }
                    } else if (macAddress != nullptr && strlen(macAddress) > 0) {
                        // Standard Sensor Message
                        // Check if this is a CONFIG message - trigger discovery
                        if (type != nullptr && strcmp(type, "CONFIG") == 0 && deviceName != nullptr) {
                            logToBoth("CONFIG message received for device: " + String(deviceName) + " (MAC: " + String(macAddress) + ")");
                            
                            // Update discovery message with actual MAC for state topic
                            publishDiscoveryWithMac(doc, macAddress);
                        }
                        
                        // Construct MAC-based topic: espnow/{macAddress}/state
                        String topic = String(mqtt_topic_base) + "/" + String(macAddress) + "/state";
                        
                        // Publish to MAC-based topic
                        if (client.publish(topic.c_str(), inputBuffer.c_str())) {
                            logToBoth("Published to MQTT topic: " + topic);
                        } else {
                            logToBoth("Failed to publish to MQTT");
                        }
                    } else {
                        logToBoth("Error: MAC address not found in JSON (and not gateway status), skipping MQTT publish");
                    }
                } else {
                    logToBoth("JSON parse error: " + String(error.c_str()));
                }
                
                inputBuffer = "";
            }
        } else if (c != '\r') {
            inputBuffer += c;
        }
    }

    // Read from Hardware Serial and output to Software Serial (for future expansion)
    if (Serial.available()) {
        while (Serial.available()) {
            char c = Serial.read();
            swSerial.write(c);
        }
    }
}
