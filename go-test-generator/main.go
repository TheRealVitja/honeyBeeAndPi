package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"math/rand"
	"os"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type ChannelTelemetry struct {
	ChannelIndex        int      `json:"channel_index"`
	ChannelName         string   `json:"channel_name"`
	HiveIndex           int      `json:"hive_index"`
	HiveName            string   `json:"hive_name"`
	WeightKG            *float64 `json:"weight_kg,omitempty"`
	CompensatedWeightKG *float64 `json:"compensated_weight_kg,omitempty"`
	Ready               *bool    `json:"ready,omitempty"`
	Stable              *bool    `json:"stable,omitempty"`
	DriftDetected       *bool    `json:"drift_detected,omitempty"`
	Calibrated          *bool    `json:"calibrated,omitempty"`
}

type HiveTelemetry struct {
	HiveIndex           int      `json:"hive_index"`
	HiveName            string   `json:"hive_name"`
	ChannelCount        *int     `json:"channel_count,omitempty"`
	WeightKG            *float64 `json:"weight_kg,omitempty"`
	CompensatedWeightKG *float64 `json:"compensated_weight_kg,omitempty"`
}

type Payload struct {
	Schema          string             `json:"schema"`
	DeviceID        string             `json:"device_id"`
	Timestamp       string             `json:"timestamp"`
	TemperatureC    *float64           `json:"temperature_c,omitempty"`
	BatteryV        *float64           `json:"battery_v,omitempty"`
	RSSI            *int               `json:"rssi,omitempty"`
	SleepSeconds    *int               `json:"sleep_seconds,omitempty"`
	FirmwareVersion string             `json:"firmware_version,omitempty"`
	ActiveHives     *int               `json:"active_hives,omitempty"`
	GTSStartYear    *int               `json:"gts_start_year,omitempty"`
	GTSStartValue   *float64           `json:"gts_start_value,omitempty"`
	Channels        []ChannelTelemetry `json:"channels"`
	Hives           []HiveTelemetry    `json:"hives"`
}

type Config struct {
	StartDate       string
	Days            int
	Devices         int
	HivesPerDevice  int
	ChannelsPerHive int
	Prefix          string
	Output          string
	MQTTBroker      string
	Publish         bool
	TopicTemplate   string
	Seed            int64
	Timezone        string
	BaseWeight      float64
	TempCompPerC    float64
	SetGTSStart     bool
	GTSStartValue   float64
}

func boolPtr(v bool) *bool        { return &v }
func intPtr(v int) *int           { return &v }
func floatPtr(v float64) *float64 { return &v }

func topicForDevice(template, deviceID string) string {
	if template == "" {
		return "devices/" + deviceID + "/telemetry"
	}
	return stringsReplaceAll(template, "{device_id}", deviceID)
}

func stringsReplaceAll(s, old, new string) string {
	for {
		i := indexOf(s, old)
		if i < 0 {
			return s
		}
		s = s[:i] + new + s[i+len(old):]
	}
}

