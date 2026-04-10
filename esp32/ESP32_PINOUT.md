# ESP32 DevKitC V2 – Pinbelegung für BeeWeight

## Übersicht

Diese Dokumentation beschreibt die Standard-Pinbelegung für:
- HX711 (Wägezellen)
- DS18B20 (Temperatursensor)
- Batteriespannungsmessung

Die Belegung basiert auf der Default-Konfiguration der Firmware.

---

## HX711 (Wägezellen)

Mehrere Kanäle werden parallel betrieben. Jeder Kanal benötigt:
- 1x DOUT
- 1x SCK

### Standard Pinbelegung

| Kanal | DOUT | SCK |
|------|------|-----|
| 0 | GPIO16 | GPIO17 |
| 1 | GPIO18 | GPIO19 |
| 2 | GPIO21 | GPIO22 |
| 3 | GPIO23 | GPIO25 |
| 4 | GPIO26 | GPIO13 |
| 5 | GPIO27 | GPIO14 |
| 6 | GPIO32 | GPIO12 |
| 7 | GPIO33 | GPIO15 |

### Hinweise

- GPIO34–39 sind **nur Input** → nicht für SCK verwenden
- GPIO12 kann Boot-Probleme verursachen (Pullups beachten)
- GPIO15 beeinflusst Boot-Modus → vorsichtig verwenden

---

## DS18B20 (Temperatur)

| Funktion | Pin |
|----------|-----|
| Data     | GPIO4 |

### Verdrahtung

- DATA → GPIO4
- VCC → 3.3V
- GND → GND
- **4.7k Pull-Up Widerstand** zwischen DATA und VCC erforderlich

---

## Batteriespannung

| Funktion | Pin |
|----------|-----|
| ADC      | GPIO34 |

### Hinweise

- Spannungsteiler erforderlich (z. B. 1:2)
- Maximal 3.3V am ADC-Pin

---

## Spannungsversorgung

- ESP32: 5V (USB) oder 3.3V geregelt
- HX711: 2.7–5V (typisch 3.3V oder 5V)
- DS18B20: 3.0–5.5V

---

## Beispiel Verdrahtung (ein Kanal)

HX711 → ESP32:

- VCC → 3.3V
- GND → GND
- DOUT → GPIO16
- SCK → GPIO17

DS18B20 → ESP32:

- DATA → GPIO4
- VCC → 3.3V
- GND → GND
- 4.7kΩ zwischen DATA und VCC

---

## Hinweise zur Praxis

- Alle GNDs müssen verbunden sein
- Kabellängen bei HX711 möglichst kurz halten
- Bei langen Leitungen:
  - geschirmte Kabel verwenden
  - zusätzliche Filterung einbauen
- Störungen können Messwerte stark beeinflussen

---

## Anpassung

Die Pins können in der Firmware geändert werden:

```cpp
const int DEFAULT_DOUT_PINS[MAX_CHANNELS] = {...};
const int DEFAULT_SCK_PINS[MAX_CHANNELS]  = {...};
const int ONEWIRE_PIN = 4;
const int BATTERY_ADC_PIN = 34;
```

---

## Empfehlung

Für stabile Systeme:

- HX711 mit separater, stabiler Versorgung
- Masse sternförmig führen
- ESP32 nicht direkt neben Wägezellen platzieren
