# EhZProject

ESP8266 (NodeMCU v2) PlatformIO project that reads energy-consumption data
from an EHZ Smart Meter via the SML protocol over a serial port and publishes
the readings to an MQTT broker.

---

## Hardware wiring

| EHZ meter IR head | NodeMCU pin | GPIO   | Used for                          |
|--------------------|-------------|--------|------------------------------------|
| TX (data out)      | **D5**      | GPIO14 | `METER_RX_PIN` — meter data into the board |
| —                  | D6          | GPIO12 | `METER_TX_PIN` — declared by SoftwareSerial but never used (we only read, never send to the meter) |
| GND                | GND         | —      | common ground                      |

(D4/GPIO2 is only the board's built-in blue LED — unrelated to the meter
interface, nothing needs to be connected there. Confirmed against the
ESP8266 Arduino core's official `nodemcu` pin table, `D0..D10` map to
GPIO `16,5,4,0,2,14,12,13,15,3,1` respectively.)

The optical IR reader delivers 9600 baud, 8N1 TTL serial.  
A SoftwareSerial instance on **D5 (RX) / D6 (TX)** is used by default so that
the USB UART (`Serial`) can still be used for debug output.

### Signal inversion

Many DIY IR read heads (phototransistor + pull-up) output an **inverted**
signal relative to normal TTL UART. That's the default assumption here:

```cpp
#define METER_SERIAL_INVERTED true   // src/main.cpp
```

**Symptom of getting this wrong:** `/debug` shows a nonzero meter byte
counter but 0 telegrams found, and the raw hex dump (or
`tools/raw_monitor.ps1`) never contains the `1B 1B 1B 1B` SML start
sequence — just repeated `FF`/`FE`-ish runs instead. Flip the define to
`false` if your specific read head turns out not to be inverted.

(Tasmota's `sensor53` meter driver supports the same concept via the
`=so2 4` script command, for comparison if you're cross-testing with it.)

If you want to use hardware UART instead (e.g. for better reliability at
higher data rates), uncomment `#define USE_HARDWARE_SERIAL` in `src/main.cpp`
and connect the meter head to the NodeMCU RX pin (D9/GPIO3).  
Note: this disables the debug output on the same port.

---

## Configuration

WiFi and MQTT settings are entered through a web portal ([IotWebConf](https://github.com/prampec/IotWebConf)) — no reflashing needed to change them.

**First-time setup:**
1. Flash the firmware and power up the board.
2. It can't connect to any known WiFi yet, so it opens its own access point
   named **`ehz-esp8266`**, password `ehzsetup` (defined in `src/main.cpp`,
   change before flashing if you want a different one).
3. Connect a phone/laptop to that AP and open `http://192.168.4.1/`.
4. Fill in your WiFi SSID/password and the MQTT settings (broker, port, user,
   password — user/password may be left empty if the broker has no auth),
   then save. The device reboots and joins your WiFi.

**Changing settings later:** once the device is on your network, open
`http://<device-ip>/config` (find the IP via your router or the serial
monitor) to update WiFi or MQTT settings at any time.

---

## MQTT topics

All energy values are published in **kWh**; power in **W**.
Values are retained on the broker.

| Topic                   | Unit | OBIS    | Description                  |
|-------------------------|------|---------|------------------------------|
| `ehz/energy/consumed`   | kWh  | 1.8.0   | Consumed energy                |
| `ehz/energy/produced`   | kWh  | 2.8.0   | Produced energy                |
| `ehz/power/current`     | W    | 16.7.0  | Current active power (optional — defaults to 0 if the meter does not send it) |

---

## Web pages

| Path       | Purpose                                                          |
|------------|-------------------------------------------------------------------|
| `/`        | Landing page with links to the pages below.                      |
| `/values`  | Last 20 parsed measurements (age, consumed/produced kWh, power W), auto-refreshes every 15s. Shows every reading the parser produces, independent of the MQTT dead-band. |
| `/debug`   | WiFi/MQTT connection status, meter byte counter, telegram counters (found vs. valid), free heap, and a hex dump of the last ~300 raw bytes received from the meter. Auto-refreshes every 10s. |
| `/config`  | WiFi/MQTT configuration form (see above).                         |

---

## Debugging the meter link

If no data reaches MQTT, check `/debug` first — it tells you which of these
is the case:

- **No bytes at all** (byte counter stuck at 0): wiring/pin/baud rate problem.
- **Bytes arriving, but "found" telegram count stays 0**: the byte stream
  never matches the SML start/stop framing (noise, wrong baud, wrong pins).
- **Telegrams "found" but not "valid"**: framing is fine but the consumed/
  produced OBIS registers weren't located in the telegram - the meter may
  need a raw capture to investigate further (see below).
- **Valid telegrams but MQTT shows "getrennt"**: broker/credentials problem,
  not a meter/parsing problem.

For a full raw byte-level capture (e.g. to verify the exact SML telegram
structure a new meter sends), the device can mirror every byte it reads from
the meter to a raw TCP port (`8266` by default). This is **off by default**
— enable it under `/config` → "Debug-Einstellungen" → "Roh-TCP-Debug (Port
8266) aktivieren" (no reflashing needed; takes effect immediately on save).
Turn it back off once you're done, since it's an unauthenticated raw byte
stream.

From a PC on the same network, once enabled:

```powershell
tools/raw_monitor.ps1 -DeviceIp 192.168.1.50
# or: tools/raw_monitor.ps1 -DeviceIp 192.168.1.50 -LogFile capture.txt
```

Only one raw TCP client is served at a time; connecting a new one drops the
previous one.

---

## SML parsing

`SmlParser` decodes the meter's SML telegrams generically: it locates each
OBIS register by its objName, then walks the standard SML TLV
(Type-Length-Value) encoding to read that entry's `scaler` and `value`
fields dynamically (`value * 10^scaler`), rather than assuming fixed byte
offsets for one specific meter. This means it should work with any
SML-conformant meter that reports OBIS 1.8.0 (consumed), 2.8.0 (produced),
and — optionally — 16.7.0 (current power), regardless of vendor-specific
telegram layout differences.

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
- [prampec/IotWebConf](https://github.com/prampec/IotWebConf) `^3.2.1`
  — WiFi/AP web configuration portal.
