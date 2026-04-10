package main

import (
	"context"
	"encoding/json"
	"log"
	"os"
	"strconv"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	influxdb2 "github.com/influxdata/influxdb-client-go/v2"
	"github.com/influxdata/influxdb-client-go/v2/api"
	"github.com/influxdata/influxdb-client-go/v2/api/write"
)

type ChannelTelemetry struct {
	ChannelIndex        int      `json:"channel_index"`
	ChannelName         string   `json:"channel_name"`
	HiveIndex           int      `json:"hive_index"`
	HiveName            string   `json:"hive_name"`
	DOUTPin             *int     `json:"dout_pin"`
	SCKPin              *int     `json:"sck_pin"`
	WeightKG            *float64 `json:"weight_kg"`
	CompensatedWeightKG *float64 `json:"compensated_weight_kg"`
	RawAvg              *float64 `json:"raw_avg"`
	RawMin              *float64 `json:"raw_min"`
	RawMax              *float64 `json:"raw_max"`
	RawStdDev           *float64 `json:"raw_stddev"`
	RawSlope            *float64 `json:"raw_slope"`
	Samples             *int     `json:"samples"`
	Stable              *bool    `json:"stable"`
	DriftDetected       *bool    `json:"drift_detected"`
	Calibrated          *bool    `json:"calibrated"`
	Ready               *bool    `json:"ready"`
}

type HiveTelemetry struct {
	HiveIndex           int      `json:"hive_index"`
	HiveName            string   `json:"hive_name"`
	ChannelCount        *int     `json:"channel_count"`
	WeightKG            *float64 `json:"weight_kg"`
	CompensatedWeightKG *float64 `json:"compensated_weight_kg"`
}

type Payload struct {
	Schema          string             `json:"schema"`
	DeviceID        string             `json:"device_id"`
	Timestamp       string             `json:"timestamp"`
	TemperatureC    *float64           `json:"temperature_c"`
	BatteryV        *float64           `json:"battery_v"`
	RSSI            *int               `json:"rssi"`
	SleepSeconds    *int               `json:"sleep_seconds"`
	FirmwareVersion string             `json:"firmware_version"`
	ActiveHives     *int               `json:"active_hives"`
	GTSStartYear    *int               `json:"gts_start_year"`
	GTSStartValue   *float64           `json:"gts_start_value"`
	Channels        []ChannelTelemetry `json:"channels"`
	Hives           []HiveTelemetry    `json:"hives"`
}

func addFloatField(fields map[string]interface{}, key string, v *float64) {
	if v != nil {
		fields[key] = *v
	}
}

func addIntField(fields map[string]interface{}, key string, v *int) {
	if v != nil {
		fields[key] = *v
	}
}

func addBoolField(fields map[string]interface{}, key string, v *bool) {
	if v != nil {
		if *v {
			fields[key] = 1
		} else {
			fields[key] = 0
		}
	}
}

func parseTimestamp(ts string) time.Time {
	if ts == "" {
		return time.Now().UTC()
	}
	t, err := time.Parse(time.RFC3339, ts)
	if err != nil {
		return time.Now().UTC()
	}
	return t.UTC()
}

func writeDevicePoint(writeAPI api.WriteAPIBlocking, p Payload, ts time.Time) error {
	fields := map[string]interface{}{
		"present": 1,
	}
	addFloatField(fields, "temperature_c", p.TemperatureC)
	addFloatField(fields, "battery_v", p.BatteryV)
	addIntField(fields, "rssi", p.RSSI)
	addIntField(fields, "sleep_seconds", p.SleepSeconds)
	addIntField(fields, "active_hives", p.ActiveHives)
	addIntField(fields, "gts_start_year", p.GTSStartYear)
	addFloatField(fields, "gts_start_value", p.GTSStartValue)

	if p.FirmwareVersion != "" {
		fields["firmware_version_present"] = 1
	}

	point := write.NewPoint(
		"device_telemetry",
		map[string]string{
			"device_id": p.DeviceID,
			"schema":    p.Schema,
		},
		fields,
		ts,
	)
	return writeAPI.WritePoint(context.Background(), point)
}

