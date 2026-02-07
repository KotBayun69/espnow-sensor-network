#include "transport.h"

// Gateway MAC: C4:5b:be:61:86:09
uint8_t gatewayAddress[] = {0xC4, 0x5B, 0xBE, 0x61, 0x86, 0x09};

esp_now_peer_info_t peerInfo;
static bool otaRequested = false;
static bool updateRequested = false;
static bool configRequested = false;
static bool ackReceived = false;

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    if (len == 0) return;
    
    uint8_t msgType = incomingData[0];
    
    if (msgType == MSG_ACK && len >= sizeof(AckMessage)) {
        Serial.println("ACK received");
        ackReceived = true;
    }
    else if (msgType == MSG_CMD && len >= sizeof(CmdMessage)) {
        CmdMessage cmd;
        memcpy(&cmd, incomingData, sizeof(CmdMessage));
        
        switch (cmd.cmdType) {
            case CMD_OTA:
                otaRequested = cmd.value;
                Serial.printf("CMD: OTA = %s\n", cmd.value ? "ON" : "OFF");
                break;
                
            case CMD_RESTART:
                if (cmd.value) {
                    Serial.println("CMD: Restart requested");
                    delay(100);
                    ESP.restart();
                }
                break;
            
            case CMD_UPDATE:
                Serial.println("CMD: Force Update requested");
                otaRequested = false; // Reuse/misuse? No, let's add a new flag
                // See below for added static bool
                updateRequested = true; 
                break;
            
            case CMD_CONFIG:
                Serial.println("CMD: Config resend requested");
                configRequested = true;
                break;
                
            default:
                Serial.printf("CMD: Unknown command type %d\n", cmd.cmdType);
                break;
        }
    }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Send Status: Success");
  } else {
    Serial.println("Send Status: Failed");
  }
}

void initTransport() {
    WiFi.mode(WIFI_STA);
    
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    // Register peer
    memcpy(peerInfo.peer_addr, gatewayAddress, 6);
    peerInfo.channel = 1;  
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK){
        Serial.println("Failed to add peer");
        return;
    }
}

bool sendConfigMessage(ConfigMessage msg) {
    esp_err_t result = esp_now_send(gatewayAddress, (uint8_t *) &msg, sizeof(msg));
    
    if (result == ESP_OK) {
        Serial.println("Sent Config Message");
        return true;
    } else {
        Serial.println("Error sending Config Message");
        return false;
    }
}

bool sendDataMessage(DataMessage msg) {
    esp_err_t result = esp_now_send(gatewayAddress, (uint8_t *) &msg, sizeof(msg));
    
    if (result == ESP_OK) {
        Serial.println("Sent Data Message");
        return true;
    } else {
        Serial.println("Error sending Data Message");
        return false;
    }
}

bool isOtaRequested() {
    return otaRequested;
}

void clearOtaRequest() {
    otaRequested = false;
}

bool isUpdateRequested() {
    return updateRequested;
}

void clearUpdateRequest() {
    updateRequested = false;
}

bool isConfigRequestRequested() {
    return configRequested;
}

void clearConfigRequest() {
    configRequested = false;
}

bool hasAckBeenReceived() {
    return ackReceived;
}

void clearAckFlag() {
    ackReceived = false;
}
