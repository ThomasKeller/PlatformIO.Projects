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

### Status LED

The onboard blue LED (D4/GPIO2) briefly flashes every time a telegram is
successfully parsed (passes CRC and yields a valid measurement) — a quick
visual "the meter link is alive" indicator without needing to open
`/debug`. It's non-blocking (no `delay()`), so it never interferes with
WiFi/MQTT/serial handling.

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

`/config` also has an **"Übertragungs-Einstellungen"** section with
**"Sendeintervall Live-Werte (s)"** - the minimum interval between two
publishes of `ehz/energy/consumed`, `ehz/energy/produced`, and
`ehz/power/current` (see "Rate limiting" below). Range 5–300 seconds,
default 15; out-of-range input is clamped on the device itself, not just
rejected client-side. Does not affect the rolling-hourly topics, which
always cover a fixed 60-minute window (see "Rolling hourly aggregates").

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
| `ehz/power/current`     | W    | 16.7.0  | Time-weighted average active power over the dead-band window since the previous publish (optional — defaults to 0 if the meter does not send it), see "Rate limiting" below |
| `ehz/power/current/min` | W    | 16.7.0  | Minimum instantaneous power reading seen in that same interval |
| `ehz/power/current/max` | W    | 16.7.0  | Maximum instantaneous power reading seen in that same interval |
| `ehz/power/current/count` | –  | –       | Number of telegrams/readings folded into that interval's average/min/max |
| `ehz/energy/consumed/hourly` | kWh | 1.8.0 | Consumed energy delta over the last rolling 60-minute window, see "Rolling hourly aggregates" below |
| `ehz/energy/produced/hourly` | kWh | 2.8.0 | Produced energy delta over the last rolling 60-minute window |
| `ehz/power/hourly/avg`  | W    | 16.7.0  | Time-weighted average power over the last rolling 60-minute window |
| `ehz/power/hourly/min`  | W    | 16.7.0  | Minimum instantaneous power seen in the last rolling 60-minute window |
| `ehz/power/hourly/max`  | W    | 16.7.0  | Maximum instantaneous power seen in the last rolling 60-minute window |

---

## Web pages

| Path       | Purpose                                                          |
|------------|-------------------------------------------------------------------|
| `/`        | Landing page with links to the pages below.                      |
| `/values`  | Last 20 parsed measurements (device uptime at the reading, consumed/produced kWh, power W), auto-refreshes every 15s. Shows every reading the parser produces, independent of the MQTT dead-band. Also lists the last 24 completed rolling-hourly windows (consumed/produced delta, average/min/max power), see "Rolling hourly aggregates". |
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

### Other OBIS registers/fields present in the telegram but not parsed

Decoded from a real capture (`tools/capture.txt`) of this device's meter (a
Landis+Gyr EHZ), each `SML_ListEntry` in the telegram carries more than the
three registers `SmlParser` currently extracts. None of these are exposed
today; noted here so they don't need re-reverse-engineering later if a
future need comes up:

| OBIS / field | Meaning | Example value seen | Notes |
|---|---|---|---|
| `1-0:96.1.0*255` | Meter server-ID / serial number | `0A 01 4C 47 5A 00 05 58 28 97` (raw octet string, vendor-specific encoding) | Static - never changes |
| `1-0:96.50.1*1` | Manufacturer ID | `"LGZ"` (Landis+Gyr) | Static - never changes |
| Status (2nd field of every `SML_ListEntry`, not an OBIS-addressed register) | Device status/error bitmask | `0x001C7904` on this meter's 1.8.0 entry; absent (optional-not-set) on the 2.8.0 and 16.7.0 entries in the same telegram | Bit meanings are vendor-specific; would need Landis+Gyr documentation to decode meaningfully |
| valTime/secIndex (3rd field) | Meter-internal seconds counter at the time of that reading | `0x0003C85A` | Relative to the meter's own clock, not wall time - the device has no NTP/RTC (see "Rolling hourly aggregates") |
| `1-0:98.10.255*255` | SML list name (which named list this telegram represents) | structural, not a measurement | |

`SmlParser::searchAndParse()` already generically skips over status/valTime/
unit for whichever OBIS it's asked to find (see `skipElement()` in
[SmlParser.h](include/SmlParser.h)), so reading any of these would mean
adding a new `SEQ_*` byte pattern and either reusing `searchAndParse()` (for
the status/serial octet-string values, `readNumeric()`'s octet-string
handling would need extending since those aren't Integer/Unsigned fields)
or a small dedicated decoder.

---

## Rate limiting (dead-band)

To avoid flooding the broker, each value is subject to a dead-band:

- **Minimum interval**: 15 seconds by default between any two publishes of
  the same topic - configurable (5–300s) via `/config` → "Übertragungs-
  Einstellungen" → "Sendeintervall Live-Werte (s)", see "Configuration"
  above. Applies to `ehz/energy/consumed`, `ehz/energy/produced`, and
  `ehz/power/current` only; the rolling-hourly topics below always use a
  fixed 60-minute window regardless of this setting.
- **Force publish**: even if the value has not changed, it is republished after
  10 minutes to confirm the reading is still live.
- **`ehz/power/current` is a time-weighted average**, not the last-seen
  instantaneous reading: every parsed measurement's power value is folded
  into a running average (weighted by how long it held, i.e. the time since
  the previous telegram), and that average is what gets published and used
  for the dead-band's change check. The averaging window resets after each
  publish, so it always covers exactly the interval since the previous
  `ehz/power/current` message - smoothing out telegram-to-telegram jitter
  instead of publishing whichever single reading happened to land on the
  15s/10min tick. `ehz/energy/json`'s `currentPower` field uses the same
  averaged value for consistency. `/values`, by contrast, still shows every
  individual instantaneous reading, unaveraged.
- **`ehz/power/current/min`, `/max`, `/count`** cover that same interval:
  the lowest and highest instantaneous power reading seen, and how many
  readings were folded in. Published alongside `ehz/power/current` (same
  publish tick, same reset), so all four always describe the identical
  window. A `count` of 1 means the dead-band fired on the very first
  reading of a fresh window (e.g. right after startup or a long gap) - avg,
  min, and max are then all equal to that single reading.

---

## Rolling hourly aggregates

In addition to the dead-band-limited "live" topics above, `ehz/energy/*/hourly`
and `ehz/power/hourly/*` report aggregates over a **rolling 60-minute
window**: consumed/produced energy delta, and average/min/max power. Each
window opens on the first measurement after boot (or after the previous
window closed) and closes - publishing all five topics and opening the next
window - once 60 minutes have elapsed.

This window is a plain timer, **not aligned to the wall clock** (e.g. it
won't land on 13:00, 14:00, ...): the device has no NTP/RTC time source, so
"an hour" here means 60 minutes since the window last reset, which itself
depends on when the device booted. If the broker is unreachable when a
window closes, that hour's aggregate is simply dropped (not queued/retried)
- the window still resets on schedule so the next one isn't oversized.

These topics are **not retained** on the broker (unlike the live ones
above), since a stale hourly aggregate from before a restart/disconnect
would be misleading if picked up by a subscriber expecting a fresh value.

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
