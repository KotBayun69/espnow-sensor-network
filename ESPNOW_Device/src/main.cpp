#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "sensors.h"
#include "transport.h"
#include "CommonUtils.h"
#include "protocol.h"

#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  15          /* Time ESP32 will go to sleep (in seconds) */

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool isRegistered = false;

bool otaMode = false;
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool isOTAUpdating = false;

// Global log helper
void log(const String& msg, bool newline = true) {
    logToBoth(msg, newline, telnetClient);
}

MqttConfig mqtt_cfg;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
bool shouldSaveConfig = false;

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void loadMqttConfig() {
  if (!LittleFS.begin(false)) {
    if (!LittleFS.begin(true)) return;
  }
  loadBaseConfig(mqtt_cfg, "/mqtt_config.json");
}

void saveMqttConfig() {
  saveBaseConfig(mqtt_cfg, "/mqtt_config.json");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Handle Calibration Topic (Raw String Payload)
  if (String(topic).endsWith("/calibrate")) {
      String payloadStr = "";
      for (unsigned int i=0; i<length; i++) payloadStr += (char)payload[i];
      
      StaticJsonDocument<128> statusDoc;
      char buffer[128];

      if (payloadStr == "dry") {
          log("Calibrating DRY...");
          statusDoc["status"] = "calibrating dry";
          serializeJson(statusDoc, buffer);
          mqttClient.publish(("espnow/" + slugify(DEVICE_NAME) + "/status").c_str(), buffer);
          
          calibrateSoil(false);
          
          statusDoc["status"] = "done";
          serializeJson(statusDoc, buffer);
          mqttClient.publish(("espnow/" + slugify(DEVICE_NAME) + "/status").c_str(), buffer);
          log("Calibration DRY Done.");

      } else if (payloadStr == "wet") {
          log("Calibrating WET...");
          statusDoc["status"] = "calibrating wet";
          serializeJson(statusDoc, buffer);
          mqttClient.publish(("espnow/" + slugify(DEVICE_NAME) + "/status").c_str(), buffer);

          calibrateSoil(true);

          statusDoc["status"] = "done";
          serializeJson(statusDoc, buffer);
          mqttClient.publish(("espnow/" + slugify(DEVICE_NAME) + "/status").c_str(), buffer);
          log("Calibration WET Done.");
      }
      return; // Done with calibration message
  }

  // Handle Control/Status Topics (JSON Payload)
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
      log("MQTT JSON Error: " + String(error.c_str()));
      return;
  }

  // Since we subscribe specifically, we assume it's for us.
  bool isTarget = true; 
    
  if (isTarget) {
    if (doc.containsKey("ota")) {
      String otaCmd = doc["ota"].as<String>();
      if (otaCmd == "off") {
        log("MQTT: OTA OFF - Restarting...");
        StaticJsonDocument<128> statusDoc;
        statusDoc["status"] = "calibration finished. restarting in 10 seconds";
        char buffer[128];
        serializeJson(statusDoc, buffer);
        mqttClient.publish(("espnow/" + slugify(DEVICE_NAME) + "/status").c_str(), buffer);
        
        delay(10000); 
        mqttClient.disconnect();
        ESP.restart();
      }
    }
    if (doc.containsKey("restart")) {
      String restartCmd = doc["restart"].as<String>();
      if (restartCmd == "on") {
        log("MQTT: Restart requested");
        delay(100); ESP.restart();
      }
    }
  }
}

void mqttReconnect() {
  if (!mqttClient.connected()) {
    log("Attempting MQTT connection to " + String(mqtt_cfg.server));
    mqttClient.setServer(mqtt_cfg.server, mqtt_cfg.port);
    String clientId = "ESP32-" + String(DEVICE_NAME);
    if (mqttClient.connect(clientId.c_str(), mqtt_cfg.user, mqtt_cfg.pass)) {
      log("✓ MQTT connected");
      mqttClient.subscribe(("espnow/" + slugify(DEVICE_NAME) + "/control").c_str());
      mqttClient.subscribe(("espnow/" + slugify(DEVICE_NAME) + "/calibrate").c_str());
      StaticJsonDocument<128> statusDoc;
      statusDoc["connection"] = WiFi.localIP().toString();
      statusDoc["status"] = "ota";
      char buffer[128];
      serializeJson(statusDoc, buffer);
      mqttClient.publish(("espnow/" + slugify(DEVICE_NAME) + "/status").c_str(), buffer);
    } else {
      log("✗ MQTT failed, rc=" + String(mqttClient.state()));
    }
  }
}

