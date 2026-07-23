#include <Arduino.h>

// IotWebConf's default WiFi password buffer is only 33 bytes (32 usable
// characters). WPA2-PSK allows up to 63 characters, and router-generated
// passwords (e.g. UniFi defaults) commonly exceed 32 - without this override
// a longer password gets silently truncated on save, so the device keeps
// "correctly" retrying a wrong (truncated) password and falls back to its
// own AP. Must be defined before including IotWebConf.h.
#define IOTWEBCONF_PASSWORD_LEN 65

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

#include "SmlParser.h"
#include "EhZMeasurement.h"
#include "DeadBand.h"
#include "MeasurementHistory.h"
#include "HourlyHistory.h"
#include "TimeWeightedAverage.h"

// ---------------------------------------------------------------------------
// IotWebConf identity / setup portal
//
// On first boot (or whenever WiFi can't connect), the device opens its own
// access point named THING_NAME, protected by WIFI_INITIAL_AP_PASSWORD.
// Connect to it and browse to 192.168.4.1 to enter your WiFi and MQTT
// settings - no reflashing needed to change them afterwards (open /config
// on the device's normal IP instead).
// ---------------------------------------------------------------------------
const char THING_NAME[]                = "ehz-esp8266";
const char WIFI_INITIAL_AP_PASSWORD[]  = "ehzsetup";
#define CONFIG_VERSION "ehz3"  // bump when the parameter set/layout below changes

#define MQTT_SERVER_LEN   128
#define MQTT_PORT_LEN     8
#define MQTT_USER_LEN     65
#define MQTT_PASSWORD_LEN 65
char mqttServerValue[MQTT_SERVER_LEN];
char mqttPortValue[MQTT_PORT_LEN] = "1883";
char mqttUserValue[MQTT_USER_LEN];
char mqttPasswordValue[MQTT_PASSWORD_LEN];

DNSServer dnsServer;
WebServer server(80);
IotWebConf iotWebConf(THING_NAME, &dnsServer, &server, WIFI_INITIAL_AP_PASSWORD, CONFIG_VERSION);

IotWebConfParameterGroup mqttGroup("mqtt", "MQTT-Einstellungen");
IotWebConfTextParameter mqttServerParam(
    "MQTT-Broker (Host/IP)", "mqttServer", mqttServerValue, MQTT_SERVER_LEN);
IotWebConfNumberParameter mqttPortParam(
    "MQTT-Port", "mqttPort", mqttPortValue, MQTT_PORT_LEN, "1883", "1..65535",
    "min='1' max='65535' step='1'");
IotWebConfTextParameter mqttUserParam(
    "MQTT-Benutzer (optional)", "mqttUser", mqttUserValue, MQTT_USER_LEN);
IotWebConfPasswordParameter mqttPasswordParam(
    "MQTT-Passwort (optional)", "mqttPassword", mqttPasswordValue, MQTT_PASSWORD_LEN);

// Off by default: opt in only while actively debugging the meter link.
#define DEBUG_TCP_ENABLED_LEN 9
IotWebConfParameterGroup debugGroup("debug", "Debug-Einstellungen");
char debugTcpEnabledValue[DEBUG_TCP_ENABLED_LEN];
IotWebConfCheckboxParameter debugTcpEnabledParam(
    "Roh-TCP-Debug (Port 8266) aktivieren", "debugTcpEnabled",
    debugTcpEnabledValue, DEBUG_TCP_ENABLED_LEN, false);

// Minimum interval between two publishes of the same "live" topic
// (ehz/energy/consumed, ehz/energy/produced, ehz/power/current) - see
// DeadBand. Clamped server-side to [PUBLISH_INTERVAL_MIN_S,
// PUBLISH_INTERVAL_MAX_S] in applyPublishInterval() regardless of what the
// form posts, the HTML min/max/step below are just browser-side hints.
#define PUBLISH_INTERVAL_LEN 5  // up to 3 digits ("300") + safety margin
#define PUBLISH_INTERVAL_MIN_S 5
#define PUBLISH_INTERVAL_MAX_S 300
IotWebConfParameterGroup publishGroup("publish", "&Uuml;bertragungs-Einstellungen");
char publishIntervalValue[PUBLISH_INTERVAL_LEN] = "15";
IotWebConfNumberParameter publishIntervalParam(
    "Sendeintervall Live-Werte (s)", "publishIntervalS",
    publishIntervalValue, PUBLISH_INTERVAL_LEN, "15", "5..300",
    "min='5' max='300' step='1'");

// MQTT topics (values are published as plain ASCII numbers)
#define TOPIC_STATUS     "ehz/status"
#define TOPIC_CONSUMED   "ehz/energy/consumed"   // kWh
#define TOPIC_PRODUCED   "ehz/energy/produced"   // kWh
#define TOPIC_POWER      "ehz/power/current"     // W, time-weighted average over the interval
#define TOPIC_POWER_MIN  "ehz/power/current/min"   // W, min instantaneous reading over the interval
#define TOPIC_POWER_MAX  "ehz/power/current/max"   // W, max instantaneous reading over the interval
#define TOPIC_POWER_COUNT "ehz/power/current/count" // number of readings folded into the interval
#define TOPIC_JSON       "ehz/energy/json"
#define TOPIC_UPTIME     "ehz/uptime/ms" // ms since epoch of the measurement

