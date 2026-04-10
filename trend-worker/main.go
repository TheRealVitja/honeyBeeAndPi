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

type HivePoint struct {
	Time                time.Time
	WeightKG            float64
	CompensatedWeightKG *float64
	ChannelCount        *int
}

type HiveTrend struct {
	DeviceID             string
	HiveIndex            int
	HiveName             string
	LatestTime           time.Time
	LatestWeightKG       float64
	Delta1hKG            *float64
	Delta6hKG            *float64
	Delta24hKG           *float64
	Slope1hKGPerHour     *float64
	Slope6hKGPerHour     *float64
	Slope24hKGPerHour    *float64
	MA1hKG               *float64
	MA6hKG               *float64
	EMA24hKG             *float64
	SampleCount          int
	SuddenDrop           bool
	SuddenGain           bool
	UnstableMeasurements bool
}

type hiveKey struct {
	DeviceID  string
	HiveIndex int
	HiveName  string
}

type pointBuilder struct {
	timestamp         time.Time
	weightKG          *float64
	compensatedWeight *float64
	channelCount      *int
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

func boolToInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

func chooseWeight(p HivePoint) (float64, bool) {
	if p.CompensatedWeightKG != nil {
		return *p.CompensatedWeightKG, true
	}
	return p.WeightKG, true
}

func movingAverage(points []HivePoint) *float64 {
	if len(points) == 0 {
		return nil
	}
	var sum float64
	for _, p := range points {
		w, _ := chooseWeight(p)
		sum += w
	}
	v := sum / float64(len(points))
	return &v
}

func ema(points []HivePoint, alpha float64) *float64 {
	if len(points) == 0 {
		return nil
	}
	w0, _ := chooseWeight(points[0])
	value := w0
	for i := 1; i < len(points); i++ {
		w, _ := chooseWeight(points[i])
		value = alpha*w + (1-alpha)*value
	}
	return &value
}

func linearSlopeKGPerHour(points []HivePoint) *float64 {
	if len(points) < 2 {
		return nil
	}
	t0 := points[0].Time
	var sumX, sumY, sumXY, sumXX float64
	n := float64(len(points))

	for _, p := range points {
		x := p.Time.Sub(t0).Seconds() / 3600.0
		y, _ := chooseWeight(p)
		sumX += x
		sumY += y
		sumXY += x * y
		sumXX += x * x
	}

	denom := n*sumXX - sumX*sumX
	if math.Abs(denom) < 1e-9 {
		return nil
	}
	slope := (n*sumXY - sumX*sumY) / denom
	return &slope
}

func closestPointAtOrBefore(points []HivePoint, ref time.Time, tolerance time.Duration) *HivePoint {
	var best *HivePoint
	bestDiff := time.Duration(math.MaxInt64)

	for i := range points {
		p := &points[i]
		if p.Time.After(ref) {
			continue
		}
		diff := ref.Sub(p.Time)
		if diff <= tolerance && diff < bestDiff {
			best = p
			bestDiff = diff
		}
	}
	return best
}

func deltaFromReference(latest HivePoint, ref *HivePoint) *float64 {
	if ref == nil {
		return nil
	}
	lw, _ := chooseWeight(latest)
	rw, _ := chooseWeight(*ref)
	v := lw - rw
	return &v
}

func filterWindow(points []HivePoint, since time.Time) []HivePoint {
	out := make([]HivePoint, 0, len(points))
	for _, p := range points {
		if !p.Time.Before(since) {
			out = append(out, p)
		}
	}
	return out
}

func buildTrend(key hiveKey, points []HivePoint, suddenDropKG, suddenGainKG float64) *HiveTrend {
	if len(points) == 0 {
		return nil
	}
	sort.Slice(points, func(i, j int) bool { return points[i].Time.Before(points[j].Time) })

	latest := points[len(points)-1]
	latestWeight, _ := chooseWeight(latest)

	now := latest.Time
	points1h := filterWindow(points, now.Add(-1*time.Hour))
	points6h := filterWindow(points, now.Add(-6*time.Hour))
	points24h := filterWindow(points, now.Add(-24*time.Hour))

	ref1h := closestPointAtOrBefore(points, now.Add(-1*time.Hour), 30*time.Minute)
	ref6h := closestPointAtOrBefore(points, now.Add(-6*time.Hour), 90*time.Minute)
	ref24h := closestPointAtOrBefore(points, now.Add(-24*time.Hour), 3*time.Hour)

	d1 := deltaFromReference(latest, ref1h)
	d6 := deltaFromReference(latest, ref6h)
	d24 := deltaFromReference(latest, ref24h)

	trend := &HiveTrend{
		DeviceID:             key.DeviceID,
		HiveIndex:            key.HiveIndex,
		HiveName:             key.HiveName,
		LatestTime:           latest.Time,
		LatestWeightKG:       latestWeight,
		Delta1hKG:            d1,
		Delta6hKG:            d6,
		Delta24hKG:           d24,
		Slope1hKGPerHour:     linearSlopeKGPerHour(points1h),
		Slope6hKGPerHour:     linearSlopeKGPerHour(points6h),
		Slope24hKGPerHour:    linearSlopeKGPerHour(points24h),
		MA1hKG:               movingAverage(points1h),
		MA6hKG:               movingAverage(points6h),
		EMA24hKG:             ema(points24h, 0.2),
		SampleCount:          len(points24h),
		SuddenDrop:           d1 != nil && *d1 <= -math.Abs(suddenDropKG),
		SuddenGain:           d1 != nil && *d1 >= math.Abs(suddenGainKG),
		UnstableMeasurements: false,
	}
	return trend
}

func buildTrendPoint(t HiveTrend) *write.Point {
	fields := map[string]interface{}{
		"latest_weight_kg":      t.LatestWeightKG,
		"sample_count":          t.SampleCount,
		"sudden_drop":           boolToInt(t.SuddenDrop),
		"sudden_gain":           boolToInt(t.SuddenGain),
		"unstable_measurements": boolToInt(t.UnstableMeasurements),
	}
	if t.Delta1hKG != nil {
		fields["delta_1h_kg"] = *t.Delta1hKG
	}
	if t.Delta6hKG != nil {
		fields["delta_6h_kg"] = *t.Delta6hKG
	}
	if t.Delta24hKG != nil {
		fields["delta_24h_kg"] = *t.Delta24hKG
	}
	if t.Slope1hKGPerHour != nil {
		fields["slope_1h_kg_per_h"] = *t.Slope1hKGPerHour
	}
	if t.Slope6hKGPerHour != nil {
		fields["slope_6h_kg_per_h"] = *t.Slope6hKGPerHour
	}
	if t.Slope24hKGPerHour != nil {
		fields["slope_24h_kg_per_h"] = *t.Slope24hKGPerHour
	}
	if t.MA1hKG != nil {
		fields["ma_1h_kg"] = *t.MA1hKG
	}
	if t.MA6hKG != nil {
		fields["ma_6h_kg"] = *t.MA6hKG
	}
	if t.EMA24hKG != nil {
		fields["ema_24h_kg"] = *t.EMA24hKG
	}

	return write.NewPoint(
		"hive_trend",
		map[string]string{
			"device_id":  t.DeviceID,
			"hive_index": strconv.Itoa(t.HiveIndex),
			"hive_name":  t.HiveName,
		},
		fields,
		t.LatestTime,
	)
}

func fetchHiveTelemetry(ctx context.Context, queryAPI api.QueryAPI, bucket string, lookback time.Duration) (map[hiveKey][]HivePoint, error) {
	flux := fmt.Sprintf(`
from(bucket: %q)
  |> range(start: -%ds)
  |> filter(fn: (r) => r._measurement == "hive_telemetry")
  |> filter(fn: (r) => r._field == "weight_kg" or r._field == "compensated_weight_kg" or r._field == "channel_count")
`, bucket, int(lookback.Seconds()))

	result, err := queryAPI.Query(ctx, flux)
	if err != nil {
		return nil, fmt.Errorf("query hive telemetry: %w", err)
	}

	builders := map[hiveKey]map[time.Time]*pointBuilder{}

	for result.Next() {
		rec := result.Record()
		deviceID, ok := rec.ValueByKey("device_id").(string)
		if !ok || deviceID == "" {
			continue
		}
		hiveIndexStr, ok := rec.ValueByKey("hive_index").(string)
		if !ok || hiveIndexStr == "" {
			continue
		}
		hiveIndex, err := strconv.Atoi(hiveIndexStr)
		if err != nil {
			continue
		}
		hiveName, _ := rec.ValueByKey("hive_name").(string)
		key := hiveKey{DeviceID: deviceID, HiveIndex: hiveIndex, HiveName: hiveName}

		if _, exists := builders[key]; !exists {
			builders[key] = map[time.Time]*pointBuilder{}
		}

		ts := rec.Time()
		pb, exists := builders[key][ts]
		if !exists {
			pb = &pointBuilder{timestamp: ts}
			builders[key][ts] = pb
		}

		field := rec.Field()
		switch field {
		case "weight_kg":
			if v, ok := rec.Value().(float64); ok {
				pb.weightKG = &v
			}
		case "compensated_weight_kg":
			if v, ok := rec.Value().(float64); ok {
				pb.compensatedWeight = &v
			}
		case "channel_count":
			switch v := rec.Value().(type) {
			case int64:
				i := int(v)
				pb.channelCount = &i
			case uint64:
				i := int(v)
				pb.channelCount = &i
			case float64:
				i := int(v)
				pb.channelCount = &i
			}
		}
	}

	if result.Err() != nil {
		return nil, fmt.Errorf("result iteration: %w", result.Err())
	}

	out := map[hiveKey][]HivePoint{}
	for key, perTime := range builders {
		points := make([]HivePoint, 0, len(perTime))
		for _, pb := range perTime {
			if pb.weightKG == nil && pb.compensatedWeight == nil {
				continue
			}
			p := HivePoint{
				Time: pb.timestamp,
			}
			if pb.weightKG != nil {
				p.WeightKG = *pb.weightKG
			}
			if pb.compensatedWeight != nil {
				p.CompensatedWeightKG = pb.compensatedWeight
				if pb.weightKG == nil {
					p.WeightKG = *pb.compensatedWeight
				}
			}
			p.ChannelCount = pb.channelCount
			points = append(points, p)
		}
		sort.Slice(points, func(i, j int) bool { return points[i].Time.Before(points[j].Time) })
		out[key] = points
	}
	return out, nil
}

func recomputeAllHiveTrends(ctx context.Context, queryAPI api.QueryAPI, writeAPI api.WriteAPIBlocking, bucket string, lookback time.Duration, suddenDropKG, suddenGainKG float64) error {
	grouped, err := fetchHiveTelemetry(ctx, queryAPI, bucket, lookback)
	if err != nil {
		return err
	}

	count := 0
	for key, points := range grouped {
		trend := buildTrend(key, points, suddenDropKG, suddenGainKG)
		if trend == nil {
			continue
		}
		if err := writeAPI.WritePoint(ctx, buildTrendPoint(*trend)); err != nil {
			return fmt.Errorf("write trend %s/%d: %w", key.DeviceID, key.HiveIndex, err)
		}
		count++
	}
	log.Printf("trend recompute complete: %d hive trend points written", count)
	return nil
}

func runTrendWorker(ctx context.Context, queryAPI api.QueryAPI, writeAPI api.WriteAPIBlocking, bucket string, interval, lookback time.Duration, suddenDropKG, suddenGainKG float64) error {
	if err := recomputeAllHiveTrends(ctx, queryAPI, writeAPI, bucket, lookback, suddenDropKG, suddenGainKG); err != nil {
		log.Printf("initial trend recompute failed: %v", err)
	}

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			if err := recomputeAllHiveTrends(ctx, queryAPI, writeAPI, bucket, lookback, suddenDropKG, suddenGainKG); err != nil {
				log.Printf("trend recompute failed: %v", err)
			}
		}
	}
}

