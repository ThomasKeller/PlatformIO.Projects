# Copilot Instructions

## Repository Overview

This repository contains [PlatformIO](https://docs.platformio.org) embedded firmware projects targeting ESP8266 microcontrollers using the Arduino framework. Each subdirectory is a self-contained PlatformIO project.

## Project Structure

```
PlatformIO.Projects/
├── EhZProject/          # ESP8266 firmware to read SML data from an EHZ electricity meter and publish via MQTT
│   ├── include/         # Header files (SmlParser.h, EhZMeasurement.h, DeadBand.h)
│   ├── lib/             # Project-local libraries
│   ├── src/main.cpp     # Application entry point (setup/loop)
│   └── platformio.ini   # Board: nodemcuv2 (ESP8266), framework: arduino
└── TemplateProject/     # Bare-bones PlatformIO starter for new ESP8266 projects
    ├── include/
    ├── lib/
    ├── src/
    ├── test/
    └── platformio.ini
```

## Technology Stack

- **Framework**: Arduino (C++17, `-std=gnu++17`)
- **Platform**: Espressif ESP8266 (`espressif8266`)
- **Boards**: `nodemcuv2`, `esp01`, `thing` (SparkFun ESP8266 Thing)
- **Key libraries**: `knolleary/PubSubClient` (MQTT), `ESP8266WiFi`, `SoftwareSerial`
- **Build system**: PlatformIO CLI / IDE

## Coding Conventions

- C++17 is required; use `inline constexpr` for static array members in header-only classes.
- Header files use `#pragma once` instead of include guards.
- Arduino-style `setup()` / `loop()` entry points in `src/main.cpp`.
- Use `F()` macro for string literals to store them in flash (PROGMEM) and save RAM.
- Prefer `uint8_t` / `int64_t` from `<stdint.h>` for fixed-width integer types.
- Classes ported from C# equivalents preserve the same algorithmic logic and naming where practical.
- Secrets (Wi-Fi credentials, MQTT broker address) are `#define` constants at the top of `main.cpp` and **must never be committed with real values** — always use placeholder strings like `"YOUR_WIFI_SSID"`.

## Building and Flashing

Use the [PlatformIO CLI](https://docs.platformio.org/en/latest/core/index.html) from inside a project directory:

```bash
# Build
pio run

# Flash to connected board
pio run --target upload

# Open serial monitor
pio device monitor --baud 115200
```

The build output (`.pio/`) is git-ignored.

## Adding a New Project

1. Copy `TemplateProject/` to a new directory.
2. Update `platformio.ini` with the correct board and dependencies.
3. Implement `setup()` and `loop()` in `src/main.cpp`.
4. Add a `README.md` describing the project purpose and wiring.

## EhZProject Domain Details

- **SML (Smart Message Language)**: binary protocol used by German EHZ electricity meters over a serial IR interface at 9600 baud, 8N1.
- **OBIS codes** identify meter values (e.g. `1.8.0` = consumed energy tariff 1, `16.7.0` = current power).
- Values are read via SoftwareSerial on D5 (RX, GPIO14) / D6 (TX, GPIO12) by default; hardware UART0 is available via `#define USE_HARDWARE_SERIAL`.
- The `DeadBand` class suppresses unnecessary MQTT publishes (15 s minimum interval; forced publish after 10 min even when value is unchanged).
- MQTT messages are published as plain ASCII floating-point numbers with retained flag set.
