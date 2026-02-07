# ESP-NOW Sensor Network

A low-power, long-range sensor network using ESP-NOW to communicate between battery-powered sensors and a central MQTT gateway.

## Architecture

1.  **Sensor Nodes (ESP32-C3 SuperMini)**:
    -   Wake up periodically (default 15s).
    -   Read sensors (BME280, BH1750, Capacitive Soil Moisture).
    -   Send data via **ESP-NOW** to the Gateway.
    -   Enter Deep Sleep to conserve battery.

2.  **Gateway (Wemos D1 Mini / ESP8266)**:
    -   Always powered.
    -   Receives ESP-NOW messages from sensors.
    -   Buffers and forwards messages via **SoftwareSerial** to the Transmitter.
    -   Queues "Wake Up" commands (OTA/Calibration) for sleeping sensors.
    -   *Note: Does not connect to MQTT/WiFi during normal operation.*

3.  **Transmitter (Wemos D1 Mini / ESP8266)**:
    -   Connects to WiFi and MQTT Broker.
    -   Receives serial data from Gateway -> Publishes to MQTT.
    -   Receives MQTT commands -> Forwards to Gateway (to control sensors).
    -   Handles **Home Assistant Auto-Discovery**.

---

## Features

-   **Low Power**: Sensors use Deep Sleep 99% of the time.
-   **Long Range**: ESP-NOW protocol offers better range/speed than standard WiFi for short bursts.
-   **OTA Updates**: Sensors can be woken up remotely via MQTT to accept Over-The-Air firmware updates.
-   **Soil Calibration**: Interactive calibration mode for soil moisture sensors.
-   **Auto-Discovery**: Sensors appear automatically in Home Assistant.

---

## Hardware

-   **Sensor**: ESP32-C3 SuperMini
    -   **Soil Sensor**: Analog Pin 0 (A0), Power Pin 2.
    -   **I2C Sensors**: SDA=8, SCL=9.
-   **Gateway/Transmitter**: Wemos D1 Mini
    -   Connected via Serial (RX/TX).

---

## Setup & Configuration

### Flashing
The project uses **PlatformIO**.
-   **Sensor**: `pio run -e esp32_c3_super_mini -t upload`
-   **Gateway**: `pio run -e d1_mini -t upload` in `ESPNOW_Gateway` folder.
-   **Transmitter**: `pio run -e d1_mini -t upload` in `ESPNOW_Transmitter` folder.

### Initial Configuration (Transmitter & Sensor OTA)
On first boot (or if connection fails), the device starts a WiFi Access Point (e.g., `ESPNOW-Transmitter` or `ESP-NOW-DEVICE-OTA`).
1.  Connect to the AP.
2.  Go to `192.168.4.1`.
3.  Enter WiFi and MQTT credentials.
4.  Save and Restart.

---

## MQTT Interface

Replace `<device_slug>` with the lowercase device name (e.g., `esp32_sensor`).

### Topics
| Topic | Direction | Description |
| :--- | :--- | :--- |
| `homeassistant/...` | Out | Auto-discovery configs |
| `espnow/<device_slug>/state` | Out | Sensor readings (JSON) |
| `espnow/<device_slug>/status` | Out | Device status / Calibration feedback |
| `espnow/<device_slug>/control` | In | Command payloads |
| `espnow/<device_slug>/calibrate` | In | Calibration payloads (`dry`, `wet`) |

### Commands (`.../control`)

**1. Wake Up / Enter OTA Mode**
Prevents the sensor from sleeping on next wake-up.
```json
{"cmd": "ota"} 
// OR 
{"cmd": "calibrate"}
```

### Commands (`.../control`)

**1. General Commands (All Devices)**
```json
{"cmd": "restart"}
```

**2. Sensor Specific**
```json
{"cmd": "ota"}       // Wake up for OTA/Calibration
{"cmd": "calibrate"} // Alias for "ota"
```

**3. Transmitter Specific (`espnow/transmitter/control`)**
```json
{"cmd": "ota"}       // Main ESP8266 OTA
{"cmd": "restart"}
```

**4. Gateway Specific (`espnow/gateway/control`)**
```json
{"cmd": "restart"}
```

### Home Assistant Integration
The system automatically discovers devices in Home Assistant via MQTT Discovery:
-   **Sensors**: Battery, Temperature, Humidity, Pressure, Lux, Soil Moisture.
-   **Buttons**:
    -   `Restart`: Reboot the device.
    -   `Wake Up / OTA`: Put device in OTA mode (or wake for calibration).
    -   `Calibrate`: Start soil sensor calibration (if applicable).
-   **Status Monitoring**:
    -   **Transmitter**: Reports `online`/`offline` via MQTT LWT.
    -   **Gateway**: Reports status via heartbeat to Transmitter.
    -   **Sensors**: Mark as `unavailable` if no data received for `3 * SleepInterval`.


---

## How-To: Calibrate Soil Sensor

1.  **Wake Up the Sensor**:
    Publish `{"cmd": "calibrate"}` to `espnow/<device_slug>/control`.
    *Wait for the sensor to wake up (Green LED or "status": "ota" message).*

2.  **Calibrate DRY (0%)**:
    Hold the sensor in the air.
    Publish `dry` to `espnow/<device_slug>/calibrate`.
    *Wait for "status": "done".*

3.  **Calibrate WET (100%)**:
    Submerge the sensor in water.
    Publish `wet` to `espnow/<device_slug>/calibrate`.
    *Wait for "status": "done".*

4.  **Finish**:
    Publish `{"cmd": "restart"}` to `espnow/<device_slug>/control`.
    *Device will report "restarting in 10 seconds" and reboot.*

---

## How-To: Updates (OTA)

1.  Send **Wake Up** command (see above).
2.  Once sensor stays awake, find its IP address in the `status` topic.
3.  Use PlatformIO or `espota` to upload firmware to that IP.
4.  Device automatically restarts after update.
