# EhZProject

ESP8266 (NodeMCU v2) PlatformIO project that reads energy-consumption data
from an EHZ Smart Meter via the SML protocol over a serial port and publishes
the readings to an MQTT broker.

---

## Hardware wiring

**Hardware UART0 is the default** (`USE_HARDWARE_SERIAL` defined in
`src/main.cpp`) - measured on real hardware, SoftwareSerial had a **47% CRC
failure rate** (220/468 telegrams) under WiFi+webserver load, since
bit-banged reception is sensitive to interrupt jitter. Switching to hardware
UART - same read head, same physical alignment, nothing else changed -
dropped that to **~2%** (2/104). SoftwareSerial remains available as a
fallback (e.g. if you need D9/D10 for something else, or want the USB debug
output), see below.

| EHZ meter IR head | NodeMCU pin | GPIO   | Used for                          |
|--------------------|-------------|--------|------------------------------------|
| TX (data out)      | **D9**      | GPIO3  | Hardware UART0 RX - meter data into the board |
| RX (data in)       | D10         | GPIO1  | Hardware UART0 TX - unused (we only read, never send to the meter), wired for symmetry only |
| GND                | GND         | —      | common ground                      |

(D4/GPIO2 is only the board's built-in blue LED — unrelated to the meter
interface, nothing needs to be connected there. Confirmed against the
ESP8266 Arduino core's official `nodemcu` pin table, `D0..D10` map to
GPIO `16,5,4,0,2,14,12,13,15,3,1` respectively.)

The optical IR reader delivers 9600 baud, 8N1 TTL serial. Using hardware
UART0 for the meter means the USB UART (`Serial`) is unavailable for debug
text while the meter is connected — use `/debug` on the web UI instead,
which works independently.

**Falling back to SoftwareSerial:** comment out `#define USE_HARDWARE_SERIAL`
in `src/main.cpp` and rewire the read head to **D5** (RX, GPIO14) / **D6**
(TX, GPIO12) instead. This frees D9/D10 and keeps the USB debug output
working, at the cost of the higher CRC failure rate measured above.

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
monitor) to update WiFi or MQTT settings at any time. The device explicitly
restarts itself ~1.5s after any save (not left to IotWebConf's own state
machine, which isn't reliable to depend on for this) so the new settings are
always applied cleanly.

### If it won't join your WiFi

`/debug` shows two rows for this (reachable via the fallback AP itself at
`192.168.4.1/debug` even while it can't join your network):

- **IotWebConf-Status**: where its own state machine is - `Verbinde mit
  WLAN...` (trying), `Setup-Netz aktiv` (gave up, hosting its own AP),
  `Online`.
- **WiFi-Statuscode**: the ESP8266 SDK's own connection result. `Fehlgeschlagen`
  (`WL_CONNECT_FAILED`) means the SSID was found but the handshake failed -
  almost always a wrong password. `SSID nicht gefunden`
  (`WL_NO_SSID_AVAIL`) means the network name itself wasn't seen - wrong
  name, out of range, or a 5 GHz-only network (the ESP8266 only has a
  2.4 GHz radio).

Same info is also logged to the serial monitor (115200 baud) every time
either value changes, so you can see the retry history even without the web
UI. Default timing: it tries a saved WiFi network for **30s**, then falls
back to its own AP for **30s** before retrying - so expect the fallback AP
to reappear roughly every minute if the configured network can't be joined.

**If `IotWebConf-Status` stays stuck (never reaches "Verbinde mit WLAN...")
even with correct WiFi credentials:** check the serial log for `"AP password
was not set in configuration"` / `"Will stay in AP mode."`. This means the
**AP Password** field on `/config` (the ESP8266's *own* setup-hotspot
password - a separate field above WiFi/MQTT, easy to overlook since password
fields always render empty) is itself empty. IotWebConf treats that as a
security condition and permanently refuses to even attempt joining your
network, no matter how correct those credentials are, until that field has
a value. Set it (e.g. back to the initial `ehzsetup`) and save. The `/config`
page now shows a placeholder warning you if this field is empty, for exactly
this reason.

If credentials are genuinely correct and it still won't connect, see
"Resetting stored configuration" below - EEPROM corruption from an earlier
firmware version's different parameter layout is possible after an update.

### Resetting stored configuration

A normal reflash does **not** clear WiFi/MQTT settings - they live in a
separate EEPROM-backed flash region IotWebConf manages, untouched by
`pio run -t upload`. To force a genuinely clean slate (e.g. after getting
locked out with wrong settings, or bumping `CONFIG_VERSION`):

```powershell
pio run -e nodemcuv2 -t erase --upload-port COM5
pio run -e nodemcuv2 -t upload --upload-port COM5
```

After this the device boots fully unconfigured and reliably opens its setup
AP (`ehz-esp8266` / `ehzsetup`) again.

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
| `/values`  | Last 20 parsed measurements (device uptime at the reading, consumed/produced kWh, power W), auto-refreshes every 15s. Shows every reading the parser produces, independent of the MQTT dead-band. |
| `/debug`   | WiFi/MQTT connection status, meter byte counter, telegram counters (found / CRC failed / valid), free heap, and a hex dump of the last ~300 raw bytes received from the meter. Auto-refreshes every 10s. |
| `/config`  | WiFi/MQTT configuration form (see above).                         |

---

## Debugging the meter link

If no data reaches MQTT, check `/debug` first — it tells you which of these
is the case:

- **No bytes at all** (byte counter stuck at 0): wiring/pin/baud rate problem.
- **Bytes arriving, but "found" telegram count stays 0**: the byte stream
  never matches the SML start/stop framing (noise, wrong baud, wrong pins).
- **"CRC-Fehler" counting up**: telegrams are being received but fail the
  CRC16 check and are discarded rather than risk publishing a garbage
  reading. This points at bytes being dropped on the link - most likely
  SoftwareSerial's RX buffer overrunning while WiFi/webserver/MQTT compete
  for CPU time. A `raw_monitor.ps1` capture will typically show one or more
  otherwise-plausible-looking telegrams with missing/shifted bytes in the
  middle. See "SML parsing" below for what's already done to reduce this.
- **Telegrams "found" but not "valid" (and CRC not failing)**: framing/CRC
  is fine but the consumed/produced OBIS registers weren't located in the
  telegram - the meter may need a raw capture to investigate further.
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

**CRC16 validation:** every telegram's trailer includes a CRC16/X-25
checksum covering the whole message. The parser now waits for the full
trailer and verifies this checksum before decoding any fields, discarding
(and counting, see `/debug`) any telegram that fails - this is what
protects published MQTT values from data corruption on the serial link
(e.g. SoftwareSerial RX buffer overruns while WiFi/webserver/MQTT compete
for CPU time), rather than an ad-hoc "does this look plausible" check that
a corrupted-but-structurally-valid telegram could slip past. The
SoftwareSerial RX buffer is also sized up from the library default of 64
bytes to 256 to reduce how often that happens in the first place.

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