func indexOf(s, sub string) int {
	if len(sub) == 0 {
		return 0
	}
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

func seasonalTemperature(dayOfYear int, rnd *rand.Rand) float64 {
	base := 10.0 + 10.0*math.Sin((2.0*math.Pi/365.0)*(float64(dayOfYear)-110.0))
	springBoost := 2.0 * math.Sin((2.0*math.Pi/365.0)*(float64(dayOfYear)-40.0))
	n := (rnd.Float64()*2 - 1) * 2.0
	return math.Round((base+springBoost+n)*10) / 10
}

func dailyDeltaKG(day int, hive int, temp float64, rnd *rand.Rand) float64 {
	season := 0.3 + 0.5*math.Sin((2.0*math.Pi/365.0)*(float64(day)-90.0))
	tempEffect := math.Max(temp, 0) * 0.015
	base := season*(0.15+rnd.Float64()*0.6) + tempEffect
	noise := (rnd.Float64()*2 - 1) * 0.12
	if rnd.Float64() < 0.08 {
		base += 0.5 + rnd.Float64()*0.8
	}
	if rnd.Float64() < 0.12 {
		base -= 0.15 + rnd.Float64()*0.45
	}
	val := base + noise + float64(hive)*0.03
	return math.Round(val*1000) / 1000
}

func generate(cfg Config) ([]Payload, error) {
	loc, err := time.LoadLocation(cfg.Timezone)
	if err != nil {
		return nil, err
	}
	start, err := time.ParseInLocation("2006-01-02", cfg.StartDate, loc)
	if err != nil {
		return nil, err
	}

	rnd := rand.New(rand.NewSource(cfg.Seed))
	var payloads []Payload

	for d := 0; d < cfg.Devices; d++ {
		deviceID := fmt.Sprintf("%s-%02d", cfg.Prefix, d+1)
		hiveWeights := make([]float64, cfg.HivesPerDevice)
		for h := 0; h < cfg.HivesPerDevice; h++ {
			hiveWeights[h] = cfg.BaseWeight + float64(h)*1.3 + rnd.Float64()*2.0
		}

		for day := 0; day < cfg.Days; day++ {
			ts := start.AddDate(0, 0, day).Add(12 * time.Hour)
			temp := seasonalTemperature(ts.YearDay(), rnd)
			battery := 4.15 - float64(day)*0.002
			if battery < 3.55 {
				battery = 3.55 + rnd.Float64()*0.05
			}
			rssi := -55 - rnd.Intn(20)
			sleep := 300
			activeHives := cfg.HivesPerDevice

			var channels []ChannelTelemetry
			var hives []HiveTelemetry

			for h := 0; h < cfg.HivesPerDevice; h++ {
				delta := dailyDeltaKG(ts.YearDay(), h, temp, rnd)
				hiveWeights[h] += delta

				rawWeight := math.Round(hiveWeights[h]*1000) / 1000
				compWeight := math.Round((rawWeight-(temp-20.0)*cfg.TempCompPerC)*1000) / 1000

				channelCount := cfg.ChannelsPerHive
				channelWeights := make([]float64, channelCount)
				remaining := rawWeight

				for c := 0; c < channelCount; c++ {
					var cw float64
					if c == channelCount-1 {
						cw = remaining
					} else {
						part := rawWeight/float64(channelCount) + (rnd.Float64()*2-1)*0.15
						cw = math.Round(part*1000) / 1000
						remaining -= cw
					}
					channelWeights[c] = cw
				}

				for c := 0; c < channelCount; c++ {
					compCh := math.Round((channelWeights[c]-(temp-20.0)*cfg.TempCompPerC/float64(channelCount))*1000) / 1000
					channels = append(channels, ChannelTelemetry{
						ChannelIndex:        h*channelCount + c,
						ChannelName:         fmt.Sprintf("Kanal %d", h*channelCount+c),
						HiveIndex:           h,
						HiveName:            fmt.Sprintf("Beute %d", h),
						WeightKG:            floatPtr(channelWeights[c]),
						CompensatedWeightKG: floatPtr(compCh),
						Ready:               boolPtr(true),
						Stable:              boolPtr(true),
						DriftDetected:       boolPtr(false),
						Calibrated:          boolPtr(true),
					})
				}

				hives = append(hives, HiveTelemetry{
					HiveIndex:           h,
					HiveName:            fmt.Sprintf("Beute %d", h),
					ChannelCount:        intPtr(channelCount),
					WeightKG:            floatPtr(rawWeight),
					CompensatedWeightKG: floatPtr(compWeight),
				})
			}

			payload := Payload{
				Schema:          "telemetry.v4",
				DeviceID:        deviceID,
				Timestamp:       ts.UTC().Format(time.RFC3339),
				TemperatureC:    floatPtr(temp),
				BatteryV:        floatPtr(math.Round(battery*1000) / 1000),
				RSSI:            intPtr(rssi),
				SleepSeconds:    intPtr(sleep),
				FirmwareVersion: "combined-generator-v1",
				ActiveHives:     intPtr(activeHives),
				Channels:        channels,
				Hives:           hives,
			}

			if cfg.SetGTSStart && day == 0 {
				year := ts.Year()
				value := cfg.GTSStartValue
				payload.GTSStartYear = &year
				payload.GTSStartValue = &value
			}

			payloads = append(payloads, payload)
		}
	}

	return payloads, nil
}

func writeJSONFile(path string, payloads []Payload) error {
	b, err := json.MarshalIndent(payloads, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, b, 0o644)
}

func publishMQTT(broker string, topicTemplate string, payloads []Payload) error {
	opts := mqtt.NewClientOptions()
	opts.AddBroker(broker)
	opts.SetClientID(fmt.Sprintf("combined-generator-%d", time.Now().UnixNano()))
	client := mqtt.NewClient(opts)
	tok := client.Connect()
	tok.Wait()
	if err := tok.Error(); err != nil {
		return err
	}
	defer client.Disconnect(250)

	for _, p := range payloads {
		b, err := json.Marshal(p)
		if err != nil {
			return err
		}
		topic := topicForDevice(topicTemplate, p.DeviceID)
		t := client.Publish(topic, 0, false, b)
		t.Wait()
		if err := t.Error(); err != nil {
			return err
		}
	}
	return nil
}

func main() {
	var cfg Config

	flag.StringVar(&cfg.StartDate, "start-date", "2026-01-01", "start date in YYYY-MM-DD")
	flag.IntVar(&cfg.Days, "days", 120, "number of days to generate")
	flag.IntVar(&cfg.Devices, "devices", 2, "number of devices")
	flag.IntVar(&cfg.HivesPerDevice, "hives-per-device", 2, "number of hives per device")
	flag.IntVar(&cfg.ChannelsPerHive, "channels-per-hive", 2, "channels per hive")
	flag.StringVar(&cfg.Prefix, "prefix", "esp32", "device prefix")
	flag.StringVar(&cfg.Output, "output", "combined_testdata.json", "output json file")
	flag.StringVar(&cfg.MQTTBroker, "mqtt-broker", "tcp://localhost:1883", "MQTT broker URL")
	flag.BoolVar(&cfg.Publish, "publish", false, "publish generated payloads to MQTT")
	flag.StringVar(&cfg.TopicTemplate, "topic-template", "devices/{device_id}/telemetry", "MQTT topic template")
	flag.Int64Var(&cfg.Seed, "seed", 42, "random seed")
	flag.StringVar(&cfg.Timezone, "timezone", "Europe/Berlin", "timezone")
	flag.Float64Var(&cfg.BaseWeight, "base-weight", 24.0, "base hive weight in kg")
	flag.Float64Var(&cfg.TempCompPerC, "temp-comp-per-c", 0.02, "temperature compensation in kg/°C on hive level")
	flag.BoolVar(&cfg.SetGTSStart, "set-gts-start", true, "include gts_start_year and gts_start_value on first day")
	flag.Float64Var(&cfg.GTSStartValue, "gts-start-value", 0.0, "GTS start value for first day")
	flag.Parse()

	payloads, err := generate(cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "generate failed: %v\n", err)
		os.Exit(1)
	}

	if err := writeJSONFile(cfg.Output, payloads); err != nil {
		fmt.Fprintf(os.Stderr, "write output failed: %v\n", err)
		os.Exit(1)
	}

	if cfg.Publish {

		if err := publishMQTT(cfg.MQTTBroker, cfg.TopicTemplate, payloads); err != nil {
			fmt.Fprintf(os.Stderr, "mqtt publish failed: %v\n", err)
			os.Exit(1)
		}
	}

	fmt.Printf("generated %d combined payloads into %s\n", len(payloads), cfg.Output)
	if cfg.Publish {
		fmt.Printf("published to %s using topic template %s\n", cfg.MQTTBroker, cfg.TopicTemplate)
	}
}