// Rolling 60-minute window aggregates (window is a fixed timer since the
// previous publish, not aligned to the wall clock - the device has no
// NTP/RTC time source).
#define TOPIC_CONSUMED_HOURLY  "ehz/energy/consumed/hourly"  // kWh, delta over the window
#define TOPIC_PRODUCED_HOURLY  "ehz/energy/produced/hourly"  // kWh, delta over the window
#define TOPIC_POWER_HOURLY_AVG "ehz/power/hourly/avg"        // W, time-weighted average
#define TOPIC_POWER_HOURLY_MIN "ehz/power/hourly/min"        // W
#define TOPIC_POWER_HOURLY_MAX "ehz/power/hourly/max"        // W

// ---------------------------------------------------------------------------
// Meter serial: hardware UART0 (D9=RX0/GPIO3, D10=TX0/GPIO1) is used by
// default rather than SoftwareSerial (D5/D6). Measured on real hardware:
// SoftwareSerial had a 47% CRC failure rate (220/468 telegrams) under
// WiFi+webserver load, since bit-banged reception is sensitive to interrupt
// jitter; switching to hardware UART - same read head, same alignment -
// dropped that to ~2% (2/104). Comment out USE_HARDWARE_SERIAL to fall back
// to SoftwareSerial on D5/D6 if you need the pins or the USB debug output
// for something else.
// ---------------------------------------------------------------------------
#define USE_HARDWARE_SERIAL
#define METER_RX_PIN  14  // D5 (only used by the SoftwareSerial fallback)
#define METER_TX_PIN  12  // D6 (only used by the SoftwareSerial fallback)

// Many DIY IR read heads (phototransistor + pull-up) output an inverted
// signal relative to normal TTL UART levels. If /debug shows bytes arriving
// but never framing into a valid telegram (no 0x1B x4 start sequence, lots
// of FF/FE runs in the hex dump), flip this.
#define METER_SERIAL_INVERTED false

// Onboard blue LED (D4/GPIO2, active-LOW) - unused by anything else on this
// board (see README), so it's free to use as a "still receiving valid
// telegrams" indicator: briefly lit on every successfully parsed measurement.
#define STATUS_LED_PIN 2
#define STATUS_LED_BLINK_MS 80

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
#ifndef USE_HARDWARE_SERIAL
SoftwareSerial meterSerial(METER_RX_PIN, METER_TX_PIN, METER_SERIAL_INVERTED);
#endif

WiFiClient    wifiClient;
PubSubClient  mqttClient(wifiClient);

SmlParser smlParser;

static const int HISTORY_SIZE = 20;
MeasurementHistory<HISTORY_SIZE> history;

// One dead-band instance per published value
DeadBand dbConsumed, dbProduced, dbPower;

// Time-weighted average of currentPower across each power dead-band window,
// so TOPIC_POWER reflects the average load over the interval rather than
// whichever instantaneous reading happened to land on the publish tick.
// powerMin/powerMax/powerSampleCount track the same window's extremes and
// sample count, for TOPIC_POWER_MIN/MAX/COUNT.
TimeWeightedAverage powerAvg;
double        powerMin         = 0.0;
double        powerMax         = 0.0;
unsigned long powerSampleCount = 0;

// Rolling 60-minute aggregation window (see TOPIC_*_HOURLY above): a plain
// timer since the last window reset, not aligned to the wall clock.
static const unsigned long HOUR_WINDOW_MS = 3600000UL;  // 60 min
bool          hourWindowOpen        = false;
unsigned long hourWindowStartMs     = 0;
double        hourStartConsumedKwh  = 0.0;
double        hourStartProducedKwh  = 0.0;
double        hourPowerMin          = 0.0;
double        hourPowerMax          = 0.0;
TimeWeightedAverage hourPowerAvg;

static const int HOURLY_HISTORY_SIZE = 24;  // ~1 day at 60 min/window
HourlyHistory<HOURLY_HISTORY_SIZE> hourlyHistory;

unsigned long lastStatusMs  = 0;
unsigned long lastMqttRetry = 0;
static const unsigned long STATUS_INTERVAL_MS = 30000UL;
static const unsigned long MQTT_RETRY_MS      = 5000UL;

bool statusLedOn = false;
unsigned long statusLedOffAtMs = 0;

// Lights the status LED; loop() below turns it off again after
// STATUS_LED_BLINK_MS without blocking.
void flashStatusLed(unsigned long now) {
    digitalWrite(STATUS_LED_PIN, LOW);  // active-LOW: on
    statusLedOn = true;
    statusLedOffAtMs = now + STATUS_LED_BLINK_MS;
}

