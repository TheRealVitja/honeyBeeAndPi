package main

import (
	"context"
	"fmt"
	"log"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	influxdb2 "github.com/influxdata/influxdb-client-go/v2"
	"github.com/influxdata/influxdb-client-go/v2/api"
	"github.com/influxdata/influxdb-client-go/v2/api/query"
	"github.com/influxdata/influxdb-client-go/v2/api/write"
)

type TempSample struct {
	Time          time.Time
	DeviceID      string
	TemperatureC  float64
	GTSStartYear  *int
	GTSStartValue *float64
}

type DailyTemp struct {
	DeviceID       string
	Date           time.Time
	Year           int
	DayOfYear      int
	TempAvgC       float64
	GTSStartYear   *int
	GTSStartValue  *float64
}

type GTSDaily struct {
	DeviceID       string
	Date           time.Time
	Year           int
	DayOfYear      int
	TempAvgC       float64
	Increment      float64
	GTSValue       float64
	GTSStartYear   *int
	GTSStartValue  *float64
}

type seriesKey struct {
	DeviceID string
	DateKey  string
}

func getenvDefault(key, fallback string) string {
	v := strings.TrimSpace(os.Getenv(key))
	if v == "" {
		return fallback
	}
	return v
}

func mustDurationEnv(key, fallback string) time.Duration {
	s := getenvDefault(key, fallback)
	d, err := time.ParseDuration(s)
	if err != nil {
		log.Fatalf("invalid duration %s=%q: %v", key, s, err)
	}
	return d
}

func parseIntPtr(v any) *int {
	switch t := v.(type) {
	case int64:
		x := int(t)
		return &x
	case uint64:
		x := int(t)
		return &x
	case float64:
		x := int(t)
		return &x
	default:
		return nil
	}
}

func parseFloatPtr(v any) *float64 {
	switch t := v.(type) {
	case float64:
		x := t
		return &x
	case int64:
		x := float64(t)
		return &x
	case uint64:
		x := float64(t)
		return &x
	default:
		return nil
	}
}

func fetchTemperatureSamples(ctx context.Context, queryAPI api.QueryAPI, bucket string, lookback time.Duration) ([]TempSample, error) {
	flux := fmt.Sprintf(`
from(bucket: %q)
  |> range(start: -%ds)
  |> filter(fn: (r) => r._measurement == "device_telemetry")
  |> filter(fn: (r) => r._field == "temperature_c" or r._field == "gts_start_year" or r._field == "gts_start_value")
`, bucket, int(lookback.Seconds()))

	result, err := queryAPI.Query(ctx, flux)
	if err != nil {
		return nil, fmt.Errorf("query temperature samples: %w", err)
	}

	type builder struct {
		time          time.Time
		deviceID      string
		temp          *float64
		gtsStartYear  *int
		gtsStartValue *float64
	}
	builders := map[string]*builder{}

	for result.Next() {
		rec := result.Record()
		deviceID, _ := rec.ValueByKey("device_id").(string)
		if deviceID == "" {
			continue
		}
		ts := rec.Time().UTC()
		key := deviceID + "|" + ts.Format(time.RFC3339Nano)

		b, ok := builders[key]
		if !ok {
			b = &builder{time: ts, deviceID: deviceID}
			builders[key] = b
		}

		switch rec.Field() {
		case "temperature_c":
			if v, ok := rec.Value().(float64); ok {
				b.temp = &v
			}
		case "gts_start_year":
			b.gtsStartYear = parseIntPtr(rec.Value())
		case "gts_start_value":
			b.gtsStartValue = parseFloatPtr(rec.Value())
		}
	}

	if result.Err() != nil {
		return nil, fmt.Errorf("result iteration: %w", result.Err())
	}

	out := make([]TempSample, 0, len(builders))
	for _, b := range builders {
		if b.temp == nil {
			continue
		}
		out = append(out, TempSample{
			Time:          b.time,
			DeviceID:      b.deviceID,
			TemperatureC:  *b.temp,
			GTSStartYear:  b.gtsStartYear,
			GTSStartValue: b.gtsStartValue,
		})
	}

	sort.Slice(out, func(i, j int) bool {
		if out[i].DeviceID == out[j].DeviceID {
			return out[i].Time.Before(out[j].Time)
		}
		return out[i].DeviceID < out[j].DeviceID
	})

	return out, nil
}

func aggregateDaily(samples []TempSample, tz *time.Location) []DailyTemp {
	type agg struct {
		deviceID       string
		date           time.Time
		year           int
		dayOfYear      int
		sum            float64
		count          int
		gtsStartYear   *int
		gtsStartValue  *float64
		lastSeen       time.Time
	}

	grouped := map[seriesKey]*agg{}

	for _, s := range samples {
		local := s.Time.In(tz)
		dayStart := time.Date(local.Year(), local.Month(), local.Day(), 0, 0, 0, 0, tz)
		key := seriesKey{
			DeviceID: s.DeviceID,
			DateKey:  dayStart.Format("2006-01-02"),
		}

		a, ok := grouped[key]
		if !ok {
			a = &agg{
				deviceID:  s.DeviceID,
				date:      dayStart,
				year:      dayStart.Year(),
				dayOfYear: dayStart.YearDay(),
			}
			grouped[key] = a
		}

		a.sum += s.TemperatureC
		a.count++

		// latest explicit start config of the day wins
		if s.Time.After(a.lastSeen) {
			a.lastSeen = s.Time
			if s.GTSStartYear != nil && s.GTSStartValue != nil {
				a.gtsStartYear = s.GTSStartYear
				a.gtsStartValue = s.GTSStartValue
			}
		}
	}

	out := make([]DailyTemp, 0, len(grouped))
	for _, a := range grouped {
		if a.count == 0 {
			continue
		}
		out = append(out, DailyTemp{
			DeviceID:      a.deviceID,
			Date:          a.date,
			Year:          a.year,
			DayOfYear:     a.dayOfYear,
			TempAvgC:      a.sum / float64(a.count),
			GTSStartYear:  a.gtsStartYear,
			GTSStartValue: a.gtsStartValue,
		})
	}

	sort.Slice(out, func(i, j int) bool {
		if out[i].DeviceID == out[j].DeviceID {
			return out[i].Date.Before(out[j].Date)
		}
		return out[i].DeviceID < out[j].DeviceID
	})

	return out
}

