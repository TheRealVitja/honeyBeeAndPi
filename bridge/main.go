package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	_ "github.com/go-sql-driver/mysql"
)

type TelemetryPayload struct {
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

type ChannelTelemetry struct {
	ChannelIndex        int      `json:"channel_index"`
	ChannelName         string   `json:"channel_name"`
	HiveIndex           *int     `json:"hive_index"`
	HiveName            string   `json:"hive_name"`
	DOUTPin             *int     `json:"dout_pin"`
	SCKPin              *int     `json:"sck_pin"`
	Ready               *bool    `json:"ready"`
	Stable              *bool    `json:"stable"`
	DriftDetected       *bool    `json:"drift_detected"`
	Calibrated          *bool    `json:"calibrated"`
	Samples             *int     `json:"samples"`
	RawAvg              *float64 `json:"raw_avg"`
	RawMin              *float64 `json:"raw_min"`
	RawMax              *float64 `json:"raw_max"`
	RawStdDev           *float64 `json:"raw_stddev"`
	RawSlope            *float64 `json:"raw_slope"`
	WeightKG            *float64 `json:"weight_kg"`
	CompensatedWeightKG *float64 `json:"compensated_weight_kg"`
	CalScale            *float64 `json:"cal_scale"`
	CalOffset           *float64 `json:"cal_offset"`
}

type HiveTelemetry struct {
	HiveIndex           int      `json:"hive_index"`
	HiveName            string   `json:"hive_name"`
	ChannelCount        *int     `json:"channel_count"`
	WeightKG            *float64 `json:"weight_kg"`
	CompensatedWeightKG *float64 `json:"compensated_weight_kg"`
}

func getenvDefault(key, fallback string) string {
	v := strings.TrimSpace(os.Getenv(key))
	if v == "" {
		return fallback
	}
	return v
}

func mustOpenDB() *sql.DB {
	dsn := getenvDefault("MYSQL_DSN", "")
	if dsn == "" {
		log.Fatal("MYSQL_DSN is required")
	}

	db, err := sql.Open("mysql", dsn)
	if err != nil {
		log.Fatalf("open mysql: %v", err)
	}

	db.SetMaxOpenConns(10)
	db.SetMaxIdleConns(10)
	db.SetConnMaxLifetime(30 * time.Minute)

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	if err := db.PingContext(ctx); err != nil {
		log.Fatalf("ping mysql: %v", err)
	}

	return db
}

func parseEventTime(ts string) sql.NullTime {
	ts = strings.TrimSpace(ts)
	if ts == "" {
		return sql.NullTime{}
	}

	formats := []string{
		time.RFC3339Nano,
		time.RFC3339,
		"2006-01-02 15:04:05.999999",
		"2006-01-02 15:04:05",
	}

	for _, f := range formats {
		t, err := time.Parse(f, ts)
		if err == nil {
			return sql.NullTime{Time: t.UTC(), Valid: true}
		}
	}

	log.Printf("warn: cannot parse timestamp %q", ts)
	return sql.NullTime{}
}

func boolToNullInt(v *bool) sql.NullInt64 {
	if v == nil {
		return sql.NullInt64{}
	}
	if *v {
		return sql.NullInt64{Int64: 1, Valid: true}
	}
	return sql.NullInt64{Int64: 0, Valid: true}
}

func intToNullInt(v *int) sql.NullInt64 {
	if v == nil {
		return sql.NullInt64{}
	}
	return sql.NullInt64{Int64: int64(*v), Valid: true}
}

func floatToNull(v *float64) sql.NullFloat64 {
	if v == nil {
		return sql.NullFloat64{}
	}
	return sql.NullFloat64{Float64: *v, Valid: true}
}

func stringToNull(v string) sql.NullString {
	v = strings.TrimSpace(v)
	if v == "" {
		return sql.NullString{}
	}
	return sql.NullString{String: v, Valid: true}
}

func computeWeightKG(rawAvg, scale, offset *float64) *float64 {
	if rawAvg == nil || scale == nil || offset == nil {
		return nil
	}
	v := *scale**rawAvg + *offset
	return &v
}

func resolveWeightKG(device, bridge *float64) float64 {
	if device != nil && *device != 0 {
		return *device
	}
	if bridge != nil && *bridge != 0 {
		return *bridge
	}
	return 0
}

func insertPayload(ctx context.Context, db *sql.DB, payload TelemetryPayload, raw []byte) error {
	if payload.DeviceID == "" {
		return errors.New("device_id is required")
	}
	if payload.Schema != "" && payload.Schema != "telemetry.v4" {
		log.Printf("warn: unexpected schema %q for device %s", payload.Schema, payload.DeviceID)
	}

	eventTime := parseEventTime(payload.Timestamp)

	tx, err := db.BeginTx(ctx, &sql.TxOptions{})
	if err != nil {
		return err
	}
	defer func() {
		if err != nil {
			_ = tx.Rollback()
		}
	}()

	res, err := tx.ExecContext(ctx, `
		INSERT INTO device_telemetry (
			device_id, event_time, temperature_c, battery_v, rssi, sleep_seconds,
			firmware_version, active_hives, gts_start_year, gts_start_value, raw_payload
		)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		payload.DeviceID,
		eventTime,
		floatToNull(payload.TemperatureC),
		floatToNull(payload.BatteryV),
		intToNullInt(payload.RSSI),
		intToNullInt(payload.SleepSeconds),
		stringToNull(payload.FirmwareVersion),
		intToNullInt(payload.ActiveHives),
		intToNullInt(payload.GTSStartYear),
		floatToNull(payload.GTSStartValue),
		string(raw),
	)
	if err != nil {
		return err
	}

	deviceTelemetryID, err := res.LastInsertId()
	if err != nil {
		return err
	}

	for _, h := range payload.Hives {
		_, err = tx.ExecContext(ctx, `
			INSERT INTO hive_telemetry (
				device_telemetry_id, device_id, hive_index, hive_name, channel_count,
				weight_kg, compensated_weight_kg, event_time
			)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			deviceTelemetryID,
			payload.DeviceID,
			h.HiveIndex,
			stringToNull(h.HiveName),
			intToNullInt(h.ChannelCount),
			floatToNull(h.WeightKG),
			floatToNull(h.CompensatedWeightKG),
			eventTime,
		)
		if err != nil {
			return err
		}
	}

	for _, ch := range payload.Channels {
		bridgeWeight := computeWeightKG(ch.RawAvg, ch.CalScale, ch.CalOffset)
		resolvedWeight := resolveWeightKG(ch.WeightKG, bridgeWeight)
		_, err = tx.ExecContext(ctx, `
			INSERT INTO channel_telemetry (
				device_telemetry_id, device_id, channel_index, channel_name, hive_index, hive_name,
				dout_pin, sck_pin, ready_flag, stable_flag, drift_detected_flag, calibrated_flag,
				samples, raw_avg, raw_min, raw_max, raw_stddev, raw_slope, weight_kg,
				compensated_weight_kg, event_time
			)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			deviceTelemetryID,
			payload.DeviceID,
			ch.ChannelIndex,
			stringToNull(ch.ChannelName),
			intToNullInt(ch.HiveIndex),
			stringToNull(ch.HiveName),
			intToNullInt(ch.DOUTPin),
			intToNullInt(ch.SCKPin),
			boolToNullInt(ch.Ready),
			boolToNullInt(ch.Stable),
			boolToNullInt(ch.DriftDetected),
			boolToNullInt(ch.Calibrated),
			intToNullInt(ch.Samples),
			floatToNull(ch.RawAvg),
			floatToNull(ch.RawMin),
			floatToNull(ch.RawMax),
			floatToNull(ch.RawStdDev),
			floatToNull(ch.RawSlope),
			resolvedWeight,
			floatToNull(ch.CompensatedWeightKG),
			eventTime,
		)
		if err != nil {
			return err
		}
	}

	return tx.Commit()
}

func main() {
	db := mustOpenDB()
	defer db.Close()

	mqttBroker := getenvDefault("MQTT_BROKER", "tcp://localhost:1883")
	mqttTopic := getenvDefault("MQTT_TOPIC", "devices/+/telemetry")
	clientID := "mysql-bridge-" + time.Now().UTC().Format("20060102T150405.000000000")

	opts := mqtt.NewClientOptions()
	opts.AddBroker(mqttBroker)
	opts.SetClientID(clientID)
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetKeepAlive(60 * time.Second)
	opts.SetConnectTimeout(15 * time.Second)
	opts.SetOrderMatters(false)
	opts.SetOnConnectHandler(func(c mqtt.Client) {
		log.Printf("mqtt connected, subscribing to %s", mqttTopic)
		if token := c.Subscribe(mqttTopic, 0, func(_ mqtt.Client, msg mqtt.Message) {
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()

			var payload TelemetryPayload
			if err := json.Unmarshal(msg.Payload(), &payload); err != nil {
				log.Printf("invalid payload on topic %s: %v", msg.Topic(), err)
				return
			}

			if err := insertPayload(ctx, db, payload, msg.Payload()); err != nil {
				log.Printf("insert payload failed device=%s topic=%s: %v", payload.DeviceID, msg.Topic(), err)
				return
			}

			log.Printf("stored payload device=%s topic=%s bytes=%d", payload.DeviceID, msg.Topic(), len(msg.Payload()))
		}); token.Wait() && token.Error() != nil {
			log.Printf("subscribe failed: %v", token.Error())
		}
	})
	opts.SetConnectionLostHandler(func(_ mqtt.Client, err error) {
		log.Printf("mqtt connection lost: %v", err)
	})

	client := mqtt.NewClient(opts)
	if token := client.Connect(); token.Wait() && token.Error() != nil {
		log.Fatalf("mqtt connect failed: %v", token.Error())
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	sig := <-sigCh
	log.Printf("shutdown signal received: %s", sig.String())

	client.Disconnect(250)
}
