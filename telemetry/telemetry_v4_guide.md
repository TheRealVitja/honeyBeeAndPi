# Telemetry v4 Setup & Test Guide

## Überblick

Dieses Dokument beschreibt, wie du deinen kompletten Stack für **telemetry.v4** betreibst und testest:

- ESP32 Firmware (v7 dynamic)
- MQTT (Mosquitto)
- Go Bridge
- InfluxDB
- Grafana
- Testdaten

---

## 1. Voraussetzungen

- Docker + Docker Compose
- MQTT Broker (im Compose enthalten)
- ESP32 mit Firmware v7
- Netzwerkzugriff auf deinen Raspberry Pi

---

## 2. Stack starten

```bash
cp .env.example .env
docker compose up -d --build
```

### Dienste

- MQTT: `localhost:1883`
- InfluxDB: `http://localhost:8086`
- Grafana: `http://localhost:3000`

Login Grafana:
```
admin / admin123456
```

---

## 3. ESP32 konfigurieren

1. ESP32 starten (AP Modus)
2. Verbinden mit:
   ```
   SSID: Waagen-Setup
   Passwort: setup1234
   ```
3. Browser öffnen:
   ```
   http://192.168.4.1
   ```

4. Eingeben:
   - WLAN
   - MQTT Host (z.B. Raspberry IP)
   - Device ID
   - Sleep Zeit

5. Speichern (inkl. MQTT Test)

---

## 4. Testdaten senden

### Beispiel:

```bash
mosquitto_pub -h <IP> -t devices/waage-01/telemetry -f test_2_hives_4_channels.json
```

---

## 5. Daten prüfen

### InfluxDB

Measurements:

- device_telemetry
- channel_telemetry
- hive_telemetry

---

### Grafana

Dashboard:
**ESP32 IoT Telemetry v4**

Filter:
- Device
- Hive
- Channel

---

## 6. Typische Checks

### Summen prüfen

Beispiel:

```
Kanal 0: 12.1 kg
Kanal 1: 11.9 kg

=> Beute: 24.0 kg
```

---

### Drift erkennen

Panel:
- `drift_detected`

---

### Signalqualität

Panel:
- `raw_stddev`

---

## 7. Typische Fehler

### Keine Daten in Grafana

- MQTT Topic falsch
- Bridge läuft nicht
- falscher Bucket

---

### MQTT Test schlägt fehl

- WLAN falsch
- MQTT Host falsch
- Port falsch

---

### Gewichte falsch

- Kalibrierung fehlt
- Kanäle falsch zugeordnet
- Mechanik fehlerhaft

---

## 8. Best Practice für Bienen

### Aufbau

- 2 Wägezellen pro Beute
- stabile Plattform
- keine Verspannung

### Kalibrierung

- jeden Kanal einzeln
- danach summieren

---

## 9. Nächste Schritte

- Alerts (Gewichtsverlust)
- Drift-Analyse im Backend
- MQTT TLS
- Remote OTA Trigger

---

## Fertig

Damit hast du ein komplettes System von Sensor bis Dashboard.
