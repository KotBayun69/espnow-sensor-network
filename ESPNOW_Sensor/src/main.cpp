#include "sensors.h"
#include "transport.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  15          /* Time ESP32 will go to sleep (in seconds) */

// Save state in RTC memory so it survives deep sleep
RTC_DATA_ATTR int bootCount = 0;

bool otaMode = false;
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool isOTAUpdating = false;

// MQTT Configuration
WiFiClient espClient;
PubSubClient mqttClient(espClient);
char mqtt_server[40] = "192.168.1.1";
char mqtt_port[6] = "1883";
char mqtt_user[40] = "";
char mqtt_pass[40] = "";

bool shouldSaveConfig = false;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void logToBoth(const String& msg, bool newline = true) {
  Serial.print(msg);
  if (newline) Serial.println();
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(msg);
    if (newline) telnetClient.println();
  }
}

void loadMqttConfig() {
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed! Formatting...");
    if (LittleFS.begin(true)) { // true = format on fail
      Serial.println("LittleFS formatted successfully");
    } else {
      Serial.println("LittleFS format failed!");
      return;
    }
  }
  
  if (LittleFS.exists("/mqtt_config.json")) {
    File configFile = LittleFS.open("/mqtt_config.json", "r");
    if (configFile) {
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, configFile);
      if (!error) {
        strlcpy(mqtt_server, doc["server"] | "192.168.1.1", sizeof(mqtt_server));
        strlcpy(mqtt_port, doc["port"] | "1883", sizeof(mqtt_port));
        strlcpy(mqtt_user, doc["user"] | "", sizeof(mqtt_user));
        strlcpy(mqtt_pass, doc["pass"] | "", sizeof(mqtt_pass));
        Serial.println("MQTT config loaded from LittleFS");
      }
      configFile.close();
    }
  } else {
    Serial.println("No MQTT config file found");
  }
}

void saveMqttConfig() {
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS not available, cannot save config");
    return;
  }
  
  StaticJsonDocument<256> doc;
  doc["server"] = mqtt_server;
  doc["port"] = mqtt_port;
  doc["user"] = mqtt_user;
  doc["pass"] = mqtt_pass;
  
  File configFile = LittleFS.open("/mqtt_config.json", "w");
  if (configFile) {
    serializeJson(doc, configFile);
    configFile.close();
    Serial.println("MQTT config saved to LittleFS");
  } else {
    Serial.println("Failed to open config file for writing");
  }
}


void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  
  if (!error && doc.containsKey("device")) {
    const char* devName = doc["device"];
    
    // Check if command is for this device
    if (strcmp(devName, DEVICE_NAME) == 0) {
      // Check for ota command
      if (doc.containsKey("ota")) {
        const char* otaCmd = doc["ota"];
        if (strcmp(otaCmd, "off") == 0) {
          logToBoth("MQTT: OTA mode OFF - publishing status and entering deep sleep");
          
          // Publish online status before leaving
          StaticJsonDocument<128> statusDoc;
          statusDoc["connection"] = "espnow";
          statusDoc["status"] = "online";
          char buffer[128];
          serializeJson(statusDoc, buffer);
          
          String statusTopic = "espnow/" + WiFi.macAddress() + "/status";
          mqttClient.publish(statusTopic.c_str(), buffer);
          
          // Give time for message to send
          delay(500); 
          mqttClient.disconnect();
          delay(100);
          
          // Go directly to deep sleep
          esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
          esp_deep_sleep_start();
        }
      }
      
      // Check for restart command
      if (doc.containsKey("restart")) {
        const char* restartCmd = doc["restart"];
        if (strcmp(restartCmd, "on") == 0) {
          logToBoth("MQTT: Restart requested");
          delay(100);
          ESP.restart();
        }
      }
    }
  }
}

