# EhZProject

ESP8266 (NodeMCU v2) PlatformIO project that reads energy-consumption data
from an EHZ Smart Meter via the SML protocol over a serial port and publishes
the readings to a [NATS](https://nats.io) server.

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

Edit the `#define` constants at the top of `src/main.cpp`:

```cpp
#define WIFI_SSID        "your-ssid"
#define WIFI_PASSWORD    "your-password"

#define NATS_SERVER      "192.168.1.100"   // IP or hostname of your NATS server
#define NATS_PORT        4222              // default NATS port
#define NATS_CLIENT_NAME "ehz-esp8266"
#define NATS_USER        ""               // leave empty if no auth
#define NATS_PASSWORD    ""
```

---

## NATS subjects

All energy values are published in **kWh**; power in **W**.

| Subject                   | Unit | OBIS    | Description                  |
|---------------------------|------|---------|------------------------------|
| `ehz.energy.consumed1`    | kWh  | 1.8.0   | Consumed energy tariff 1     |
| `ehz.energy.produced1`    | kWh  | 2.8.0   | Produced energy tariff 1     |
| `ehz.energy.consumed2`    | kWh  | 1.8.1   | Consumed energy tariff 2     |
| `ehz.energy.produced2`    | kWh  | 2.8.1   | Produced energy tariff 2     |
| `ehz.power.current`       | W    | 16.7.0  | Current active power         |
| `ehz.energy.json`         | —    | all     | All values as a JSON object  |

> **Note:** NATS does not support retained messages. Subscribers must be
> online at the time a message is published to receive it.

---

## Rate limiting (dead-band)

To avoid flooding the server, each value is subject to a dead-band:

- **Minimum interval**: 15 seconds between any two publishes of the same subject.
- **Force publish**: even if the value has not changed, it is republished after
  10 minutes to confirm the reading is still live.

---

## NATS protocol

The firmware implements the [NATS client protocol](https://docs.nats.io/reference/reference-protocols/nats-protocol)
directly over a plain TCP `WiFiClient` connection — no additional library is
required:

1. On connect the server sends an `INFO` JSON line.
2. The firmware replies with a `CONNECT` JSON command (credentials included
   when `NATS_USER` is non-empty).
3. Values are published with `PUB subject nbytes\r\npayload\r\n`.
4. Incoming `PING` messages from the server are answered with `PONG` to keep
   the connection alive.

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

No external libraries are required beyond the ESP8266 Arduino core
(`ESP8266WiFi`, `SoftwareSerial`) which is provided by the
`espressif8266` PlatformIO platform.
