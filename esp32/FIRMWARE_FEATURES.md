# BeeWeight ESP32 Firmware – Funktionsübersicht

- **WLAN-Verbindung mit Retry-Logik**
  - Verbindet sich mit gespeicherten WLAN-Zugangsdaten
  - Versucht bis zu 3-mal, bevor in den Konfigurationsmodus (AP) gewechselt wird

- **MQTT-Verbindung mit Retry-Logik**
  - Baut Verbindung zum konfigurierten MQTT-Broker auf
  - Versucht bis zu 3-mal, bevor in den Konfigurationsmodus (AP) gewechselt wird

- **Access Point (AP) / Konfigurationsportal**
  - Startet eigenen WLAN-AP, wenn keine Verbindung zu WLAN/MQTT möglich ist oder keine Konfiguration vorliegt
  - Webinterface zur Konfiguration von WLAN, MQTT, Kanälen, Kalibrierung, Temperaturkompensation, OTA, etc.
  - Konfigurationsportal läuft für 10 Minuten, dann Neustart

- **Sensorik & Messung**
  - Unterstützt bis zu 6 Kanäle (Wägezellen über HX711)
  - Temperaturmessung (DS18B20)
  - Batteriespannungsmessung
  - Messwertaufnahme mit Stabilitäts- und Driftprüfung
  - Temperaturkompensation pro Kanal möglich

- **Kalibrierung**
  - Mehrpunkt-Kalibrierung pro Kanal
  - Verwaltung und Speicherung der Kalibrierpunkte im Flash
  - Webinterface zur Kalibrierung und Löschung einzelner Punkte

- **MQTT-Telemetrie**
  - Sendet Messdaten als JSON an einen MQTT-Broker
  - Enthält alle Kanäle, Hives, Temperatur, Batteriespannung, RSSI, etc.
  - Device-ID und Topic konfigurierbar

- **OTA-Update**
  - Ermöglicht Over-the-Air Firmware-Updates für einen konfigurierbaren Zeitraum nach jedem Boot

- **Energiesparmodus**
  - Nach erfolgreicher Messung und Übertragung: Trennung von MQTT/WLAN, dann Deep Sleep für konfigurierbare Zeit

- **Weitere Features**
  - Diagnose- und Scan-Seiten im Webinterface
  - Reset- und Konfigurations-Reset-Funktion
  - GTS-Startwert/Startjahr für Langzeitmessungen
  - Debug-Ausgaben über die serielle Schnittstelle

---

**Hinweis:**
- Die Firmware ist für batteriebetriebene, vernetzte Bienenstockwaagen ausgelegt.
- Alle Einstellungen werden persistent im Flash gespeichert.