void mqttReconnect() {
  if (!mqttClient.connected()) {
    logToBoth("Attempting MQTT connection...");
    logToBoth("  Server: " + String(mqtt_server) + ":" + String(mqtt_port));
    logToBoth("  User: " + String(mqtt_user));

    
    int port = atoi(mqtt_port);
    mqttClient.setServer(mqtt_server, port);
    mqttClient.setKeepAlive(15);
    mqttClient.setSocketTimeout(5);
    
    String clientId = "ESP32-" + String(DEVICE_NAME);
    
    bool connected = false;
    if (strlen(mqtt_user) > 0) {
      logToBoth("  Connecting with credentials...");
      connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
    } else {
      logToBoth("  Connecting without credentials...");
      connected = mqttClient.connect(clientId.c_str());
    }
    
    if (connected) {
      logToBoth("✓ MQTT connected!");
      mqttClient.subscribe("espnow/control");
      logToBoth("✓ Subscribed to espnow/control");
      
      // Publish status
      StaticJsonDocument<128> statusDoc;
      statusDoc["connection"] = WiFi.localIP().toString();
      statusDoc["status"] = "ota";
      char buffer[128];
      serializeJson(statusDoc, buffer);
      
      String statusTopic = "espnow/" + WiFi.macAddress() + "/status";
      mqttClient.publish(statusTopic.c_str(), buffer);
      logToBoth("✓ Published status to " + statusTopic + ": " + String(buffer));
    } else {
      int state = mqttClient.state();
      String error = "✗ MQTT connection failed, rc=";
      error += state;
      error += " (";
      switch(state) {
        case -4: error += "TIMEOUT"; break;
        case -3: error += "CONNECTION_LOST"; break;
        case -2: error += "CONNECT_FAILED"; break;
        case -1: error += "DISCONNECTED"; break;
        case 1: error += "BAD_PROTOCOL"; break;
        case 2: error += "BAD_CLIENT_ID"; break;
        case 3: error += "UNAVAILABLE"; break;
        case 4: error += "BAD_CREDENTIALS"; break;
        case 5: error += "UNAUTHORIZED"; break;
        default: error += "UNKNOWN"; break;
      }
      error += ")";
      logToBoth(error);
      
      if (state == 4) {
        logToBoth("  Check MQTT username and password!");
      }
    }
  }
}


void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=== SENSOR BOOT ===");
  bootCount++;
  Serial.printf("Boot #%d\n", bootCount);

  // Load MQTT config
  loadMqttConfig();

  // Initialize
  initSensors();
  initTransport();

  // Send Config on first boot
  if (bootCount == 1) {
    Serial.println("First boot - sending Config...");
    ConfigMessage configMsg;
    configMsg.type = MSG_CONFIG;
    WiFi.macAddress(configMsg.macAddr);
    strcpy(configMsg.deviceName, DEVICE_NAME);
    
    SensorReadings readings = readSensors();
    configMsg.hasBME = readings.validBME;
    configMsg.hasBH1750 = readings.validBH1750;
    configMsg.hasBattery = true;
    configMsg.hasBinary = readings.validBinary;
    configMsg.hasAnalog = readings.validAnalog;
    configMsg.sleepInterval = TIME_TO_SLEEP;

    sendConfigMessage(configMsg);
    delay(100); // Wait for send callback
  }

  // Read and send data
  Serial.println("Reading sensors...");
  SensorReadings readings = readSensors();
  Serial.printf("T=%.2f°C, H=%.2f%%, P=%.2f hPa, Lux=%.2f, Batt=%.2fV\n",
                readings.temperature, readings.humidity, readings.pressure, 
                readings.lux, readings.batteryVoltage);

  DataMessage dataMsg;
  dataMsg.type = MSG_DATA;
  dataMsg.temperature = readings.temperature;
  dataMsg.humidity = readings.humidity;
  dataMsg.pressure = readings.pressure;
  dataMsg.lux = readings.lux;
  dataMsg.batteryVoltage = readings.batteryVoltage;
  dataMsg.binaryState = readings.binaryState;
  dataMsg.analogValue = readings.analogValue;

  sendDataMessage(dataMsg);
  delay(500); // Wait for send callback and potential CMD response

  // Check if OTA was requested
  if (isOtaRequested()) {
    logToBoth("Entering OTA Mode...");
    otaMode = true;
    
    // WiFiManager with custom MQTT parameters
    WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
    WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
    WiFiManagerParameter custom_mqtt_user("user", "MQTT User", mqtt_user, 40);
    WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Pass", mqtt_pass, 40);
    
    WiFiManager wm;
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setDebugOutput(false);
    wm.addParameter(&custom_mqtt_server);
    wm.addParameter(&custom_mqtt_port);
    wm.addParameter(&custom_mqtt_user);
    wm.addParameter(&custom_mqtt_pass);

    bool wifiConnected = false;
    bool mqttConnected = false;

    // === WIFI: Try 3 times ===
    for (int attempt = 1; attempt <= 3 && !wifiConnected; attempt++) {
      logToBoth("WiFi connection attempt " + String(attempt) + "/3...");
      if (wm.autoConnect("ESP-NOW-SENSOR-OTA")) {
        wifiConnected = true;
        logToBoth("✓ WiFi connected! IP: " + WiFi.localIP().toString());
      } else {
        logToBoth("✗ WiFi attempt " + String(attempt) + " failed");
        delay(2000);
      }
    }

    if (!wifiConnected) {
      logToBoth("✗ WiFi failed after 3 attempts. Clearing config...");
      if (LittleFS.exists("/mqtt_config.json")) {
        LittleFS.remove("/mqtt_config.json");
      }
      
      logToBoth("Starting config portal...");
      if (wm.startConfigPortal("ESP-NOW-SENSOR-OTA")) {
        wifiConnected = true;
        logToBoth("✓ WiFi connected via portal! IP: " + WiFi.localIP().toString());
      } else {
        logToBoth("✗ Portal also failed. Entering deep sleep.");
        otaMode = false;
      }
    }

    // === MQTT: If WiFi succeeded, try 3 times ===
    if (wifiConnected && otaMode) {
      // Read updated parameters
      strcpy(mqtt_server, custom_mqtt_server.getValue());
      strcpy(mqtt_port, custom_mqtt_port.getValue());
      strcpy(mqtt_user, custom_mqtt_user.getValue());
      strcpy(mqtt_pass, custom_mqtt_pass.getValue());
      
      // Save config if needed
      if (shouldSaveConfig) {
        saveMqttConfig();
        logToBoth("✓ MQTT config saved");
      }

      // Setup MQTT
      mqttClient.setCallback(mqttCallback);
      
      for (int attempt = 1; attempt <= 3 && !mqttConnected; attempt++) {
        logToBoth("MQTT connection attempt " + String(attempt) + "/3...");
        mqttReconnect();
        delay(1000); // Give it time
        
        if (mqttClient.connected()) {
          mqttConnected = true;
          logToBoth("✓ MQTT connected!");
        } else {
          logToBoth("✗ MQTT attempt " + String(attempt) + " failed");
          delay(2000);
        }
      }

      if (!mqttConnected) {
        logToBoth("✗ MQTT failed after 3 attempts. Clearing config...");
        if (LittleFS.exists("/mqtt_config.json")) {
          LittleFS.remove("/mqtt_config.json");
        }
        
        logToBoth("Restarting config portal for new credentials...");
        if (wm.startConfigPortal("ESP-NOW-SENSOR-OTA")) {
          // Read new parameters
          strcpy(mqtt_server, custom_mqtt_server.getValue());
          strcpy(mqtt_port, custom_mqtt_port.getValue());
          strcpy(mqtt_user, custom_mqtt_user.getValue());
          strcpy(mqtt_pass, custom_mqtt_pass.getValue());
          saveMqttConfig();
          
          // Try MQTT one more time with new config
          mqttReconnect();
          delay(1000);
          if (mqttClient.connected()) {
            mqttConnected = true;
            logToBoth("✓ MQTT connected with new config!");
          }
        }
      }
    }

    if (!mqttConnected && otaMode) {
      logToBoth("✗ Cannot establish MQTT. Entering deep sleep.");
      otaMode = false;
    }

    if (otaMode) {

      
      ArduinoOTA.setHostname(DEVICE_NAME);
      ArduinoOTA.onStart([]() {
        isOTAUpdating = true;
        Serial.println("OTA Update Starting...");
      });
      ArduinoOTA.onEnd([]() {
        isOTAUpdating = false;
        Serial.println("OTA Update Complete!");
      });
      ArduinoOTA.onError([](ota_error_t error) {
        isOTAUpdating = false;
        Serial.printf("OTA Error[%u]\n", error);
      });
      
      ArduinoOTA.begin();
      telnetServer.begin();
      logToBoth("OTA Ready. Telnet Ready. MQTT subscribed to espnow/control");
    }
  }

  if (!otaMode) {
    // Go to sleep
    Serial.printf("Going to sleep for %d seconds...\n", TIME_TO_SLEEP);
    Serial.flush();
    
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
}

