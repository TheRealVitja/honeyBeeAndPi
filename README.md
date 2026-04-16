# HoneyBeeAndPi – ESP32 Hive System

## Features

- **ESP32-basierte Bienenstock-Waage**
  - Unterstützung für mehrere HX711-Kanäle (mehrere Waagen gleichzeitig)
  - Kalibrierung pro Kanal mit mehreren Referenzpunkten
  - Temperaturmessung über DS18B20
  - Batteriespannungsmessung
  - Temperaturkompensation für stabilere Gewichtswerte

- **WLAN & Konfigurationsportal**
  - Automatischer Access Point bei fehlender Konfiguration
  - Weboberfläche zur Einrichtung:
    - WLAN
    - MQTT
    - Device-ID
    - Anzahl Beuten / Kanäle
    - Kalibrierung der Waagen
    - Startwert für GTS
  - Persistente Speicherung via Preferences

- **MQTT-Telemetrie**
  - Periodisches Senden von:
    - Gewicht
    - Temperatur
    - Batteriespannung
    - RSSI
  - Dynamische Topic-Erzeugung basierend auf Device-ID
  - JSON-Format für einfache Weiterverarbeitung

- **OTA-Updates**
  - Firmware-Updates over-the-air
  - Zeitlich begrenztes OTA-Fenster zur Sicherheit

- **Backend (Go + MySQL)**
  - MQTT-Bridge speichert Daten in MySQL
  - trend-worker berechnet Trends
  - gts-worker berechnet Grünlandtemperatursumme (GTS)
  - Grafana Dashboard für Visualisierung

- **Grünlandtemperatursumme (GTS)**
  - Berechnung ab 1. Januar
  - Nur positive Tagesmitteltemperaturen werden berücksichtigt
  - Januar × 0,5
  - Februar × 0,75
  - ab März × 1,0

- **Offline-fähige Installation**
  - vendor/ Support für Go
  - Build ohne Internet möglich

## Voraussetzungen

### Hardware
- ESP32
- HX711 + Wägezellen
- DS18B20
- Raspberry Pi

### Software
- arduino-cli
- Docker
- Go (optional)

## Projektstruktur

├── esp32/
├── bridge/
├── trend-worker/
├── gts-worker/
├── grafana/
├── docker-compose.yml

## Flashen

- arduino-cli compile --fqbn esp32:esp32:esp32 .
- arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 .

## Start

docker compose up -d --build

## MQTT Beispiel

{
  "device_id": "scale-01",
  "temperature": 21.5,
  "channels": [{"id":0,"weight":12.34}]
}