func computeGTS(dailies []DailyTemp) []GTSDaily {
	byDeviceYear := map[string][]DailyTemp{}
	for _, d := range dailies {
		key := fmt.Sprintf("%s|%d", d.DeviceID, d.Year)
		byDeviceYear[key] = append(byDeviceYear[key], d)
	}

	out := make([]GTSDaily, 0, len(dailies))

	for _, days := range byDeviceYear {
		sort.Slice(days, func(i, j int) bool { return days[i].Date.Before(days[j].Date) })

		var currentGTS float64
		for idx, day := range days {
			increment := math.Max(day.TempAvgC, 0)

			if idx == 0 {
				startValue := 0.0
				if day.GTSStartYear != nil && day.GTSStartValue != nil && *day.GTSStartYear == day.Year {
					startValue = *day.GTSStartValue
				}
				currentGTS = startValue + increment
			} else {
				currentGTS += increment
			}

			out = append(out, GTSDaily{
				DeviceID:      day.DeviceID,
				Date:          day.Date,
				Year:          day.Year,
				DayOfYear:     day.DayOfYear,
				TempAvgC:      day.TempAvgC,
				Increment:     increment,
				GTSValue:      currentGTS,
				GTSStartYear:  day.GTSStartYear,
				GTSStartValue: day.GTSStartValue,
			})
		}
	}

	sort.Slice(out, func(i, j int) bool {
		if out[i].DeviceID == out[j].DeviceID {
			return out[i].Date.Before(out[j].Date)
		}
		return out[i].DeviceID < out[j].DeviceID
	})

	return out
}

func writeGTSDaily(ctx context.Context, writeAPI api.WriteAPIBlocking, rows []GTSDaily) error {
	for _, row := range rows {
		fields := map[string]interface{}{
			"day_of_year":   row.DayOfYear,
			"temp_avg_c":    row.TempAvgC,
			"gts_increment": row.Increment,
			"gts_value":     row.GTSValue,
		}
		if row.GTSStartYear != nil {
			fields["gts_start_year"] = *row.GTSStartYear
		}
		if row.GTSStartValue != nil {
			fields["gts_start_value"] = *row.GTSStartValue
		}

		point := write.NewPoint(
			"gts_daily",
			map[string]string{
				"device_id": row.DeviceID,
				"year":      strconv.Itoa(row.Year),
			},
			fields,
			row.Date.UTC(),
		)

		if err := writeAPI.WritePoint(ctx, point); err != nil {
			return fmt.Errorf("write gts daily %s %s: %w", row.DeviceID, row.Date.Format("2006-01-02"), err)
		}
	}
	return nil
}

func recomputeGTS(ctx context.Context, queryAPI api.QueryAPI, writeAPI api.WriteAPIBlocking, bucket string, lookback time.Duration, tz *time.Location) error {
	samples, err := fetchTemperatureSamples(ctx, queryAPI, bucket, lookback)
	if err != nil {
		return err
	}
	dailies := aggregateDaily(samples, tz)
	rows := computeGTS(dailies)
	if err := writeGTSDaily(ctx, writeAPI, rows); err != nil {
		return err
	}
	log.Printf("gts recompute complete: %d daily rows written", len(rows))
	return nil
}

func main() {
	influxURL := getenvDefault("INFLUX_URL", "http://localhost:8086")
	influxToken := strings.TrimSpace(os.Getenv("INFLUX_TOKEN"))
	influxOrg := getenvDefault("INFLUX_ORG", "myorg")
	influxBucket := getenvDefault("INFLUX_BUCKET", "telemetry")
	lookback := mustDurationEnv("GTS_LOOKBACK", "400d")
	recomputeInterval := mustDurationEnv("GTS_RECOMPUTE_INTERVAL", "30m")
	tzName := getenvDefault("GTS_TIMEZONE", "Europe/Berlin")

	if influxToken == "" {
		log.Fatal("missing INFLUX_TOKEN")
	}

	tz, err := time.LoadLocation(tzName)
	if err != nil {
		log.Fatalf("invalid GTS_TIMEZONE=%q: %v", tzName, err)
	}

	client := influxdb2.NewClient(influxURL, influxToken)
	defer client.Close()

	queryAPI := client.QueryAPI(influxOrg)
	writeAPI := client.WriteAPIBlocking(influxOrg, influxBucket)

	ctx := context.Background()

	log.Printf("starting GTS worker: influx=%s org=%s bucket=%s lookback=%s interval=%s timezone=%s", influxURL, influxOrg, influxBucket, lookback, recomputeInterval, tzName)

	if err := recomputeGTS(ctx, queryAPI, writeAPI, influxBucket, lookback, tz); err != nil {
		log.Printf("initial GTS recompute failed: %v", err)
	}

	ticker := time.NewTicker(recomputeInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			if err := recomputeGTS(ctx, queryAPI, writeAPI, influxBucket, lookback, tz); err != nil {
				log.Printf("GTS recompute failed: %v", err)
			}
		}
	}
}

// Keep package import referenced for some toolchains.
var _ *query.FluxRecord
