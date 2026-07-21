#include <Arduino.h>
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
#define CONFIG_VERSION "ehz1"  // bump when the parameter set below changes

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

// MQTT topics (values are published as plain ASCII numbers)
#define TOPIC_STATUS     "ehz/status"
#define TOPIC_CONSUMED   "ehz/energy/consumed"   // kWh
#define TOPIC_PRODUCED   "ehz/energy/produced"   // kWh
#define TOPIC_POWER      "ehz/power/current"     // W
#define TOPIC_JSON       "ehz/energy/json"
#define TOPIC_UPTIME     "ehz/uptime/ms" // ms since epoch of the measurement

// ---------------------------------------------------------------------------
// SoftwareSerial pins for the EHZ meter (D5 = GPIO14 RX, D6 = GPIO12 TX)
// Change to USE_HARDWARE_SERIAL if you want UART0 for the meter and disable
// the debug Serial output.
// ---------------------------------------------------------------------------
// #define USE_HARDWARE_SERIAL
#define METER_RX_PIN  14  // D5
#define METER_TX_PIN  12  // D6

// Many DIY IR read heads (phototransistor + pull-up) output an inverted
// signal relative to normal TTL UART levels. If /debug shows bytes arriving
// but never framing into a valid telegram (no 0x1B x4 start sequence, lots
// of FF/FE runs in the hex dump), flip this.
#define METER_SERIAL_INVERTED false

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

unsigned long lastStatusMs  = 0;
unsigned long lastMqttRetry = 0;
static const unsigned long STATUS_INTERVAL_MS = 30000UL;
static const unsigned long MQTT_RETRY_MS      = 5000UL;

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
void publishFloat(const char* topic, double value) {
    char buf[24];
    dtostrf(value, 1, 4, buf);
    if (!mqttClient.publish(topic, buf, /*retained=*/true)) {
        Serial.print(F("[MQTT] publish failed for topic: "));
        Serial.println(topic);
    }
}

void publishMeasurementJson(const EhZMeasurement& m) {
    char payload[220];

    const char* t = "{\"consumedEnergy\":%.4f,"
               "\"producedEnergy\":%.4f,"
               "\"currentPower\":%.1f,"
               "\"valid\":%s}";
    // m.consumedEnergy/producedEnergy are in Wh (native SML unit); convert to
    // kWh here to match the scalar topics.
    int n = snprintf(payload, sizeof(payload), t,
        m.consumedEnergy / 1000.0,
        m.producedEnergy / 1000.0,
        m.currentPower,
        m.valid ? "true" : "false");

    if (n <= 0 || n >= (int)sizeof(payload)) {
        Serial.println(F("[MQTT] JSON payload truncated/invalid"));
        return;
    }
    if (!mqttClient.publish(TOPIC_JSON, payload, true)) {
        Serial.println(F("[MQTT] publish failed for JSON topic"));
    }
}

