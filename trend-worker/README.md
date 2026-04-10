# Hive Trend Worker v1

Dieser Worker liest `hive_telemetry` aus InfluxDB und schreibt daraus Trenddaten in das Measurement `hive_trend`.

## Berechnete Felder

- `latest_weight_kg`
- `delta_1h_kg`
- `delta_6h_kg`
- `delta_24h_kg`
- `slope_1h_kg_per_h`
- `slope_6h_kg_per_h`
- `slope_24h_kg_per_h`
- `ma_1h_kg`
- `ma_6h_kg`
- `ema_24h_kg`
- `sample_count`
- `sudden_drop`
- `sudden_gain`
- `unstable_measurements`

## Eingabedaten

Der Worker verwendet bevorzugt:
- `compensated_weight_kg`

Fallback:
- `weight_kg`

Quelle:
- Measurement `hive_telemetry`

## Umgebungsvariablen

- `INFLUX_URL` – Standard: `http://localhost:8086`
- `INFLUX_TOKEN`
- `INFLUX_ORG` – Standard: `myorg`
- `INFLUX_BUCKET` – Standard: `telemetry`
- `TREND_RECOMPUTE_INTERVAL` – Standard: `5m`
- `TREND_LOOKBACK` – Standard: `48h`
- `TREND_SUDDEN_DROP_KG` – Standard: `2.0`
- `TREND_SUDDEN_GAIN_KG` – Standard: `2.0`

## Lokal starten

```bash
go mod tidy
go run main.go
```

## Docker bauen

```bash
docker build -t hive-trend-worker .
```

## Docker Compose Beispiel

```yaml
  trend-worker:
    build:
      context: ./trend-worker
    restart: unless-stopped
    env_file:
      - .env
    environment:
      INFLUX_URL: http://influxdb:8086
      INFLUX_ORG: ${INFLUXDB_ORG}
      INFLUX_BUCKET: ${INFLUXDB_BUCKET}
      INFLUX_TOKEN: ${INFLUXDB_TOKEN}
      TREND_RECOMPUTE_INTERVAL: 5m
      TREND_LOOKBACK: 48h
      TREND_SUDDEN_DROP_KG: 2.0
      TREND_SUDDEN_GAIN_KG: 2.0
```

## Grafana Query Beispiel

```flux
from(bucket: "telemetry")
  |> range(start: -24h)
  |> filter(fn: (r) => r._measurement == "hive_trend")
  |> filter(fn: (r) => r.device_id == "waage-01")
```

## Hinweise

- `unstable_measurements` ist in v1 noch ein Platzhalter und wird derzeit immer als `0` geschrieben.
- Für echte Instabilitätsanalyse solltest du später `channel_telemetry` mit `stable` und `drift_detected` zusätzlich einbeziehen.
- Die Trendwerte werden immer pro `device_id + hive_index` berechnet.
