#pragma once

#include <Arduino.h>
#include <EEPROM.h>

// Magic number stored in the first 4 bytes of EEPROM to detect a valid config.
static constexpr uint32_t MQTT_CONFIG_MAGIC = 0xEA5C0001u;

// Persistent MQTT configuration stored in EEPROM.
struct MqttConfig {
    uint32_t magic;
    char     host[64];
    uint16_t port;
    char     user[32];
    char     password[64];
};

// Reads config from EEPROM.  Returns true when the stored magic matches and
// the config is considered valid; returns false when EEPROM has never been
// written (or was erased), in which case the caller should fill in defaults.
inline bool loadMqttConfig(MqttConfig& cfg) {
    EEPROM.begin(sizeof(MqttConfig));
    EEPROM.get(0, cfg);
    EEPROM.end();
    return cfg.magic == MQTT_CONFIG_MAGIC;
}

// Writes config to EEPROM and commits it to flash.
inline void saveMqttConfig(MqttConfig& cfg) {
    cfg.magic = MQTT_CONFIG_MAGIC;
    EEPROM.begin(sizeof(MqttConfig));
    EEPROM.put(0, cfg);
    EEPROM.commit();
    EEPROM.end();
}
