# ESP-NOW Sensor Network

This project consists of three components for an ESP-NOW based sensor network:
- **Sensor**: Collects sensor data and sends it via ESP-NOW.
- **Gateway**: Recieves ESP-NOW data and forwards it to the Transmitter.
- **Transmitter**: Receives data from the Gateway and publishes it to MQTT / Home Assistant.

## Project Structure
- `ESPNOW_Sensor/`: ESP32C3 Super Mini Sensor code.
- `ESPNOW_Gateway/`: D1 Mini Gateway code.
- `ESPNOW_Transmitter/`: D1 Mini Transmitter code.