func writeChannelPoints(writeAPI api.WriteAPIBlocking, p Payload, ts time.Time) error {
	for _, ch := range p.Channels {
		fields := map[string]interface{}{
			"present": 1,
		}
		addFloatField(fields, "weight_kg", ch.WeightKG)
		addFloatField(fields, "compensated_weight_kg", ch.CompensatedWeightKG)
		addFloatField(fields, "raw_avg", ch.RawAvg)
		addFloatField(fields, "raw_min", ch.RawMin)
		addFloatField(fields, "raw_max", ch.RawMax)
		addFloatField(fields, "raw_stddev", ch.RawStdDev)
		addFloatField(fields, "raw_slope", ch.RawSlope)
		addIntField(fields, "samples", ch.Samples)
		addBoolField(fields, "stable", ch.Stable)
		addBoolField(fields, "drift_detected", ch.DriftDetected)
		addBoolField(fields, "calibrated", ch.Calibrated)
		addBoolField(fields, "ready", ch.Ready)
		if ch.DOUTPin != nil {
			fields["dout_pin"] = *ch.DOUTPin
		}
		if ch.SCKPin != nil {
			fields["sck_pin"] = *ch.SCKPin
		}

		point := write.NewPoint(
			"channel_telemetry",
			map[string]string{
				"device_id":     p.DeviceID,
				"schema":        p.Schema,
				"channel_index": strconv.Itoa(ch.ChannelIndex),
				"channel_name":  ch.ChannelName,
				"hive_index":    strconv.Itoa(ch.HiveIndex),
				"hive_name":     ch.HiveName,
			},
			fields,
			ts,
		)
		if err := writeAPI.WritePoint(context.Background(), point); err != nil {
			return err
		}
	}
	return nil
}

func writeHivePoints(writeAPI api.WriteAPIBlocking, p Payload, ts time.Time) error {
	for _, hive := range p.Hives {
		fields := map[string]interface{}{
			"present": 1,
		}
		addFloatField(fields, "weight_kg", hive.WeightKG)
		addFloatField(fields, "compensated_weight_kg", hive.CompensatedWeightKG)
		addIntField(fields, "channel_count", hive.ChannelCount)

		point := write.NewPoint(
			"hive_telemetry",
			map[string]string{
				"device_id":  p.DeviceID,
				"schema":     p.Schema,
				"hive_index": strconv.Itoa(hive.HiveIndex),
				"hive_name":  hive.HiveName,
			},
			fields,
			ts,
		)
		if err := writeAPI.WritePoint(context.Background(), point); err != nil {
			return err
		}
	}
	return nil
}

func main() {
	mqttBroker := os.Getenv("MQTT_BROKER")
	if mqttBroker == "" {
		mqttBroker = "tcp://localhost:1883"
	}

	mqttTopic := os.Getenv("MQTT_TOPIC")
	if mqttTopic == "" {
		mqttTopic = "devices/+/telemetry"
	}

	influxURL := os.Getenv("INFLUX_URL")
	influxToken := os.Getenv("INFLUX_TOKEN")
	influxOrg := os.Getenv("INFLUX_ORG")
	influxBucket := os.Getenv("INFLUX_BUCKET")

	if influxURL == "" || influxToken == "" || influxOrg == "" || influxBucket == "" {
		log.Fatal("missing influx environment variables")
	}

	influxClient := influxdb2.NewClient(influxURL, influxToken)
	defer influxClient.Close()

	writeAPI := influxClient.WriteAPIBlocking(influxOrg, influxBucket)

	opts := mqtt.NewClientOptions()
	opts.AddBroker(mqttBroker)
	opts.SetClientID("iot-bridge-v4-gts")
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetCleanSession(true)

	client := mqtt.NewClient(opts)
	token := client.Connect()
	token.Wait()
	if err := token.Error(); err != nil {
		log.Fatalf("mqtt connect: %v", err)
	}

	if sub := client.Subscribe(mqttTopic, 0, func(c mqtt.Client, m mqtt.Message) {
		var p Payload
		if err := json.Unmarshal(m.Payload(), &p); err != nil {
			log.Printf("invalid payload on %s: %v", m.Topic(), err)
			return
		}

		if p.Schema != "telemetry.v4" {
			log.Printf("skip unsupported schema=%q on %s", p.Schema, m.Topic())
			return
		}

		ts := parseTimestamp(p.Timestamp)

		if err := writeDevicePoint(writeAPI, p, ts); err != nil {
			log.Printf("write device point failed: %v", err)
			return
		}
		if err := writeChannelPoints(writeAPI, p, ts); err != nil {
			log.Printf("write channel point failed: %v", err)
			return
		}
		if err := writeHivePoints(writeAPI, p, ts); err != nil {
			log.Printf("write hive point failed: %v", err)
			return
		}

		log.Printf("ingested device=%s channels=%d hives=%d gts_start_year=%v gts_start_value=%v", p.DeviceID, len(p.Channels), len(p.Hives), p.GTSStartYear, p.GTSStartValue)
	}); sub.Wait() && sub.Error() != nil {
		log.Fatalf("mqtt subscribe: %v", sub.Error())
	}

	select {}
}
