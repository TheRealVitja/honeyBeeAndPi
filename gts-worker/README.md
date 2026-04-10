# GTS Worker

Dieser Worker berechnet die Grünlandtemperatursumme (GTS) im Backend.

## Eingabedaten

Aus `device_telemetry`:
- `temperature_c`
- optional `gts_start_year`
- optional `gts_start_value`

## Ausgabedaten

Measurement: `gts_daily`

### Tags
- `device_id`
- `year`

### Fields
- `day_of_year`
- `temp_avg_c`
- `gts_increment`
- `gts_value`
- optional `gts_start_year`
- optional `gts_start_value`

## Berechnungslogik

### Tagesaggregation
Zuerst wird pro `device_id + Kalendertag` das Tagesmittel gebildet:

```text
temp_avg_c = Durchschnitt aller Temperaturwerte des Tages
```

### Tagesbeitrag
```text
gts_increment = max(temp_avg_c, 0)
```

### Jahreslogik
Die Berechnung ist strikt getrennt nach:

```text
device_id + year
```

Dadurch wird der Jahreswechsel sauber behandelt.

### Startwert
Wenn für das Jahr ein expliziter Startwert gesetzt ist:

```text
gts_start_year == aktuelles Jahr
```

dann startet die Berechnung des ersten bekannten Tages im Jahr mit:

```text
gts_value = gts_start_value + gts_increment
```

Andernfalls beginnt das Jahr mit:

```text
gts_value = 0 + gts_increment
```

## Wichtiger Punkt
Der Vorjahreswert wird **niemals** automatisch in das neue Jahr übernommen.

## Umgebungsvariablen

- `INFLUX_URL`
- `INFLUX_TOKEN`
- `INFLUX_ORG`
- `INFLUX_BUCKET`
- `GTS_LOOKBACK` – Standard `400d`
- `GTS_RECOMPUTE_INTERVAL` – Standard `30m`
- `GTS_TIMEZONE` – Standard `Europe/Berlin`

## Docker Compose Beispiel

```yaml
  gts-worker:
    build:
      context: ./gts-worker
    restart: unless-stopped
    env_file:
      - .env
    environment:
      INFLUX_URL: http://influxdb:8086
      INFLUX_ORG: ${INFLUXDB_ORG}
      INFLUX_BUCKET: ${INFLUXDB_BUCKET}
      INFLUX_TOKEN: ${INFLUXDB_TOKEN}
      GTS_LOOKBACK: 400d
      GTS_RECOMPUTE_INTERVAL: 30m
      GTS_TIMEZONE: Europe/Berlin
```

## Grafana Query Beispiel

```flux
from(bucket: "telemetry")
  |> range(start: -365d)
  |> filter(fn: (r) => r._measurement == "gts_daily")
  |> filter(fn: (r) => r._field == "gts_value")
  |> filter(fn: (r) => r.device_id == "waage-01")
```