// Called by IotWebConf right after new settings are persisted to EEPROM.
void configSaved() {
    Serial.println(F("[Config] Saved - reconnecting MQTT with new settings"));
    mqttClient.disconnect();
    mqttClient.setServer(mqttServerValue, atoi(mqttPortValue));
    updateRawTcpServerState();
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
// Web pages
// ---------------------------------------------------------------------------
void handleRoot() {
    if (iotWebConf.handleCaptivePortal()) return;  // captive portal redirect

    String page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>EhZ Smart Meter</title></head><body>"
        "<h1>EhZ Smart Meter</h1>"
        "<p><a href='/values'>Aktuelle Messwerte</a></p>"
        "<p><a href='/debug'>Debug-Informationen</a></p>"
        "<p><a href='config'>Konfiguration (WLAN / MQTT)</a></p>"
        "</body></html>";
    server.send(200, "text/html; charset=utf-8", page);
}

void handleValues() {
    String page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='15'>"
        "<title>EhZ - Messwerte</title>"
        "<style>"
        "body{font-family:sans-serif;margin:1.5em;}"
        "table{border-collapse:collapse;}"
        "th,td{border:1px solid #ccc;padding:4px 10px;text-align:right;}"
        "th{background:#eee;}"
        "</style></head><body>"
        "<h1>Aktuelle Messwerte</h1>"
        "<p><a href='/'>zur&uuml;ck</a></p>";

    if (history.count() == 0) {
        page += "<p>Noch keine Messung empfangen.</p>";
    } else {
        page += "<table><tr><th>Alter (s)</th><th>Verbrauch (kWh)</th>"
                "<th>Einspeisung (kWh)</th><th>Leistung (W)</th></tr>";
        unsigned long now = millis();
        char row[160];
        for (int i = 0; i < history.count(); i++) {
            const MeasurementHistory<HISTORY_SIZE>::Entry& e = history.get(i);
            unsigned long ageS = (now - e.uptimeMs) / 1000UL;
            snprintf(row, sizeof(row),
                "<tr><td>%lu</td><td>%.3f</td><td>%.3f</td><td>%.1f</td></tr>",
                ageS, e.consumedEnergy / 1000.0, e.producedEnergy / 1000.0, e.currentPower);
            page += row;
        }
        page += "</table>";
    }
    page += "</body></html>";
    server.send(200, "text/html; charset=utf-8", page);
}

void handleDebug() {
    unsigned long now = millis();

    String page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='10'>"
        "<title>EhZ - Debug</title>"
        "<style>"
        "body{font-family:sans-serif;margin:1.5em;}"
        "table{border-collapse:collapse;margin-bottom:1em;}"
        "th,td{border:1px solid #ccc;padding:4px 10px;text-align:left;}"
        "th{background:#eee;}"
        "pre{background:#f5f5f5;padding:0.8em;overflow-x:auto;white-space:pre-wrap;word-break:break-all;}"
        "</style></head><body>"
        "<h1>Debug</h1>"
        "<p><a href='/'>zur&uuml;ck</a></p>"
        "<table>";

    page += "<tr><th>WLAN</th><td>";
    page += (WiFi.status() == WL_CONNECTED) ? "verbunden" : "getrennt";
    page += " (" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm, IP " +
            WiFi.localIP().toString() + ")</td></tr>";

    page += "<tr><th>MQTT</th><td>";
    page += mqttClient.connected() ? "verbunden" : "getrennt";
    page += " (Broker " + String(mqttServerValue) + ":" + String(mqttPortValue) +
            ", rc=" + String(mqttClient.state()) + ")</td></tr>";

    page += "<tr><th>Bytes vom Z&auml;hler</th><td>" + String(meterByteCount) +
            " gesamt, letztes vor " +
            String((now - lastMeterByteMs) / 1000UL) + "s</td></tr>";

    page += "<tr><th>Telegramme</th><td>" + String(smlParser.telegramsFound()) +
            " gefunden, " + String(smlParser.telegramsValid()) + " g&uuml;ltig</td></tr>";

    page += "<tr><th>Freier Heap</th><td>" + String(ESP.getFreeHeap()) + " Bytes</td></tr>";
    page += "<tr><th>Uptime</th><td>" + String(now / 1000UL) + " s</td></tr>";
    page += "</table>";

    page += "<h2>Letzte Rohbytes vom Z&auml;hler (" + String(rawBufCount) + ")</h2><pre>";
    int start = (rawBufHead - rawBufCount + RAW_BUF_SIZE) % RAW_BUF_SIZE;
    char hexByte[4];
    for (int i = 0; i < rawBufCount; i++) {
        int idx = (start + i) % RAW_BUF_SIZE;
        snprintf(hexByte, sizeof(hexByte), "%02X ", rawBuf[idx]);
        page += hexByte;
    }
    page += "</pre>";

    if (rawTcpServerRunning) {
        page += "<p>Kompletter Roh-Bytestrom per TCP: <code>" + WiFi.localIP().toString() +
                ":" + String(RAW_TCP_PORT) + "</code> "
                "(z.B. <code>tools/raw_monitor.ps1</code> oder <code>nc</code>)</p>";
    } else {
        page += "<p>Roh-TCP-Debug ist deaktiviert. Unter "
                "<a href='/config'>/config</a> &rarr; \"Debug-Einstellungen\" aktivieren.</p>";
    }

    page += "</body></html>";
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

#ifdef USE_HARDWARE_SERIAL
    // UART0 is used for debug; switch meter to UART0 means no debug output
    // (use /debug on the web UI instead while testing this mode).
    // Reconfigure UART0 for 9600 baud, 8N1, RX-only, same invert setting as
    // the SoftwareSerial path.
    Serial.begin(9600, SERIAL_8N1, SERIAL_RX_ONLY, 1, METER_SERIAL_INVERTED);
    Serial.println(F("[EhZ] Using hardware serial at 9600 baud"));
#else
    meterSerial.begin(9600);
    Serial.println(F("[EhZ] SoftwareSerial RX=D5, TX=D6 at 9600 baud"));
#endif

    mqttGroup.addItem(&mqttServerParam);
    mqttGroup.addItem(&mqttPortParam);
    mqttGroup.addItem(&mqttUserParam);
    mqttGroup.addItem(&mqttPasswordParam);
    iotWebConf.addParameterGroup(&mqttGroup);
    debugGroup.addItem(&debugTcpEnabledParam);
    iotWebConf.addParameterGroup(&debugGroup);
    iotWebConf.setConfigSavedCallback(&configSaved);
    iotWebConf.init();

    server.on("/", handleRoot);
    server.on("/config", [] { iotWebConf.handleConfig(); });
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
    iotWebConf.doLoop();

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
        }

        if (m.valid && mqttClient.connected()) {
            double diff = 0.0;
            bool publishedAny = false;

            // m.consumedEnergy/producedEnergy are in Wh (native SML unit);
            // the scalar topics are documented/published in kWh.
            double consumedKwh = m.consumedEnergy / 1000.0;
            double producedKwh = m.producedEnergy / 1000.0;

            if (dbConsumed.addValue(now, consumedKwh, diff)) {
                publishFloat(TOPIC_CONSUMED, consumedKwh);
                publishedAny = true;
            }
            if (dbProduced.addValue(now, producedKwh, diff)) {
                publishFloat(TOPIC_PRODUCED, producedKwh);
                publishedAny = true;
            }
            if (dbPower.addValue(now, m.currentPower, diff)) {
                publishFloat(TOPIC_POWER, m.currentPower);
                publishedAny = true;
            }
            if (publishedAny) {
                publishMeasurementJson(m);
                mqttClient.publish(TOPIC_UPTIME, String(now).c_str(), true);
            }
        }
    }

    // Periodic status print
    unsigned long now = millis();
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
