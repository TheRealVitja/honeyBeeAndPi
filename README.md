# HoneyBeeAndPi – ESP32 Hive System

## Features

- **ESP32-basierte Bienenstock-Waage**
  - Unterstützung für mehrere HX711-Kanäle (mehrere Waagen gleichzeitig)
  - Kalibrierung pro Kanal mit mehreren Referenzpunkten
  - Temperaturmessung über DS18B20
  - Batteriespannungsmessung

- **WLAN & Konfigurationsportal**
  - Automatischer Access Point bei fehlender Konfiguration
  - Weboberfläche zur Einrichtung (WLAN, MQTT, Geräte-ID, GTS, Beuten, Kalibrierung der Waagen)
  - Persistente Speicherung via Preferences

- **MQTT-Telemetrie**
  - Periodisches Senden von Gewicht, Temperatur, Batterie und RSSI
  - Dynamische Topic-Erzeugung basierend auf Device-ID
  - JSON-Format für einfache Weiterverarbeitung

- **OTA-Updates**
  - Firmware-Updates over-the-air
  - Zeitlich begrenztes OTA-Fenster zur Sicherheit

- **Backend (Go)**
  - MQTT-Bridge zur Weiterverarbeitung der Daten
  - Optionaler Worker für Aggregation/Trends
  - Docker-basiertes Setup

- **Offline-fähige Installation**
  - Unterstützung für `vendor/`-Dependencies (Go)
  - Build ohne Internet auf Zielsystem möglich

---

## Voraussetzungen

### Hardware
- ESP32
- HX711 + Wägezellen
- DS18B20 (Temperaturfühler)
- Raspberry Pi (für Backend)
- WLAN-Netz

### Software
- `arduino-cli`
- Docker + Docker Compose
- Go (nur für Entwicklung nötig)

---

## Projektstruktur

```
.
├── esp32/                 # Firmware
├── bridge/                # Go MQTT Bridge
├── trend-worker/          # Optionaler Trend Worker
├── gts-worker/            # Go GrünlandTemperaturSumme
├── docker-compose.yml
```

---

## 1. ESP32 Firmware flashen

### Abhängigkeiten installieren

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install PubSubClient
arduino-cli lib install "HX711 Arduino Library"
arduino-cli lib install OneWire
arduino-cli lib install DallasTemperature
arduino-cli lib install ArduinoJson
```

### Kompilieren

```bash
cd esp32
arduino-cli compile --fqbn esp32:esp32:esp32 .
```

### Upload

```bash
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 .
```

### Seriellen Monitor öffnen

```bash
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
```

---

## 2. Erstkonfiguration

Nach dem ersten Start:

1. ESP32 erstellt WLAN:
   SSID: Waagen-Setup  
   Passwort: setup  

2. Browser öffnen:
   http://192.168.4.1

3. Konfigurieren:
   - WLAN
   - MQTT-Server
   - Device-ID

---

## 3. Backend starten (Docker)

### Mit Internet

```bash
docker compose up -d --build
```

### Ohne Internet

```bash
cd bridge
go mod tidy
go mod vendor

cd ../trend-worker
go mod tidy
go mod vendor
```

Dann:

```bash
docker compose build --no-cache
docker compose up -d
```

---

## 4. MQTT Datenformat

```json
{
  "device_id": "scale-01",
  "timestamp": "2026-01-01T12:00:00Z",
  "temperature": 21.5,
  "battery": 3.9,
  "rssi": -65,
  "channels": [
    {
      "id": 0,
      "weight": 12.34
    }
  ]
}
```

---

## 5. OTA Update

- OTA ist nach dem Start für begrenzte Zeit aktiv
- Upload über Arduino OTA Tools möglich

---

## 6. Troubleshooting

- Fehlende Libraries → `arduino-cli lib install`
- Nur eine `.ino` Datei im Ordner
- MQTT prüfen (Host, Port, Credentials)
- Offline Build → `go mod vendor`

---

## 7. Entwicklung

```bash
cd bridge
go build -mod=vendor
```

```bash
GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -o bridge
```

---

## 8. Lizenz & Beiträge

- Pull Requests willkommen
- Issues für Bugs nutzen