// ---------------------------------------------------------------------------
// Debug: raw-byte ring buffer (for /debug's hex dump) + raw TCP passthrough
// so the meter's byte stream can be captured on a PC without touching the
// firmware (e.g. tools/raw_monitor.ps1, or `nc <device-ip> 8266`).
// ---------------------------------------------------------------------------
static const int RAW_BUF_SIZE = 300;
uint8_t rawBuf[RAW_BUF_SIZE];
int rawBufHead = 0;
int rawBufCount = 0;
unsigned long meterByteCount = 0;
unsigned long lastMeterByteMs = 0;

static const uint16_t RAW_TCP_PORT = 8266;
WiFiServer rawTcpServer(RAW_TCP_PORT);
WiFiClient rawTcpClient;
bool rawTcpServerRunning = false;

// Starts/stops the raw TCP debug listener to match the "debugTcpEnabled"
// checkbox - called once at boot and whenever the config portal saves.
void updateRawTcpServerState() {
    bool shouldRun = debugTcpEnabledParam.isChecked();
    if (shouldRun && !rawTcpServerRunning) {
        rawTcpServer.begin();
        rawTcpServer.setNoDelay(true);
        rawTcpServerRunning = true;
        Serial.println(F("[Debug] Raw TCP passthrough enabled (port 8266)"));
    } else if (!shouldRun && rawTcpServerRunning) {
        if (rawTcpClient) rawTcpClient.stop();
        rawTcpServer.close();
        rawTcpServerRunning = false;
        Serial.println(F("[Debug] Raw TCP passthrough disabled"));
    }
}

