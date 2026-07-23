#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

#include "SmlParser.h"
#include "EhZMeasurement.h"
#include "DeadBand.h"
#include "MqttConfig.h"

// ---------------------------------------------------------------------------
// User configuration – fill in before flashing
// ---------------------------------------------------------------------------
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// MQTT defaults used when EEPROM has never been written.
// After the first boot the values can be changed via the web UI at http://<ip>/config
// and are then persisted in EEPROM across resets.
#define MQTT_BROKER_DEFAULT  "192.168.111.47"
#define MQTT_PORT_DEFAULT    1883
#define MQTT_USER_DEFAULT    ""
#define MQTT_PASS_DEFAULT    ""

#define MQTT_CLIENT_ID  "ehz-esp8266"

// MQTT topics (values are published as plain ASCII numbers)
#define TOPIC_STATUS     "ehz/status"
#define TOPIC_CONSUMED1  "ehz/energy/consumed1"   // kWh
#define TOPIC_PRODUCED1  "ehz/energy/produced1"   // kWh
#define TOPIC_CONSUMED2  "ehz/energy/consumed2"   // kWh
#define TOPIC_PRODUCED2  "ehz/energy/produced2"   // kWh
#define TOPIC_POWER      "ehz/power/current"      // W
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

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
#ifndef USE_HARDWARE_SERIAL
SoftwareSerial meterSerial(METER_RX_PIN, METER_TX_PIN);
#endif

MqttConfig       mqttConfig;
WiFiClient       wifiClient;
PubSubClient     mqttClient(wifiClient);
ESP8266WebServer webServer(80);

SmlParser smlParser;

// Last successfully decoded measurement (shown on web dashboard)
EhZMeasurement lastMeas;
bool           lastMeasValid = false;

// One dead-band instance per published value
DeadBand dbConsumed1, dbProduced1, dbConsumed2, dbProduced2, dbPower;

unsigned long lastStatusMs  = 0;
unsigned long lastMqttRetry = 0;
static const unsigned long STATUS_INTERVAL_MS = 30000UL;
static const unsigned long MQTT_RETRY_MS      = 5000UL;

// ---------------------------------------------------------------------------
// HTML helpers
// ---------------------------------------------------------------------------

// Escape special HTML characters to prevent XSS / broken markup when
// config values are inserted into HTML attribute values.
static String htmlEscape(const char* s) {
    String out;
    out.reserve(strlen(s) * 6 + 1);
    while (*s) {
        switch (*s) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *s;
        }
        ++s;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Web server handlers
// ---------------------------------------------------------------------------

// GET / — live dashboard showing current meter readings + uptime
void handleRoot() {
    unsigned long nowMs     = millis();
    unsigned long uptimeSec = nowMs / 1000UL;
    unsigned long h = uptimeSec / 3600UL;
    unsigned long m = (uptimeSec % 3600UL) / 60UL;
    unsigned long s = uptimeSec % 60UL;

    String html;
    html.reserve(2048);
    html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<title>EhZ Monitor</title>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<meta http-equiv='refresh' content='10'>"
              "<style>"
              "body{font-family:sans-serif;margin:20px;background:#f0f4f8}"
              "h1{color:#2c3e50}"
              "table{border-collapse:collapse;width:100%;max-width:520px}"
              "td,th{border:1px solid #bbb;padding:8px 12px;text-align:left}"
              "th{background:#2980b9;color:#fff}"
              "tr:nth-child(even){background:#dce8fc}"
              ".btn{display:inline-block;margin-top:16px;padding:9px 18px;"
              "background:#2980b9;color:#fff;text-decoration:none;border-radius:4px}"
              "</style></head><body>"
              "<h1>EhZ Monitor</h1>"
              "<table><tr><th>Metric</th><th>Value</th></tr>");

    char buf[24];

    dtostrf(lastMeas.consumedEnergy1, 1, 4, buf);
    html += F("<tr><td>Consumed Energy 1</td><td>"); html += buf; html += F(" kWh</td></tr>");

    dtostrf(lastMeas.producedEnergy1, 1, 4, buf);
    html += F("<tr><td>Produced Energy 1</td><td>"); html += buf; html += F(" kWh</td></tr>");

    dtostrf(lastMeas.consumedEnergy2, 1, 4, buf);
    html += F("<tr><td>Consumed Energy 2</td><td>"); html += buf; html += F(" kWh</td></tr>");

    dtostrf(lastMeas.producedEnergy2, 1, 4, buf);
    html += F("<tr><td>Produced Energy 2</td><td>"); html += buf; html += F(" kWh</td></tr>");

    dtostrf(lastMeas.currentPower, 1, 1, buf);
    html += F("<tr><td>Current Power</td><td>"); html += buf; html += F(" W</td></tr>");

    html += F("<tr><td>Uptime</td><td>");
    html += h; html += F("h "); html += m; html += F("m "); html += s; html += F("s</td></tr>");

    html += F("<tr><td>MQTT</td><td>");
    html += mqttClient.connected() ? F("Connected") : F("Disconnected");
    html += F("</td></tr>");

    html += F("<tr><td>Measurement</td><td>");
    html += lastMeasValid ? F("Valid") : F("Waiting&hellip;");
    html += F("</td></tr>");

    html += F("</table>"
              "<a href='/config' class='btn'>MQTT Settings</a>"
              "</body></html>");

    webServer.send(200, "text/html", html);
}

