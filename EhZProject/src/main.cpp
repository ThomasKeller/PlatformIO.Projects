#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

#include "SmlParser.h"
#include "EhZMeasurement.h"
#include "DeadBand.h"

// ---------------------------------------------------------------------------
// User configuration – fill in before flashing
// ---------------------------------------------------------------------------
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

#define MQTT_BROKER     "192.168.111.100"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "ehz-esp8266"
// Optional credentials – leave empty strings if broker has no auth
#define MQTT_USER       ""
#define MQTT_PASSWORD   ""

// MQTT topics (values are published as plain ASCII numbers)
#define TOPIC_CONSUMED1  "ehz/energy/consumed1"   // kWh
#define TOPIC_PRODUCED1  "ehz/energy/produced1"   // kWh
#define TOPIC_CONSUMED2  "ehz/energy/consumed2"   // kWh
#define TOPIC_PRODUCED2  "ehz/energy/produced2"   // kWh
#define TOPIC_POWER      "ehz/power/current"      // W
#define TOPIC_JSON       "ehz/energy/json"

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

WiFiClient    wifiClient;
PubSubClient  mqttClient(wifiClient);

SmlParser smlParser;

// One dead-band instance per published value
DeadBand dbConsumed1, dbProduced1, dbConsumed2, dbProduced2, dbPower;

unsigned long lastStatusMs  = 0;
unsigned long lastMqttRetry = 0;
static const unsigned long STATUS_INTERVAL_MS = 30000UL;
static const unsigned long MQTT_RETRY_MS      = 5000UL;

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

    const char* t = "{\"consumedEnergy1\":%.4f,"
               "\"consumedEnergy2\":%.4f,"
               "\"producedEnergy1\":%.4f,"
               "\"producedEnergy2\":%.4f,"
               "\"currentPower\":%.1f,"  
               "\"valid\":%s}";
    // Energies are published as kWh in JSON (same unit as scalar topics)
    int n = snprintf(payload, sizeof(payload), t,
        m.consumedEnergy1 / 1000.0,
        m.consumedEnergy2 / 1000.0,
        m.producedEnergy1 / 1000.0,
        m.producedEnergy2 / 1000.0,
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
    Serial.print(MQTT_BROKER);
    Serial.print(F(" ... "));

    bool ok;
    if (strlen(MQTT_USER) > 0) {
        ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
    } else {
        ok = mqttClient.connect(MQTT_CLIENT_ID);
    }

    if (ok) {
        Serial.println(F("connected."));
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

    connectWifi();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setBufferSize(512);
    connectMqtt();

    Serial.println(F("[EhZ] Ready."));
}

void loop() {
    connectWifi();
    connectMqtt();
    mqttClient.loop();

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
        if (m.valid && mqttClient.connected()) {
            unsigned long now = millis();
            double diff = 0.0;
            bool publishedAny = false;

            if (dbConsumed1.addValue(now, m.consumedEnergy1, diff)) {
                publishFloat(TOPIC_CONSUMED1, m.consumedEnergy1 / 1000.0);
                publishedAny = true;
            }
            if (dbProduced1.addValue(now, m.producedEnergy1, diff)) {
                publishFloat(TOPIC_PRODUCED1, m.producedEnergy1 / 1000.0);
                publishedAny = true;
            }
            if (dbConsumed2.addValue(now, m.consumedEnergy2, diff)) {
                publishFloat(TOPIC_CONSUMED2, m.consumedEnergy2 / 1000.0);
                publishedAny = true;
            }
            if (dbProduced2.addValue(now, m.producedEnergy2, diff)) {
                publishFloat(TOPIC_PRODUCED2, m.producedEnergy2 / 1000.0);
                publishedAny = true;
            }
            if (dbPower.addValue(now, m.currentPower, diff)) {
                publishFloat(TOPIC_POWER, m.currentPower);
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
        Serial.print(F("  MQTT="));
        Serial.println(mqttClient.connected() ? F("OK") : F("DOWN"));
    }
}