func main() {
	influxURL := getenvDefault("INFLUX_URL", "http://localhost:8086")
	influxToken := strings.TrimSpace(os.Getenv("INFLUX_TOKEN"))
	influxOrg := getenvDefault("INFLUX_ORG", "myorg")
	influxBucket := getenvDefault("INFLUX_BUCKET", "telemetry")
	recomputeInterval := mustDurationEnv("TREND_RECOMPUTE_INTERVAL", "5m")
	lookback := mustDurationEnv("TREND_LOOKBACK", "48h")

	suddenDropKG, err := strconv.ParseFloat(getenvDefault("TREND_SUDDEN_DROP_KG", "2.0"), 64)
	if err != nil {
		log.Fatalf("invalid TREND_SUDDEN_DROP_KG: %v", err)
	}
	suddenGainKG, err := strconv.ParseFloat(getenvDefault("TREND_SUDDEN_GAIN_KG", "2.0"), 64)
	if err != nil {
		log.Fatalf("invalid TREND_SUDDEN_GAIN_KG: %v", err)
	}

	if influxToken == "" {
		log.Fatal("missing INFLUX_TOKEN")
	}

	client := influxdb2.NewClient(influxURL, influxToken)
	defer client.Close()

	queryAPI := client.QueryAPI(influxOrg)
	writeAPI := client.WriteAPIBlocking(influxOrg, influxBucket)

	log.Printf(
		"starting hive trend worker: influx=%s org=%s bucket=%s interval=%s lookback=%s sudden_drop=%.2f sudden_gain=%.2f",
		influxURL, influxOrg, influxBucket, recomputeInterval, lookback, suddenDropKG, suddenGainKG,
	)

	ctx := context.Background()
	if err := runTrendWorker(ctx, queryAPI, writeAPI, influxBucket, recomputeInterval, lookback, suddenDropKG, suddenGainKG); err != nil && err != context.Canceled {
		log.Fatal(err)
	}
}

var _ *query.FluxRecord