// GET /config — MQTT settings form pre-filled from current config
void handleConfig() {
    String html;
    html.reserve(1600);
    html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<title>EhZ - MQTT Settings</title>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<style>"
              "body{font-family:sans-serif;margin:20px;background:#f0f4f8}"
              "h1{color:#2c3e50}form{max-width:400px}"
              "label{display:block;margin-top:12px;font-weight:bold;color:#2c3e50}"
              "input{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;"
              "border:1px solid #bbb;border-radius:4px;font-size:14px}"
              ".save{display:inline-block;margin-top:16px;padding:9px 18px;"
              "background:#27ae60;color:#fff;border:none;border-radius:4px;"
              "cursor:pointer;font-size:14px}"
              ".back{display:inline-block;margin-top:16px;margin-left:8px;padding:9px 18px;"
              "background:#7f8c8d;color:#fff;text-decoration:none;border-radius:4px}"
              "</style></head><body>"
              "<h1>MQTT Settings</h1>"
              "<form method='POST' action='/config'>"
              "<label>Host</label>"
              "<input name='host' value='");
    html += htmlEscape(mqttConfig.host);
    html += F("'><label>Port</label>"
              "<input name='port' type='number' min='1' max='65535' value='");
    html += mqttConfig.port;
    html += F("'><label>User</label>"
              "<input name='user' value='");
    html += htmlEscape(mqttConfig.user);
    html += F("'><label>Password</label>"
              "<input name='password' type='password' value='");
    html += htmlEscape(mqttConfig.password);
    html += F("'><br>"
              "<button class='save' type='submit'>Save &amp; Apply</button>"
              "<a href='/' class='back'>Cancel</a>"
              "</form></body></html>");

    webServer.send(200, "text/html", html);
}

// POST /config — persist submitted MQTT settings and reconnect
void handleConfigSave() {
    if (webServer.hasArg("host")) {
        webServer.arg("host").toCharArray(mqttConfig.host, sizeof(mqttConfig.host));
        mqttConfig.host[sizeof(mqttConfig.host) - 1] = '\0';
    }
    if (webServer.hasArg("port")) {
        int p = webServer.arg("port").toInt();
        if (p > 0 && p <= 65535) mqttConfig.port = (uint16_t)p;
    }
    if (webServer.hasArg("user")) {
        webServer.arg("user").toCharArray(mqttConfig.user, sizeof(mqttConfig.user));
        mqttConfig.user[sizeof(mqttConfig.user) - 1] = '\0';
    }
    if (webServer.hasArg("password")) {
        webServer.arg("password").toCharArray(mqttConfig.password, sizeof(mqttConfig.password));
        mqttConfig.password[sizeof(mqttConfig.password) - 1] = '\0';
    }

    saveMqttConfig(mqttConfig);
    Serial.println(F("[Web] MQTT config saved to EEPROM."));

    // Apply new broker settings and force an immediate reconnect.
    mqttClient.disconnect();
    mqttClient.setServer(mqttConfig.host, mqttConfig.port);
    lastMqttRetry = 0;

    webServer.sendHeader("Location", "/");
    webServer.send(303, "text/plain", "Redirecting...");
}