// Records one byte received from the meter for the debug page/hex dump, and
// mirrors it to the raw TCP client, if any is connected.
void recordMeterByte(uint8_t b) {
    rawBuf[rawBufHead] = b;
    rawBufHead = (rawBufHead + 1) % RAW_BUF_SIZE;
    if (rawBufCount < RAW_BUF_SIZE) rawBufCount++;
    meterByteCount++;
    lastMeterByteMs = millis();

    if (rawTcpClient && rawTcpClient.connected()) {
        rawTcpClient.write(b);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void publishFloat(const char* topic, double value, bool retained = true) {
    char buf[24];
    dtostrf(value, 1, 4, buf);
    if (!mqttClient.publish(topic, buf, retained)) {
        Serial.print(F("[MQTT] publish failed for topic: "));
        Serial.println(topic);
    }
}

void publishULong(const char* topic, unsigned long value, bool retained = true) {
    if (!mqttClient.publish(topic, String(value).c_str(), retained)) {
        Serial.print(F("[MQTT] publish failed for topic: "));
        Serial.println(topic);
    }
}

void publishMeasurementJson(const EhZMeasurement& m, double currentPower) {
    char payload[220];

    const char* t = "{\"consumedEnergy\":%.4f,"
               "\"producedEnergy\":%.4f,"
               "\"currentPower\":%.1f,"
               "\"valid\":%s}";
    // m.consumedEnergy/producedEnergy are in Wh (native SML unit); convert to
    // kWh here to match the scalar topics. currentPower is passed in
    // separately (rather than read from m) since it's the dead-band-window
    // average, not necessarily this particular telegram's instantaneous
    // reading.
    int n = snprintf(payload, sizeof(payload), t,
        m.consumedEnergy / 1000.0,
        m.producedEnergy / 1000.0,
        currentPower,
        m.valid ? "true" : "false");

    if (n <= 0 || n >= (int)sizeof(payload)) {
        Serial.println(F("[MQTT] JSON payload truncated/invalid"));
        return;
    }
    if (!mqttClient.publish(TOPIC_JSON, payload, true)) {
        Serial.println(F("[MQTT] publish failed for JSON topic"));
    }
}

// Updates the rolling 60-minute aggregation window with one new reading, and
// publishes+resets it once HOUR_WINDOW_MS has elapsed since it opened. The
// window itself always advances on schedule (even if MQTT is briefly down,
// so it doesn't balloon into an oversized interval once reconnected); only
// the publish step is skipped without a broker connection, so that hour's
// aggregate is simply lost rather than queued.
void updateHourlyAggregation(unsigned long now, double consumedKwh, double producedKwh, double power) {
    if (!hourWindowOpen) {
        hourWindowOpen       = true;
        hourWindowStartMs    = now;
        hourStartConsumedKwh = consumedKwh;
        hourStartProducedKwh = producedKwh;
        hourPowerMin         = power;
        hourPowerMax         = power;
        hourPowerAvg.reset(now, power);
        return;
    }

    if (power < hourPowerMin) hourPowerMin = power;
    if (power > hourPowerMax) hourPowerMax = power;
    double avgPower = hourPowerAvg.sample(now, power);

    if (now - hourWindowStartMs < HOUR_WINDOW_MS) return;

    HourlyHistory<HOURLY_HISTORY_SIZE>::Entry entry = {
        now,
        consumedKwh - hourStartConsumedKwh,
        producedKwh - hourStartProducedKwh,
        avgPower,
        hourPowerMin,
        hourPowerMax,
    };
    hourlyHistory.push(entry);

    if (mqttClient.connected()) {
        // Not retained: a stale hourly aggregate from before a
        // restart/disconnect would mislead a subscriber picking up a
        // "current" value on connect.
        publishFloat(TOPIC_CONSUMED_HOURLY, consumedKwh - hourStartConsumedKwh, false);
        publishFloat(TOPIC_PRODUCED_HOURLY, producedKwh - hourStartProducedKwh, false);
        publishFloat(TOPIC_POWER_HOURLY_AVG, avgPower, false);
        publishFloat(TOPIC_POWER_HOURLY_MIN, hourPowerMin, false);
        publishFloat(TOPIC_POWER_HOURLY_MAX, hourPowerMax, false);
    }

    hourWindowStartMs    = now;
    hourStartConsumedKwh = consumedKwh;
    hourStartProducedKwh = producedKwh;
    hourPowerMin         = power;
    hourPowerMax         = power;
    hourPowerAvg.reset(now, power);
}

// Applies publishIntervalValue (from /config) to the three "live" topics'
// dead-bands, clamping server-side since the form's min/max/step are only
// browser-side hints - called once at boot and whenever the config portal
// saves.
void applyPublishInterval() {
    long seconds = atol(publishIntervalValue);
    if (seconds < PUBLISH_INTERVAL_MIN_S) seconds = PUBLISH_INTERVAL_MIN_S;
    if (seconds > PUBLISH_INTERVAL_MAX_S) seconds = PUBLISH_INTERVAL_MAX_S;
    unsigned long intervalMs = (unsigned long)seconds * 1000UL;
    dbConsumed.timeDeadBandMs = intervalMs;
    dbProduced.timeDeadBandMs = intervalMs;
    dbPower.timeDeadBandMs    = intervalMs;
}

// Called by IotWebConf right after new settings are persisted to EEPROM.
// IotWebConf itself does not reboot after a save - it relies on its state
// machine picking up new WiFi settings during normal operation, which is
// unreliable to depend on (that's what left the device unreachable after a
// WiFi credential typo). Restarting explicitly makes "save -> reboot ->
// reconnect with new settings" deterministic. The actual restart happens a
// little later from loop(), so the "saved" confirmation page the browser is
// waiting for has time to be sent first.
bool restartPending = false;
unsigned long restartAtMs = 0;

void configSaved() {
    Serial.println(F("[Config] Saved - restarting to apply new settings"));
    mqttClient.disconnect();
    mqttClient.setServer(mqttServerValue, atoi(mqttPortValue));
    updateRawTcpServerState();
    applyPublishInterval();
    restartPending = true;
    restartAtMs = millis() + 1500;
}

bool connectMqtt() {
    if (mqttClient.connected()) return true;
    if (strlen(mqttServerValue) == 0) return false;  // not configured yet

    unsigned long now = millis();
    if (now - lastMqttRetry < MQTT_RETRY_MS) return false;
    lastMqttRetry = now;

    Serial.print(F("[MQTT] Connecting to "));
    Serial.print(mqttServerValue);
    Serial.print(F(" ... "));

    bool ok;
    if (strlen(mqttUserValue) > 0) {
        ok = mqttClient.connect(
            THING_NAME,
            mqttUserValue,
            mqttPasswordValue,
            TOPIC_STATUS,   // will topic
            1,              // will QoS
            true,           // will retained
            "stopped"       // will payload
        );
    } else {
        ok = mqttClient.connect(
            THING_NAME,
            TOPIC_STATUS,   // will topic
            1,              // will QoS
            true,           // will retained
            "stopped"       // will payload
        );
    }

    if (ok) {
        Serial.println(F("connected."));
        mqttClient.publish(TOPIC_STATUS, "started", true);
    } else {
        Serial.print(F("failed, rc="));
        Serial.println(mqttClient.state());
    }
    return ok;
}

// ---------------------------------------------------------------------------
// WiFi/IotWebConf diagnostics
// ---------------------------------------------------------------------------
const char* networkStateName(iotwebconf::NetworkState state) {
    switch (state) {
        case iotwebconf::Boot:          return "Startet";
        case iotwebconf::NotConfigured: return "Nicht konfiguriert";
        case iotwebconf::ApMode:        return "Setup-Netz aktiv (AP-Modus)";
        case iotwebconf::Connecting:    return "Verbinde mit WLAN...";
        case iotwebconf::OnLine:        return "Online";
        case iotwebconf::OffLine:       return "Offline";
        default:                       return "Unbekannt";
    }
}

// wl_status_t values, in particular WL_CONNECT_FAILED, is the ESP8266's own
// signal for "auth/handshake failed" - i.e. almost always a wrong password
// (as opposed to WL_NO_SSID_AVAIL, which means the network name itself
// wasn't found/visible).
const char* wifiStatusName(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "Leerlauf";
        case WL_NO_SSID_AVAIL:   return "SSID nicht gefunden (Name falsch oder außer Reichweite)";
        case WL_SCAN_COMPLETED:  return "Scan abgeschlossen";
        case WL_CONNECTED:       return "Verbunden";
        case WL_CONNECT_FAILED:  return "Fehlgeschlagen (häufigste Ursache: falsches Passwort)";
        case WL_CONNECTION_LOST: return "Verbindung verloren";
        case WL_DISCONNECTED:    return "Getrennt";
        default:                return "Unbekannt";
    }
}

// Logs to Serial only when the WiFi/IotWebConf state actually changes, so the
// serial monitor builds a readable history of connection attempts over time
// instead of one line per loop() iteration.
iotwebconf::NetworkState lastLoggedState  = (iotwebconf::NetworkState)255;
wl_status_t              lastLoggedWifi   = (wl_status_t)255;

void logWifiStateChanges() {
    iotwebconf::NetworkState state = iotWebConf.getState();
    wl_status_t wifiStatus = WiFi.status();

    if (state != lastLoggedState || wifiStatus != lastLoggedWifi) {
        Serial.print(F("[WiFi] t="));
        Serial.print(millis() / 1000UL);
        Serial.print(F("s state="));
        Serial.print(networkStateName(state));
        Serial.print(F(" wifi="));
        Serial.print(wifiStatusName(wifiStatus));
        Serial.print(F(" (code "));
        Serial.print((int)wifiStatus);
        Serial.println(F(")"));
        lastLoggedState = state;
        lastLoggedWifi  = wifiStatus;
    }
}

// ---------------------------------------------------------------------------
// Web pages
// ---------------------------------------------------------------------------

// Shared look for all our own pages (not IotWebConf's own /config UI).
// Light/dark aware via CSS custom properties + prefers-color-scheme.
const char PAGE_STYLE[] =
    ":root{--bg:#f4f6f8;--card:#ffffff;--text:#1c2530;--muted:#6b7785;"
    "--accent:#2f6feb;--ok:#1f9d55;--bad:#d64545;--border:#e2e6ea;}"
    "@media (prefers-color-scheme:dark){:root{--bg:#14181d;--card:#1c2229;"
    "--text:#e6e9ec;--muted:#8a939d;--border:#2b323a;}}"
    "*{box-sizing:border-box;}"
    "body{margin:0;font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
    "background:var(--bg);color:var(--text);}"
    "header{background:var(--card);border-bottom:1px solid var(--border);"
    "padding:0.9em 1.2em;display:flex;align-items:center;gap:1em;flex-wrap:wrap;}"
    "header strong{font-size:1.05em;}"
    "nav a{color:var(--muted);text-decoration:none;margin-right:1em;font-size:0.92em;}"
    "nav a:hover{color:var(--accent);}"
    "main{max-width:720px;margin:1.5em auto;padding:0 1em;}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:10px;"
    "padding:1.1em 1.3em;margin-bottom:1.1em;}"
    "h1{font-size:1.2em;margin:0 0 0.6em;}"
    "table{border-collapse:collapse;width:100%;}"
    "th,td{padding:0.5em 0.6em;text-align:left;border-bottom:1px solid var(--border);font-size:0.93em;}"
    "th{color:var(--muted);font-weight:600;}"
    "td.num,th.num{text-align:right;}"
    ".badge{display:inline-block;padding:0.15em 0.6em;border-radius:999px;font-size:0.85em;font-weight:600;}"
    ".badge.ok{background:rgba(31,157,85,0.15);color:var(--ok);}"
    ".badge.bad{background:rgba(214,69,69,0.15);color:var(--bad);}"
    "pre{background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:0.8em;"
    "overflow-x:auto;white-space:pre-wrap;word-break:break-all;font-size:0.82em;}"
    ".muted{color:var(--muted);font-size:0.88em;}"
    "a.btn{display:inline-block;background:var(--accent);color:#fff;padding:0.5em 1em;"
    "border-radius:8px;text-decoration:none;font-size:0.92em;margin:0.2em 0.4em 0.2em 0;}"
    "a.btn.secondary{background:transparent;color:var(--accent);border:1px solid var(--accent);}";

String htmlHead(const char* title, int refreshSeconds = 0) {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    if (refreshSeconds > 0) {
        h += "<meta http-equiv='refresh' content='" + String(refreshSeconds) + "'>";
    }
    h += "<title>" + String(title) + "</title><style>" + String(PAGE_STYLE) +
         "</style></head><body>"
         "<header><strong>&#9889; EhZ Smart Meter</strong><nav>"
         "<a href='/'>Start</a><a href='/values'>Messwerte</a>"
         "<a href='/debug'>Debug</a><a href='/config'>Konfiguration</a>"
         "</nav></header><main>";
    return h;
}

String htmlFoot() {
    return "</main></body></html>";
}

void handleRoot() {
    if (iotWebConf.handleCaptivePortal()) return;  // captive portal redirect

    String page = htmlHead("EhZ Smart Meter");
    page += "<div class='card'>"
            "<h1>EhZ Smart Meter</h1>"
            "<p class='muted'>ESP8266-Auslesung f&uuml;r deinen Stromz&auml;hler "
            "&uuml;ber die optische SML-Schnittstelle.</p>"
            "<p>"
            "<a class='btn' href='/values'>Aktuelle Messwerte</a>"
            "<a class='btn secondary' href='/debug'>Debug-Informationen</a>"
            "<a class='btn secondary' href='/config'>Konfiguration</a>"
            "</p></div>";
    page += htmlFoot();
    server.send(200, "text/html; charset=utf-8", page);
}

void handleValues() {
    String page = htmlHead("EhZ - Messwerte", 15);
    page += "<div class='card'><h1>Aktuelle Messwerte</h1>";

    if (history.count() == 0) {
        page += "<p class='muted'>Noch keine Messung empfangen.</p>";
    } else {
        page += "<table><tr><th>Uptime (s)</th><th class='num'>Verbrauch (kWh)</th>"
                "<th class='num'>Einspeisung (kWh)</th><th class='num'>Leistung (W)</th></tr>";
        char row[200];
        for (int i = 0; i < history.count(); i++) {
            const MeasurementHistory<HISTORY_SIZE>::Entry& e = history.get(i);
            unsigned long uptimeS = e.uptimeMs / 1000UL;
            snprintf(row, sizeof(row),
                "<tr><td>%lu</td><td class='num'>%.3f</td><td class='num'>%.3f</td>"
                "<td class='num'>%.1f</td></tr>",
                uptimeS, e.consumedEnergy / 1000.0, e.producedEnergy / 1000.0, e.currentPower);
            page += row;
        }
        page += "</table>";
    }
    page += "</div>";

    page += "<div class='card'><h1>Stundenwerte (rollierendes 60-Min-Fenster)</h1>";
    if (hourlyHistory.count() == 0) {
        page += "<p class='muted'>Noch kein Stundenfenster abgeschlossen.</p>";
    } else {
        page += "<table><tr><th>Fensterende (Uptime, s)</th>"
                "<th class='num'>Verbrauch &Delta; (kWh)</th>"
                "<th class='num'>Einspeisung &Delta; (kWh)</th>"
                "<th class='num'>Leistung &Oslash; (W)</th>"
                "<th class='num'>Leistung Min (W)</th>"
                "<th class='num'>Leistung Max (W)</th></tr>";
        char row[260];
        for (int i = 0; i < hourlyHistory.count(); i++) {
            const HourlyHistory<HOURLY_HISTORY_SIZE>::Entry& e = hourlyHistory.get(i);
            unsigned long uptimeS = e.windowEndUptimeMs / 1000UL;
            snprintf(row, sizeof(row),
                "<tr><td>%lu</td><td class='num'>%.3f</td><td class='num'>%.3f</td>"
                "<td class='num'>%.1f</td><td class='num'>%.1f</td><td class='num'>%.1f</td></tr>",
                uptimeS, e.consumedDeltaKwh, e.producedDeltaKwh,
                e.avgPowerW, e.minPowerW, e.maxPowerW);
            page += row;
        }
        page += "</table>";
    }
    page += "</div>";

    page += htmlFoot();
    server.send(200, "text/html; charset=utf-8", page);
}

void handleDebug() {
    unsigned long now = millis();
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool mqttOk = mqttClient.connected();
    bool crcHealthy = smlParser.telegramsCrcFailed() == 0;

    String page = htmlHead("EhZ - Debug", 10);
    page += "<div class='card'><h1>Debug</h1><table>";

    page += "<tr><th>WLAN</th><td><span class='badge ";
    page += wifiOk ? "ok'>verbunden</span> " : "bad'>getrennt</span> ";
    page += "(" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm, IP " +
            WiFi.localIP().toString() + ")</td></tr>";

    page += "<tr><th>IotWebConf-Status</th><td>" +
            String(networkStateName(iotWebConf.getState())) + "</td></tr>";

    page += "<tr><th>WiFi-Statuscode</th><td>" +
            String(wifiStatusName(WiFi.status())) +
            " (Code " + String((int)WiFi.status()) + ")</td></tr>";

    page += "<tr><th>MQTT</th><td><span class='badge ";
    page += mqttOk ? "ok'>verbunden</span> " : "bad'>getrennt</span> ";
    page += "(Broker " + String(mqttServerValue) + ":" + String(mqttPortValue) +
            ", rc=" + String(mqttClient.state()) + ")</td></tr>";

    page += "<tr><th>Bytes vom Z&auml;hler</th><td>" + String(meterByteCount) +
            " gesamt, letztes vor " +
            String((now - lastMeterByteMs) / 1000UL) + "s</td></tr>";

    page += "<tr><th>Telegramme</th><td>" + String(smlParser.telegramsFound()) +
            " gefunden, <span class='badge " + (crcHealthy ? "ok" : "bad") + "'>" +
            String(smlParser.telegramsCrcFailed()) + " CRC-Fehler</span>, " +
            String(smlParser.telegramsValid()) + " g&uuml;ltig</td></tr>";

    page += "<tr><th>Freier Heap</th><td>" + String(ESP.getFreeHeap()) + " Bytes</td></tr>";
    page += "<tr><th>Uptime</th><td>" + String(now / 1000UL) + " s</td></tr>";
    page += "</table></div>";

    page += "<div class='card'><h1>Letzte Rohbytes vom Z&auml;hler (" +
            String(rawBufCount) + ")</h1><pre>";
    int start = (rawBufHead - rawBufCount + RAW_BUF_SIZE) % RAW_BUF_SIZE;
    char hexByte[4];
    for (int i = 0; i < rawBufCount; i++) {
        int idx = (start + i) % RAW_BUF_SIZE;
        snprintf(hexByte, sizeof(hexByte), "%02X ", rawBuf[idx]);
        page += hexByte;
    }
    page += "</pre></div>";

    page += "<div class='card'>";
    if (rawTcpServerRunning) {
        page += "<p>Kompletter Roh-Bytestrom per TCP: <code>" + WiFi.localIP().toString() +
                ":" + String(RAW_TCP_PORT) + "</code> "
                "(z.B. <code>tools/raw_monitor.ps1</code> oder <code>nc</code>)</p>";
    } else {
        page += "<p class='muted'>Roh-TCP-Debug ist deaktiviert. Unter "
                "<a href='/config'>/config</a> &rarr; \"Debug-Einstellungen\" aktivieren.</p>";
    }
    page += "</div>";

    page += htmlFoot();
    server.send(200, "text/html; charset=utf-8", page);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[EhZ] Starting up..."));

    // The ESP8266 SDK persists the last WiFi.begin() SSID/password to its own
    // flash sector independently of IotWebConf's EEPROM config. Disable that
    // so a network configured by an older firmware version can't linger and
    // get auto-reconnected to alongside the network IotWebConf now manages.
    WiFi.persistent(false);
    WiFi.disconnect(true);

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH);  // active-LOW: off

#ifdef USE_HARDWARE_SERIAL
    // UART0 is used for debug; switch meter to UART0 means no debug output
    // (use /debug on the web UI instead while testing this mode).
    // Reconfigure UART0 for 9600 baud, 8N1, RX-only, same invert setting as
    // the SoftwareSerial path.
    Serial.begin(9600, SERIAL_8N1, SERIAL_RX_ONLY, 1, METER_SERIAL_INVERTED);
    Serial.println(F("[EhZ] Using hardware serial at 9600 baud"));
#else
    // Larger-than-default RX buffer (64 bytes default is thin margin for a
    // ~300-byte telegram while WiFi/webserver/MQTT compete for CPU time).
    meterSerial.begin(9600, SWSERIAL_8N1, METER_RX_PIN, METER_TX_PIN,
                      METER_SERIAL_INVERTED, 256);
    Serial.println(F("[EhZ] SoftwareSerial RX=D5, TX=D6 at 9600 baud"));
#endif

    mqttGroup.addItem(&mqttServerParam);
    mqttGroup.addItem(&mqttPortParam);
    mqttGroup.addItem(&mqttUserParam);
    mqttGroup.addItem(&mqttPasswordParam);
    iotWebConf.addParameterGroup(&mqttGroup);
    publishGroup.addItem(&publishIntervalParam);
    iotWebConf.addParameterGroup(&publishGroup);
    debugGroup.addItem(&debugTcpEnabledParam);
    iotWebConf.addParameterGroup(&debugGroup);
    iotWebConf.setConfigSavedCallback(&configSaved);
    iotWebConf.init();
    applyPublishInterval();

    server.on("/", handleRoot);
    server.on("/config", [] {
        // The password field is intentionally always rendered empty (the
        // stored value is never sent to the browser); set a placeholder so
        // it's clear a password IS already saved, without revealing it.
        mqttPasswordParam.placeholder = (strlen(mqttPasswordValue) > 0)
            ? "******** (gesetzt - leer lassen zum Beibehalten)"
            : "(kein Passwort gesetzt)";

        // The built-in "AP password" field suffers the exact same "can't
        // tell if it's set" problem, but with much bigger consequences if
        // overlooked: if it ends up empty (e.g. after an EEPROM erase),
        // IotWebConf's mustStayInApMode() permanently refuses to even
        // attempt joining the configured WiFi, regardless of how correct
        // those credentials are. Make that impossible to miss.
        iotwebconf::Parameter* apPasswordParam = iotWebConf.getApPasswordParameter();
        ((IotWebConfPasswordParameter*)apPasswordParam)->placeholder =
            (strlen(apPasswordParam->valueBuffer) > 0)
                ? "******** (gesetzt - leer lassen zum Beibehalten)"
                : "WICHTIG: leer! Bitte setzen, sonst bleibt das Geraet dauerhaft im Setup-Modus.";

        iotWebConf.handleConfig();
    });
    server.on("/values", handleValues);
    server.on("/debug", handleDebug);
    server.onNotFound([]() { iotWebConf.handleNotFound(); });

    mqttClient.setBufferSize(512);
    mqttClient.setServer(mqttServerValue, atoi(mqttPortValue));

    updateRawTcpServerState();

    Serial.println(F("[EhZ] Ready."));
    Serial.println(F("[EhZ] If unconfigured, join the setup AP shown above, "
                      "then open http://192.168.4.1/ to enter WiFi/MQTT settings."));
}

