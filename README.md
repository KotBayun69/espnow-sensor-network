# ESP-NOW Sensor Network

This project consists of three components for an ESP-NOW based sensor network:
- **Gateway**: Recieves ESP-NOW data and forwards it to the Transmitter.
- **Sensor**: Collects sensor data and sends it via ESP-NOW.
- **Transmitter**: Receives data from the Gateway and publishes it to MQTT / Home Assistant.

## Project Structure
- `ESPNOW_Gateway/`: ESP32 Gateway code.
- `ESPNOW_Sensor/`: XIAO ESP32C3 Sensor code.
- `ESPNOW_Transmitter/`: D1 Mini Transmitter code.
