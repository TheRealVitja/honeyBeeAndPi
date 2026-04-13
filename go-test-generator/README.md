# GTS Testdaten-Generator in Go

Dieser Generator erzeugt realistische `telemetry.v4` Temperaturdaten für ein ganzes Jahr und kann sie:

- als JSON-Datei schreiben
- optional direkt per MQTT veröffentlichen

## Features

- generiert Tageswerte für ein ganzes Jahr
- mehrere Devices gleichzeitig
- reproduzierbar per Seed
- setzt optional `gts_start_year` und `gts_start_value` am 1. Januar
- Ausgabe kompatibel mit deinem GTS-Worker

## Dateien

- `main.go`
- `go.mod`

## Build

```bash
go mod tidy
go build -o gts-generator
```

## Beispiel 1: JSON-Datei erzeugen

```bash
./gts-generator   -year 2026   -devices 4   -prefix waage   -set-gts-start=true   -gts-start-value 0   -output gts_2026.json
```

## Beispiel 2: Direkt per MQTT veröffentlichen

```bash
./gts-generator   -year 2026   -devices 2   -prefix waage   -mqtt-broker tcp://192.168.2.217:1883   -publish=true
```

## Beispiel 3: Startwert mitten im Setup testen

Wenn du testen willst, dass dein Backend einen gesetzten Startwert übernimmt:

```bash
./gts-generator   -year 2026   -devices 1   -prefix waage   -set-gts-start=true   -gts-start-value 187.4   -output gts_with_start.json
```

## Ausgabeformat

Der Generator erzeugt Payloads wie:

```json
{
  "schema": "telemetry.v4",
  "device_id": "waage-01",
  "timestamp": "2026-01-01T11:00:00Z",
  "temperature_c": 3.7,
  "gts_start_year": 2026,
  "gts_start_value": 0
}
```

Danach folgen für jeden weiteren Tag Temperaturwerte ohne erneute Startwert-Felder.

## Hinweise

- Die Werte sind synthetisch, aber für GTS-Tests plausibel.
- Für Januar/Februar gibt es teils negative Temperaturen.
- Die GTS sollte deshalb an kalten Tagen nicht sinken, sondern nur nicht weiter steigen.