void loop() {
    if (restartPending && (long)(millis() - restartAtMs) >= 0) {
        Serial.println(F("[Config] Restarting now."));
        delay(50);
        ESP.restart();
    }

    iotWebConf.doLoop();
    logWifiStateChanges();

    if (WiFi.status() == WL_CONNECTED) {
        connectMqtt();
        mqttClient.loop();
    }

    // Accept (at most one) raw TCP debug client, replacing any previous one.
    if (rawTcpServerRunning && rawTcpServer.hasClient()) {
        if (rawTcpClient) rawTcpClient.stop();
        rawTcpClient = rawTcpServer.accept();
    }

    // Read available bytes from the meter and feed them to the parser
#ifdef USE_HARDWARE_SERIAL
    while (Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();
        recordMeterByte(b);
        smlParser.addBytes(&b, 1);
        yield();  // let the WiFi/TCP stack run even during a long byte burst
    }
#else
    while (meterSerial.available()) {
        uint8_t b = (uint8_t)meterSerial.read();
        recordMeterByte(b);
        smlParser.addBytes(&b, 1);
        yield();  // let the WiFi/TCP stack run even during a long byte burst
    }
#endif

    if (smlParser.hasMeasurement()) {
        EhZMeasurement m = smlParser.getMeasurement();
        unsigned long now = millis();

        if (m.valid) {
            history.push(m, now);
            flashStatusLed(now);

            // m.consumedEnergy/producedEnergy are in Wh (native SML unit);
            // the scalar topics are documented/published in kWh.
            double consumedKwh = m.consumedEnergy / 1000.0;
            double producedKwh = m.producedEnergy / 1000.0;

            updateHourlyAggregation(now, consumedKwh, producedKwh, m.currentPower);

            if (mqttClient.connected()) {
                double diff = 0.0;
                bool publishedAny = false;

                // Extend the running time-weighted average with this
                // reading; avgPower is the average over the whole window
                // since the last power publish (or the instantaneous value
                // if this is the first sample of a fresh window). Track the
                // window's min/max/sample-count alongside it.
                double avgPower = powerAvg.sample(now, m.currentPower);
                if (powerSampleCount == 0) {
                    powerMin = m.currentPower;
                    powerMax = m.currentPower;
                } else {
                    if (m.currentPower < powerMin) powerMin = m.currentPower;
                    if (m.currentPower > powerMax) powerMax = m.currentPower;
                }
                powerSampleCount++;

                if (dbConsumed.addValue(now, consumedKwh, diff)) {
                    publishFloat(TOPIC_CONSUMED, consumedKwh);
                    publishedAny = true;
                }
                if (dbProduced.addValue(now, producedKwh, diff)) {
                    publishFloat(TOPIC_PRODUCED, producedKwh);
                    publishedAny = true;
                }
                if (dbPower.addValue(now, avgPower, diff)) {
                    publishFloat(TOPIC_POWER, avgPower);
                    publishFloat(TOPIC_POWER_MIN, powerMin);
                    publishFloat(TOPIC_POWER_MAX, powerMax);
                    publishULong(TOPIC_POWER_COUNT, powerSampleCount);
                    publishedAny = true;
                    // Start a fresh averaging window for the next interval;
                    // this reading is already the first sample of it.
                    powerAvg.reset(now, m.currentPower);
                    powerMin = m.currentPower;
                    powerMax = m.currentPower;
                    powerSampleCount = 1;
                }
                if (publishedAny) {
                    publishMeasurementJson(m, avgPower);
                    mqttClient.publish(TOPIC_UPTIME, String(now).c_str(), true);
                }
            }
        }
    }

    unsigned long now = millis();

    if (statusLedOn && (long)(now - statusLedOffAtMs) >= 0) {
        digitalWrite(STATUS_LED_PIN, HIGH);  // active-LOW: off
        statusLedOn = false;
    }

    // Periodic status print
    if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
        lastStatusMs = now;
        Serial.print(F("[EhZ] uptime="));
        Serial.print(now / 1000);
        Serial.print(F("s  WiFi="));
        Serial.print(WiFi.status() == WL_CONNECTED ? F("OK") : F("DOWN"));
        Serial.print(F("  MQTT="));
        Serial.println(mqttClient.connected() ? F("OK") : F("DOWN"));
    }
}
