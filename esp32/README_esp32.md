# ESP32 Firmware v7 Dynamic

Dateien:
- `esp32.ino`

## Neu in v7
- bis zu 8 HX711-Kanaele
- bis zu 4 logische Beuten
- Kanal-Zuordnung zu Beuten im Portal
- pro Kanal eigene Kalibrierung
- pro Kanal eigene Temperaturkompensation
- Summierung der kanalweise kalibrierten Gewichte pro Beute
- MQTT/WLAN-Test vor dem Speichern bleibt erhalten
- OTA bleibt erhalten
- Topic weiterhin automatisch aus `device_id`

## Architektur
Es gibt jetzt zwei Ebenen:

### 1. Kanaele
Das sind die physisch angeschlossenen HX711-Kanaele.

Beispiel:
- Kanal 0
- Kanal 1
- Kanal 2
- Kanal 3

### 2. Beuten
Eine Beute kann aus 1, 2 oder mehr Kanaelen bestehen.

Beispiel:
- Beute 0 = Kanal 0 + Kanal 1
- Beute 1 = Kanal 2 + Kanal 3

Die Firmware:
- liest jeden Kanal einzeln
- kalibriert jeden Kanal einzeln
- summiert danach die Gewichte pro Beute

## Portal-Seiten
- `/` Grundkonfiguration
- `/channels` Kanalkonfiguration
- `/calibration` Mehrpunkt-Kalibrierung pro Kanal
- `/tempcomp` Temperaturkompensation pro Kanal
- `/diag` Diagnose
- `/ota` OTA-Info

## Empfohlener Aufbau für 2 Waagen pro Beute
Zum Beispiel 2 Beuten mit je 2 Waagen:

- Kanal 0 -> Beute 0
- Kanal 1 -> Beute 0
- Kanal 2 -> Beute 1
- Kanal 3 -> Beute 1

Dann bekommst du im Payload:
- `channels[]` mit Einzelwerten
- `hives[]` mit Summen pro Beute

## Telemetrie-Format
Die Firmware sendet jetzt `schema = telemetry.v4`.

Wichtige Bereiche:
- `channels[]`
- `hives[]`

## Beispiel
```json
{
  "schema": "telemetry.v4",
  "device_id": "waage-01",
  "channels": [
    {
      "channel_index": 0,
      "hive_index": 0,
      "weight_kg": 12.1
    },
    {
      "channel_index": 1,
      "hive_index": 0,
      "weight_kg": 11.9
    }
  ],
  "hives": [
    {
      "hive_index": 0,
      "weight_kg": 24.0
    }
  ]
}
```

## Wichtige Hinweise
- Jeder Kanal muss einzeln kalibriert werden.
- Erst danach werden die kalibrierten Gewichte pro Beute summiert.


