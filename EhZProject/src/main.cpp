#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>

#include "SmlParser.h"
#include "EhZMeasurement.h"
#include "DeadBand.h"

// ---------------------------------------------------------------------------
// User configuration – fill in before flashing
// ---------------------------------------------------------------------------
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

#define NATS_SERVER      "192.168.111.100"
#define NATS_PORT        4222
#define NATS_CLIENT_NAME "ehz-esp8266"
// Optional credentials – leave empty strings if server has no auth
#define NATS_USER        ""
#define NATS_PASSWORD    ""

// NATS subjects (values are published as plain ASCII numbers)
#define SUBJECT_CONSUMED1  "ehz.energy.consumed1"   // kWh
#define SUBJECT_PRODUCED1  "ehz.energy.produced1"   // kWh
#define SUBJECT_CONSUMED2  "ehz.energy.consumed2"   // kWh
#define SUBJECT_PRODUCED2  "ehz.energy.produced2"   // kWh
#define SUBJECT_POWER      "ehz.power.current"      // W
#define SUBJECT_JSON       "ehz.energy.json"

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

WiFiClient natsClient;

SmlParser smlParser;

// One dead-band instance per published value
DeadBand dbConsumed1, dbProduced1, dbConsumed2, dbProduced2, dbPower;

unsigned long lastStatusMs  = 0;
unsigned long lastNatsRetry = 0;
static const unsigned long STATUS_INTERVAL_MS = 30000UL;
static const unsigned long NATS_RETRY_MS      = 5000UL;

// ---------------------------------------------------------------------------
// NATS helpers
// ---------------------------------------------------------------------------

// Read one line from the server, blocking up to timeoutMs.
// Used only during the connect handshake.
static String natsReadLine(unsigned long timeoutMs) {
    String line;
    unsigned long deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        while (natsClient.available()) {
            char c = (char)natsClient.read();
            if (c == '\r') continue;
            if (c == '\n') return line;
            line += c;
        }
        yield();
    }
    return line;
}

// Non-blocking processing of any data the server sends.
// Responds to PING with PONG and logs -ERR messages.
void natsLoop() {
    if (!natsClient.connected()) return;
    static String incoming;   // line accumulator – persists across calls
    while (natsClient.available()) {
        char c = (char)natsClient.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (incoming.startsWith(F("PING"))) {
                natsClient.print(F("PONG\r\n"));
            } else if (incoming.startsWith(F("-ERR"))) {
                Serial.print(F("[NATS] Error: "));
                Serial.println(incoming);
            }
            incoming = "";
        } else {
            incoming += c;
        }
    }
}

// Publish a raw byte payload to a NATS subject.
void publishNats(const char* subject, const char* payload) {
    if (!natsClient.connected()) return;
    int len = strlen(payload);
    natsClient.print(F("PUB "));
    natsClient.print(subject);
    natsClient.print(' ');
    natsClient.print(len);
    natsClient.print(F("\r\n"));
    natsClient.write(reinterpret_cast<const uint8_t*>(payload), len);
    natsClient.print(F("\r\n"));
}

// Format a double as ASCII and publish it.
void publishFloat(const char* subject, double value) {
    char buf[24];
    dtostrf(value, 1, 4, buf);
    publishNats(subject, buf);
}

// Build and publish the JSON measurement envelope.
void publishMeasurementJson(const EhZMeasurement& m) {
    char payload[220];
    const char* t = "{\"consumedEnergy1\":%.4f,"
                    "\"consumedEnergy2\":%.4f,"
                    "\"producedEnergy1\":%.4f,"
                    "\"producedEnergy2\":%.4f,"
                    "\"currentPower\":%.1f,"
                    "\"valid\":%s}";
    // Energies are published as kWh in JSON (same unit as scalar subjects)
    int n = snprintf(payload, sizeof(payload), t,
        m.consumedEnergy1 / 1000.0,
        m.consumedEnergy2 / 1000.0,
        m.producedEnergy1 / 1000.0,
        m.producedEnergy2 / 1000.0,
        m.currentPower,
        m.valid ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(payload)) {
        Serial.println(F("[NATS] JSON payload truncated/invalid"));
        return;
    }
    publishNats(SUBJECT_JSON, payload);
}