// ---------------------------------------------------------------------------
// MQTT helpers
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

    const char* t = "{\"consumedEnergy1\":%.4f,"
               "\"consumedEnergy2\":%.4f,"
               "\"producedEnergy1\":%.4f,"
               "\"producedEnergy2\":%.4f,"
               "\"currentPower\":%.1f,"  
               "\"valid\":%s}";
    // Energies are published as kWh in JSON (same unit as scalar topics)
    int n = snprintf(payload, sizeof(payload), t,
        m.consumedEnergy1,
        m.consumedEnergy2,
        m.producedEnergy1,
        m.producedEnergy2,
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

bool connectMqtt() {
    if (mqttClient.connected()) return true;

    unsigned long now = millis();
    if (now - lastMqttRetry < MQTT_RETRY_MS) return false;
    lastMqttRetry = now;

    Serial.print(F("[MQTT] Connecting to "));
    Serial.print(mqttConfig.host);
    Serial.print(F(" ... "));

    bool ok;
    if (strlen(mqttConfig.user) > 0) {
        ok = mqttClient.connect(
            MQTT_CLIENT_ID,
            mqttConfig.user,
            mqttConfig.password,
            TOPIC_STATUS,   // will topic
            1,              // will QoS
            true,           // will retained
            "stopped"       // will payload
        );
    } else {
        ok = mqttClient.connect(
            MQTT_CLIENT_ID,
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

void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.print(F("[WiFi] Connecting to "));
    Serial.print(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.print(F(" IP: "));
    Serial.println(WiFi.localIP());
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("\n[EhZ] Starting up..."));

#ifdef USE_HARDWARE_SERIAL
    // UART0 is used for debug; switch meter to UART0 means no debug output.
    // Reconfigure UART0 for 9600 baud, 8N1.
    Serial.begin(9600, SERIAL_8N1);
    Serial.println(F("[EhZ] Using hardware serial at 9600 baud"));
#else
    meterSerial.begin(9600);
    Serial.println(F("[EhZ] SoftwareSerial RX=D5, TX=D6 at 9600 baud"));
#endif

    // Load MQTT config from EEPROM; fall back to compile-time defaults when
    // EEPROM has never been written.
    if (!loadMqttConfig(mqttConfig)) {
        Serial.println(F("[EEPROM] No valid config found – using defaults."));
        strncpy(mqttConfig.host, MQTT_BROKER_DEFAULT, sizeof(mqttConfig.host) - 1);
        mqttConfig.host[sizeof(mqttConfig.host) - 1] = '\0';
        mqttConfig.port = MQTT_PORT_DEFAULT;
        strncpy(mqttConfig.user, MQTT_USER_DEFAULT, sizeof(mqttConfig.user) - 1);
        mqttConfig.user[sizeof(mqttConfig.user) - 1] = '\0';
        strncpy(mqttConfig.password, MQTT_PASS_DEFAULT, sizeof(mqttConfig.password) - 1);
        mqttConfig.password[sizeof(mqttConfig.password) - 1] = '\0';
    } else {
        Serial.println(F("[EEPROM] MQTT config loaded."));
    }

    connectWifi();

    mqttClient.setServer(mqttConfig.host, mqttConfig.port);
    mqttClient.setBufferSize(512);
    connectMqtt();

    // Web server routes
    webServer.on("/",       HTTP_GET,  handleRoot);
    webServer.on("/config", HTTP_GET,  handleConfig);
    webServer.on("/config", HTTP_POST, handleConfigSave);
    webServer.begin();

    Serial.print(F("[Web] Server started at http://"));
    Serial.println(WiFi.localIP());
    Serial.println(F("[EhZ] Ready."));
}

void loop() {
    connectWifi();
    connectMqtt();
    mqttClient.loop();
    webServer.handleClient();

    // Read available bytes from the meter and feed them to the parser
#ifdef USE_HARDWARE_SERIAL
    while (Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();
        smlParser.addBytes(&b, 1);
    }
#else
    while (meterSerial.available()) {
        uint8_t b = (uint8_t)meterSerial.read();
        smlParser.addBytes(&b, 1);
    }
#endif

    if (smlParser.hasMeasurement()) {
        EhZMeasurement m = smlParser.getMeasurement();
        if (m.valid) {
            lastMeas      = m;
            lastMeasValid = true;
        }
        if (m.valid && mqttClient.connected()) {
            unsigned long now = millis();
            double diff = 0.0;
            bool publishedAny = false;

            if (dbConsumed1.addValue(now, m.consumedEnergy1, diff)) {
                publishFloat(TOPIC_CONSUMED1, m.consumedEnergy1);
                publishedAny = true;
            }
            if (dbProduced1.addValue(now, m.producedEnergy1, diff)) {
                publishFloat(TOPIC_PRODUCED1, m.producedEnergy1);
                publishedAny = true;
            }
            if (dbConsumed2.addValue(now, m.consumedEnergy2, diff)) {
                publishFloat(TOPIC_CONSUMED2, m.consumedEnergy2);
                publishedAny = true;
            }
            if (dbProduced2.addValue(now, m.producedEnergy2, diff)) {
                publishFloat(TOPIC_PRODUCED2, m.producedEnergy2);
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