void enterOtaMode() {
    log("Entering OTA Mode...");
    otaMode = true;
    clearOtaRequest();
    
    char portStr[6]; itoa(mqtt_cfg.port, portStr, 10);
    WiFiManagerParameter c_server("server", "MQTT Server", mqtt_cfg.server, 40);
    WiFiManagerParameter c_port("port", "MQTT Port", portStr, 6);
    WiFiManagerParameter c_user("user", "MQTT User", mqtt_cfg.user, 40);
    WiFiManagerParameter c_pass("pass", "MQTT Pass", mqtt_cfg.pass, 40);
    
    WiFiManager wm;
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.addParameter(&c_server); wm.addParameter(&c_port);
    wm.addParameter(&c_user); wm.addParameter(&c_pass);

    wm.addParameter(&c_user); wm.addParameter(&c_pass);

    bool connected;
    if (strlen(mqtt_cfg.server) == 0) {
        log("No MQTT config. Forcing Config Portal...");
        connected = wm.startConfigPortal("ESP-NOW-DEVICE-OTA");
    } else {
        connected = wm.autoConnect("ESP-NOW-DEVICE-OTA");
    }

    if (connected) {
      strlcpy(mqtt_cfg.server, c_server.getValue(), 40);
      mqtt_cfg.port = atoi(c_port.getValue());
      strlcpy(mqtt_cfg.user, c_user.getValue(), 40);
      strlcpy(mqtt_cfg.pass, c_pass.getValue(), 40);
      if (shouldSaveConfig) saveMqttConfig();

      mqttClient.setCallback(mqttCallback);
      mqttReconnect();
    }

    if (otaMode) {
      ArduinoOTA.setHostname(DEVICE_NAME);
      ArduinoOTA.onStart([]() { isOTAUpdating = true; });
      ArduinoOTA.onEnd([]() { isOTAUpdating = false; });
      ArduinoOTA.begin();
      telnetServer.begin();
      log("OTA Ready.");
    }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  loadMqttConfig();
  pinMode(8, OUTPUT); digitalWrite(8, HIGH);
  initSensors(); 
  initTransport();

  ConfigMessage configMsg;
  memset(&configMsg, 0, sizeof(configMsg));
  configMsg.type = MSG_CONFIG;
  configMsg.sensorFlags = 0;
  #ifdef USE_BME280
      configMsg.sensorFlags |= SENSOR_FLAG_BME;
  #endif
  #ifdef USE_BH1750
      configMsg.sensorFlags |= SENSOR_FLAG_LUX;
  #endif
  #ifdef USE_SOIL_SENSOR
      configMsg.sensorFlags |= SENSOR_FLAG_SOIL;
  #endif
  #ifdef USE_BINARY_SENSOR
      configMsg.sensorFlags |= SENSOR_FLAG_BINARY;
  #endif
  
  WiFi.macAddress(configMsg.macAddr);
  strcpy(configMsg.deviceName, DEVICE_NAME);
  configMsg.sleepInterval = TIME_TO_SLEEP;

  sendConfigMessage(configMsg);
  SensorReadings readings = readSensors();
  
  DataMessage dataMsg;
  memset(&dataMsg, 0, sizeof(dataMsg));
  dataMsg.type = MSG_DATA;
  dataMsg.sensorFlags = readings.flags;
  dataMsg.bme = readings.bme;
  dataMsg.lux = readings.lux;
  dataMsg.soil = readings.soil;
  dataMsg.binary = readings.binary;

  sendDataMessage(dataMsg);
  
  unsigned long waitStart = millis();
  while (millis() - waitStart < 300) {
      if (isOtaRequested() || isConfigRequestRequested()) break;
      delay(10);
  }

  if (isConfigRequestRequested()) {
      clearConfigRequest();
      sendConfigMessage(configMsg);
      delay(100); // Give time for transmission
  }

  if (isOtaRequested()) enterOtaMode();
  else {
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
}

void loop() {
  if (otaMode) {
    if (!mqttClient.connected()) mqttReconnect();
    mqttClient.loop();
    ArduinoOTA.handle();
    if (isOTAUpdating) return;

    if (telnetServer.hasClient()) {
      WiFiClient nC = telnetServer.accept();
      if (!telnetClient || !telnetClient.connected()) {
        if (telnetClient) telnetClient.stop();
        telnetClient = nC;
        log("--- Connected Telnet ---");
      } else nC.stop();
    }
    delay(10);
  }
}