bool connectNats() {
    if (natsClient.connected()) return true;

    unsigned long now = millis();
    if (now - lastNatsRetry < NATS_RETRY_MS) return false;
    lastNatsRetry = now;

    Serial.print(F("[NATS] Connecting to "));
    Serial.print(NATS_SERVER);
    Serial.print(':');
    Serial.print(NATS_PORT);
    Serial.print(F(" ... "));

    if (!natsClient.connect(NATS_SERVER, NATS_PORT)) {
        Serial.println(F("failed."));
        return false;
    }

    // The server sends an INFO line immediately upon connection.
    String info = natsReadLine(2000);
    if (!info.startsWith(F("INFO"))) {
        Serial.println(F("unexpected response (expected INFO)."));
        natsClient.stop();
        return false;
    }

    // Send CONNECT with verbose:false so the server sends no +OK reply.
    // Note: NATS_USER and NATS_PASSWORD must not contain special JSON characters
    // (quotes, backslashes) as no escaping is performed.
    char connectMsg[320];
    int n;
    if (strlen(NATS_USER) > 0) {
        n = snprintf(connectMsg, sizeof(connectMsg),
            "CONNECT {\"verbose\":false,\"pedantic\":false,"
            "\"tls_required\":false,\"name\":\"%s\","
            "\"user\":\"%s\",\"pass\":\"%s\","
            "\"lang\":\"c\",\"version\":\"1.0.0\"}\r\n",
            NATS_CLIENT_NAME, NATS_USER, NATS_PASSWORD);
    } else {
        n = snprintf(connectMsg, sizeof(connectMsg),
            "CONNECT {\"verbose\":false,\"pedantic\":false,"
            "\"tls_required\":false,\"name\":\"%s\","
            "\"lang\":\"c\",\"version\":\"1.0.0\"}\r\n",
            NATS_CLIENT_NAME);
    }
    if (n <= 0 || n >= (int)sizeof(connectMsg)) {
        Serial.println(F("CONNECT message truncated – check credential lengths."));
        natsClient.stop();
        return false;
    }
    natsClient.print(connectMsg);

    Serial.println(F("connected."));
    return true;
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
    // UART0 is used for the meter; no debug output is available in this mode.
    Serial.begin(9600, SERIAL_8N1);
    Serial.println(F("[EhZ] Using hardware serial at 9600 baud"));
#else
    meterSerial.begin(9600);
    Serial.println(F("[EhZ] SoftwareSerial RX=D5, TX=D6 at 9600 baud"));
#endif

    connectWifi();
    connectNats();

    Serial.println(F("[EhZ] Ready."));
}

void loop() {
    connectWifi();
    connectNats();
    natsLoop();

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
        if (m.valid && natsClient.connected()) {
            unsigned long now = millis();
            double diff = 0.0;
            bool publishedAny = false;

            if (dbConsumed1.addValue(now, m.consumedEnergy1, diff)) {
                publishFloat(SUBJECT_CONSUMED1, m.consumedEnergy1 / 1000.0);
                publishedAny = true;
            }
            if (dbProduced1.addValue(now, m.producedEnergy1, diff)) {
                publishFloat(SUBJECT_PRODUCED1, m.producedEnergy1 / 1000.0);
                publishedAny = true;
            }
            if (dbConsumed2.addValue(now, m.consumedEnergy2, diff)) {
                publishFloat(SUBJECT_CONSUMED2, m.consumedEnergy2 / 1000.0);
                publishedAny = true;
            }
            if (dbProduced2.addValue(now, m.producedEnergy2, diff)) {
                publishFloat(SUBJECT_PRODUCED2, m.producedEnergy2 / 1000.0);
                publishedAny = true;
            }
            if (dbPower.addValue(now, m.currentPower, diff)) {
                publishFloat(SUBJECT_POWER, m.currentPower);
                publishedAny = true;
            }
            if (publishedAny) {
                publishMeasurementJson(m);
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
        Serial.print(F("  NATS="));
        Serial.println(natsClient.connected() ? F("OK") : F("DOWN"));
    }
}
