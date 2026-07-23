# EhZProject

ESP8266 (NodeMCU v2) PlatformIO project that reads energy-consumption data
from an EHZ Smart Meter via the SML protocol over a serial port and publishes
the readings to an MQTT broker.

---

## Hardware wiring

| EHZ meter IR head | NodeMCU v2 pin |
|-------------------|----------------|
| TX (data out)     | D5 (GPIO14)    |
| GND               | GND            |

The optical IR reader delivers 9600 baud, 8N1 TTL serial.  
A SoftwareSerial instance on **D5 (RX) / D6 (TX)** is used by default so that
the USB UART (`Serial`) can still be used for debug output.

If you want to use hardware UART instead (e.g. for better reliability at
higher data rates), uncomment `#define USE_HARDWARE_SERIAL` in `src/main.cpp`
and connect the meter head to the NodeMCU RX pin (D9/GPIO3).  
Note: this disables the debug output on the same port.

---

## Configuration

### Wi-Fi

Edit the `#define` constants at the top of `src/main.cpp`:

```cpp
#define WIFI_SSID      "your-ssid"
#define WIFI_PASSWORD  "your-password"
```

### MQTT (compile-time defaults + web UI)

The MQTT broker address, port, username and password can be configured in two
ways:

1. **Compile-time defaults** – edit the `MQTT_BROKER_DEFAULT` / `MQTT_PORT_DEFAULT`
   / `MQTT_USER_DEFAULT` / `MQTT_PASS_DEFAULT` defines in `src/main.cpp` before
   flashing.  These are only used when the EEPROM has never been written.

2. **Web UI** – after the device boots and connects to Wi-Fi, open
   `http://<device-ip>/config` in a browser.  Fill in the fields and click
   **Save & Apply**.  The settings are written to EEPROM and survive power
   cycles and resets.

---

## Web Dashboard

A built-in HTTP server runs on port 80 and provides two pages:

| URL | Description |
|-----|-------------|
| `http://<ip>/`       | Live dashboard: current meter readings + uptime + MQTT status (auto-refreshes every 10 s) |
| `http://<ip>/config` | MQTT settings form (host, port, user, password) |

The device IP address is printed to the serial monitor on every boot:

```
[Web] Server started at http://192.168.x.y
```

---

## MQTT topics

All energy values are published in **kWh**; power in **W**.
Values are retained on the broker.

| Topic                   | Unit | OBIS    | Description                  |
|-------------------------|------|---------|------------------------------|
| `ehz/energy/consumed1`  | kWh  | 1.8.0   | Consumed energy tariff 1     |
| `ehz/energy/produced1`  | kWh  | 2.8.0   | Produced energy tariff 1     |
| `ehz/energy/consumed2`  | kWh  | 1.8.1   | Consumed energy tariff 2     |
| `ehz/energy/produced2`  | kWh  | 2.8.1   | Produced energy tariff 2     |
| `ehz/power/current`     | W    | 16.7.0  | Current active power         |

---

## Rate limiting (dead-band)

To avoid flooding the broker, each value is subject to a dead-band:

- **Minimum interval**: 15 seconds between any two publishes of the same topic.
- **Force publish**: even if the value has not changed, it is republished after
  10 minutes to confirm the reading is still live.

---

## Building & flashing

```bash
# Install PlatformIO Core or use the VS Code extension
pio run -e nodemcuv2          # build
pio run -e nodemcuv2 -t upload # flash
pio device monitor             # serial monitor at 115200 baud
```

---

## Dependencies

- [knolleary/PubSubClient](https://github.com/knolleary/pubsubclient) `^2.8`
  — MQTT client for Arduino/ESP8266.