void loop() {
  if (otaMode) {
    static int mqttFailCount = 0;
    static unsigned long lastMqttAttempt = 0;
    
    // Handle MQTT
    if (!mqttClient.connected()) {
      unsigned long now = millis();
      // Try reconnecting every 10 seconds
      if (now - lastMqttAttempt > 10000) {
        lastMqttAttempt = now;
        
        int prevState = mqttClient.state();
        mqttReconnect();
        
        if (!mqttClient.connected()) {
          mqttFailCount++;
          logToBoth("MQTT connection failed (" + String(mqttFailCount) + "/3)");
          
          if (mqttFailCount >= 3) {
            logToBoth("Too many MQTT failures. Clearing config and restarting...");
            // Delete config file
            if (LittleFS.exists("/mqtt_config.json")) {
              LittleFS.remove("/mqtt_config.json");
            }
            delay(1000);
            ESP.restart();
          }
        } else {
          mqttFailCount = 0; // Reset on success
        }
      }
    }
    mqttClient.loop();

    
    ArduinoOTA.handle();
    if (isOTAUpdating) return;

    // Handle Telnet
    if (telnetServer.hasClient()) {
      WiFiClient newClient = telnetServer.accept();
      if (!telnetClient || !telnetClient.connected()) {
        if (telnetClient) telnetClient.stop();
        telnetClient = newClient;
        telnetClient.println("\n--- Connected to " + String(DEVICE_NAME) + " Telnet Log ---");
      } else {
        newClient.stop();
      }
    }
    delay(10);
  }
}
