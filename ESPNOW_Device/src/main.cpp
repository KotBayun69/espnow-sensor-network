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

// Device configuration defined in platformio.ini
// #define DEVICE_TYPE defined by build flag
// #define IS_BATTERY_POWERED defined by build flag
// Save state in RTC memory so it survives deep sleep
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool isRegistered = false;

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
  
  if (error) return;

  bool isTarget = false;
  if (doc.containsKey("device")) {
    const char* devName = doc["device"];
    if (strcmp(devName, DEVICE_NAME) == 0) {
      isTarget = true;
    }
  } else {
    // If we are in OTA mode and receive an OTA command without device name, 
    // assume it might be for us (broadcast or direct to our IP)
    if (otaMode && doc.containsKey("ota")) {
      isTarget = true;
    }
  }
    
  if (isTarget) {
    // Check for ota command
    if (doc.containsKey("ota")) {
      const char* otaCmd = doc["ota"];
      if (strcmp(otaCmd, "off") == 0) {
        logToBoth("MQTT: OTA mode OFF - publishing status and returning to normal...");
        
        // Publish status
        StaticJsonDocument<128> statusDoc;
        statusDoc["connection"] = "espnow";
        statusDoc["status"] = "online";
        char buffer[128];
        serializeJson(statusDoc, buffer);
        String statusTopic = "espnow/" + WiFi.macAddress() + "/status";
        mqttClient.publish(statusTopic.c_str(), buffer);
        
        delay(500); 
        mqttClient.disconnect();
        delay(100);
        
        if (IS_BATTERY_POWERED) {
          logToBoth("Entering deep sleep...");
          esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
          esp_deep_sleep_start();
        } else {
          logToBoth("Restarting...");
          ESP.restart();
        }
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


void enterOtaMode() {
    logToBoth("Entering OTA Mode...");
    otaMode = true;
    clearOtaRequest(); // Reset the flag once we've entered the mode
    
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
      if (wm.autoConnect("ESP-NOW-DEVICE-OTA")) {
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
      if (wm.startConfigPortal("ESP-NOW-DEVICE-OTA")) {
        wifiConnected = true;
        logToBoth("✓ WiFi connected via portal! IP: " + WiFi.localIP().toString());
      } else {
        logToBoth("✗ Portal also failed. Entering deep sleep.");
        otaMode = false;
        return;
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
        if (wm.startConfigPortal("ESP-NOW-DEVICE-OTA")) {
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
      return;
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

bool ensureRegistered() {
  if (otaMode) return true;

  if (!isRegistered || bootCount % 100 == 0) {
    Serial.println(isRegistered ? "--- Refreshing Registration ---" : "--- Device Registration Required ---");
    
    ConfigMessage configMsg;
    memset(&configMsg, 0, sizeof(configMsg));
    configMsg.type = MSG_CONFIG;
    configMsg.deviceType = DEVICE_TYPE;
    configMsg.isBatteryPowered = IS_BATTERY_POWERED;
    WiFi.macAddress(configMsg.macAddr);
    strcpy(configMsg.deviceName, DEVICE_NAME);
    
    #if DEVICE_TYPE == 1 // DEV_PLANT
        configMsg.config.plant.sleepInterval = TIME_TO_SLEEP;
    #elif DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
        configMsg.config.enviro.sleepInterval = TIME_TO_SLEEP;
        configMsg.config.enviro.motionTimeout = 30;
    #elif DEVICE_TYPE == 3 // DEV_BINARY
        configMsg.config.binary.sleepInterval = TIME_TO_SLEEP;
    #endif

    for (int i = 0; i < 3; i++) {
        clearAckFlag();
        Serial.printf("Sending Config (Attempt %d/3)...\n", i+1);
        sendConfigMessage(configMsg);
        
        unsigned long start = millis();
        while (millis() - start < 300 && !hasAckBeenReceived()) {
            delay(10);
        }
        
        if (hasAckBeenReceived()) {
            Serial.println("✓ Gateway Acknowledged Config");
            isRegistered = true;
            return true;
        }
        Serial.println("✗ Config ACK timeout...");
    }
    
    Serial.println("⚠ Registration failed. Will retry on next data attempt.");
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n=== DEVICE BOOT ===");
  bootCount++;
  Serial.printf("Boot #%d\n", bootCount);

  // Load MQTT config
  loadMqttConfig();

  // === LED CONTROL ===
  // ESP32-C3 Super Mini usually has Blue LED on GPIO 8 (Active LOW).
  // Red LED is usually hardware Power LED and cannot be controlled via software.
  // We will ensure Blue LED is OFF.
  pinMode(8, OUTPUT);
  digitalWrite(8, HIGH); // HIGH = OFF for Active Low
  
  // Initialize hardware and transport
  initSensors(); 
  initTransport();

  // Ensure registration before sending data
  ensureRegistered();

  // Read and send data
  Serial.println("Reading sensors...");
  SensorReadings readings = readSensors();
  
  #if DEVICE_TYPE == 1 // DEV_PLANT
      Serial.printf("Plant: T=%.2f°C, H=%.2f%%, Lux=%.2f, Moist=%.2f%%, Batt=%.2fV\n",
                    readings.data.plant.temperature, readings.data.plant.humidity, 
                    readings.data.plant.lux, readings.data.plant.soilMoisture, readings.batteryVoltage);
  #elif DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
      Serial.printf("Enviro: T=%.2f°C, Motion=%d, Batt=%.2fV\n",
                    readings.data.enviro.temperature, readings.data.enviro.motionDetected,
                    readings.batteryVoltage);
  #elif DEVICE_TYPE == 3 // DEV_BINARY
      Serial.printf("Binary: State=%d, Batt=%.2fV\n",
                    readings.data.binary.state, readings.batteryVoltage);
  #endif

  DataMessage dataMsg;
  memset(&dataMsg, 0, sizeof(dataMsg));
  dataMsg.type = MSG_DATA;
  dataMsg.deviceType = DEVICE_TYPE;
  dataMsg.batteryVoltage = readings.batteryVoltage;
  
  #if DEVICE_TYPE == 1 // DEV_PLANT
      dataMsg.data.plant = readings.data.plant;
  #elif DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
      dataMsg.data.enviro = readings.data.enviro;
  #elif DEVICE_TYPE == 3 // DEV_BINARY
      dataMsg.data.binary = readings.data.binary;
  #endif

  if (isRegistered) {
      clearAckFlag();
      sendDataMessage(dataMsg);
      
      unsigned long startData = millis();
      while (millis() - startData < 500 && !hasAckBeenReceived()) {
          delay(10);
      }

      if (hasAckBeenReceived()) {
          Serial.println("✓ Data Acknowledged");
      } else {
          Serial.println("✗ Data ACK Timeout! Resetting registration.");
          isRegistered = false; 
      }
  } else {
      Serial.println("! Skipping data send: Device not registered");
  }
  
  delay(100); 

  // Check if OTA was requested
  if (isOtaRequested()) {
    enterOtaMode();
  }

  // Sleep or Continuous checks
  if (!otaMode && IS_BATTERY_POWERED) {
    // Go to sleep
    Serial.printf("Going to sleep for %d seconds...\n", TIME_TO_SLEEP);
    Serial.flush();
    
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  } else if (!otaMode && !IS_BATTERY_POWERED) {
    Serial.println("Running in continuous mode (not battery powered)");
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
  
  #if !IS_BATTERY_POWERED
  else {
      // Logic for continuous sensors (e.g., Motion + Enviro)
      // Only applicable if not battery powered
      
      static unsigned long lastSent = 0;
      static unsigned long lastEnviroRead = 0;
      static bool firstRun = true;
      
      // Last known values
      static bool lastMotionState = false;
      static float lastLux = 0.0;
      static float lastTemp = 0.0;
      static float lastHum = 0.0;
      static float lastPressure = 0.0;
      
      // Intervals
      const unsigned long ENVIRO_INTERVAL = 60000; // 60s for Light/Temp
      
      bool forceUpdate = isUpdateRequested();
      if (forceUpdate) {
          clearUpdateRequest();
          Serial.println("Force Update Triggered!");
      }
      
      bool readingEnviro = (millis() - lastEnviroRead > ENVIRO_INTERVAL) || firstRun || forceUpdate;

      // Read Sensos (pass readingEnviro flag to avoid I2C/Blink if not needed)
      SensorReadings readings = readSensors(readingEnviro);
      
      bool triggerSend = false;
      String triggerReason = "Heartbeat";

      #if DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
          bool currMotion = readings.data.enviro.motionDetected;
          
          // Update cached values if we actually read them
          if (readingEnviro) {
              lastLux = readings.data.enviro.lux;
              lastTemp = readings.data.enviro.temperature;
              lastHum = readings.data.enviro.humidity;
              lastPressure = readings.data.enviro.pressure;
              lastEnviroRead = millis();
              
              // Always send on 60s interval (Heartbeat + Enviro Data)
              triggerSend = true;
              triggerReason = forceUpdate ? "Force Update" : "Timer (60s)";
          } else {
              // Fill struct with cached values so we don't send 0s on motion event
              readings.data.enviro.lux = lastLux;
              readings.data.enviro.temperature = lastTemp;
              readings.data.enviro.humidity = lastHum;
              readings.data.enviro.pressure = lastPressure;
          }

          // Check Motion Change
          if (currMotion != lastMotionState) {
              triggerSend = true;
              triggerReason = "Motion Change";
          }
      #endif

      if (triggerSend) { 
          if (ensureRegistered()) {
              
              DataMessage dataMsg;
              memset(&dataMsg, 0, sizeof(dataMsg));
              dataMsg.type = MSG_DATA;
              dataMsg.deviceType = DEVICE_TYPE;
              dataMsg.batteryVoltage = 0.0;
              
              #if DEVICE_TYPE == 2 // DEV_ENVIRO_MOTION
                  dataMsg.data.enviro = readings.data.enviro;
                  
                  // Update last motion state
                  lastMotionState = readings.data.enviro.motionDetected;
              #endif
              
              clearAckFlag();
              sendDataMessage(dataMsg);
              
              lastSent = millis();
              firstRun = false;
              
              unsigned long startData = millis();
              while (millis() - startData < 500 && !hasAckBeenReceived()) {
                  delay(10);
              }

              if (hasAckBeenReceived()) {
                  Serial.printf("✓ Data Sent (%s)\n", triggerReason.c_str());
              } else {
                  Serial.println("✗ Data ACK timeout. Force re-registration.");
                  isRegistered = false;
              }
          }
      }
          
      // Check if OTA was requested in the response
      if (isOtaRequested()) {
          enterOtaMode();
      }
      delay(10);
  }
  #endif
}
