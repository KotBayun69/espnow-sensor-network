#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "protocol.h"

// Device-side transport functions
void initTransport();
bool sendConfigMessage(ConfigMessage msg);
bool sendDataMessage(DataMessage msg);
bool isOtaRequested();  // Check if OTA mode was requested via CMD
void clearOtaRequest(); // Clear the flag
bool isUpdateRequested(); 
void clearUpdateRequest();
bool isConfigRequestRequested();
void clearConfigRequest();
bool hasAckBeenReceived(); // Check if MSG_ACK was received
void clearAckFlag();       // Reset the ACK flag

#endif
