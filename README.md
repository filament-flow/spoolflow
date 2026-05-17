# SpoolFlow – NFC Spool-Tracker für FilamentFlow

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

SpoolFlow ist ein ESP32-basiertes NFC-Gerät für [FilamentFlow](https://filament-flow.com). Es liest NFC-Tags von Filamentspulen und bucht den Verbrauch automatisch in der FilamentFlow Web-App.

## Features

- 🏷️ **FilamentFlow NTAG** – Selbst beschriebene NFC-Tags (ISO 14443A)
- 🔵 **Prusa OpenPrintTag** – Prusa Originalspulen (ISO 15693 / NFC-V) *(erfordert Hardware-Lizenz)*
- 🟠 **Bambu UID-Erkennung** – Bambu Originalspulen via UID-Mapping *(erfordert Hardware-Lizenz)*
- ⚖️ 4-Tasten-UI für Gramm-Eingabe (100/10/1/"OK")
- 📺 OLED-Display (SSD1306 128x64)
- 🔧 Setup via Browser (AP-Mode)

## Hardware

| Bauteil | Wert |
|---|---|
| Mikrocontroller | ESP32 Dev Module (WROOM-32E) |
| NFC-Reader | PN5180 |
| Display | SSD1306 OLED 128x64 (I2C) |
| Taster | 4x Drucktaster gegen GND |
| Stromversorgung | 5V via Micro-USB (min. 1A) |

## Pinbelegung

| Signal | GPIO |
|---|---|
| SPI NSS (PN5180) | 5 |
| SPI BUSY (PN5180) | 27 |
| SPI RST (PN5180) | 16 |
| SPI SCK | 18 |
| SPI MOSI → SDO | 23 |
| SPI MISO → SDI | 19 |
| I2C SDA (OLED) | 21 |
| I2C SCL (OLED) | 22 |
| BTN_L ◄ | 32 |
| BTN_R ► | 33 |
| BTN_ADD ✚ | 25 |
| BTN_OK ✓ | 26 |

> ⚠️ **Wichtig:** MOSI und MISO sind beim PN5180 absichtlich vertauscht (SDI↔SDO) – das ist kein Fehler!

## Bibliotheken

- [PN5180 by Andreas Trappmann](https://github.com/ATrappmann/PN5180-Library)
- ArduinoJson v6.x
- Adafruit SSD1306
- Adafruit GFX

## Flashen

1. `SpoolFlow_v5.ino` in Arduino IDE öffnen
2. Board: **ESP32 Dev Module**
3. Flashen

## Setup

1. ESP32 einschalten → Setup-Button beim Boot halten
2. Mit WLAN `SpoolFlow-Setup` verbinden
3. Browser öffnen: `http://192.168.4.1`
4. WLAN, API-Key und E-Mail konfigurieren
5. Speichern → startet neu

## Hardware-Lizenz (Pro-Features)

| Feature | Ohne Key | Mit SF/BN Key |
|---|---|---|
| FilamentFlow NTAG | ✅ | ✅ |
| Prusa OpenPrintTag | ❌ | ✅ |
| Bambu UID-Erkennung | ❌ | ✅ |

Keys erhältlich auf [filament-flow.com](https://filament-flow.com).

## NFC-Tag Workflows

**Workflow A – SpoolFlow-first:**
Spule scannen → unbekannt → in App verknüpfen → nächster Scan bucht direkt

**Workflow B – App-first:**
In App Filament anlegen + NFC-Tag verknüpfen → SpoolFlow erkennt sofort

**Workflow C – Prusa/Bambu:**
Originalspule scannen → UID wird erkannt → in App mit Filament verknüpfen

## Verwandte Projekte

- [FilamentFlow](https://filament-flow.com) – Die Web-App
- [WatchFlow](https://github.com/filament-flow/watchflow) – Drucker-Monitor

---

*MW Service 3D | filament-flow.com | @filament_flow_com*
