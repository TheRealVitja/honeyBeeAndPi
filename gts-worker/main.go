package main

import (
	"context"
	"database/sql"
	"log"
	"os"
	"strconv"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

type TempDay struct {
	DeviceID    string
	GTSDate     time.Time
	MeanTemp    sql.NullFloat64
	SourceCount int
	StartYear   sql.NullInt64
	StartValue  sql.NullFloat64
}

func getenvDefault(key, fallback string) string {
	v := strings.TrimSpace(os.Getenv(key))
	if v == "" {
		return fallback
	}
	return v
}

func parseDurationEnv(key, fallback string) time.Duration {
	v := getenvDefault(key, fallback)
	d, err := time.ParseDuration(v)
	if err != nil {
		log.Fatalf("invalid duration %s=%q: %v", key, v, err)
	}
	return d
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
	if err := db.Ping(); err != nil {
		log.Fatalf("ping mysql: %v", err)
	}
	return db
}

func upsertGTS(ctx context.Context, db *sql.DB) error {
	rows, err := db.QueryContext(ctx, `
		SELECT
			d.device_id,
			DATE(CONVERT_TZ(d.event_time, '+00:00', '+00:00')) AS gts_date,
			AVG(d.temperature_c) AS mean_temp,
			COUNT(*) AS source_count,
			MAX(d.gts_start_year) AS start_year,
			MAX(d.gts_start_value) AS start_value
		FROM device_telemetry d
		WHERE d.event_time IS NOT NULL
		  AND d.temperature_c IS NOT NULL
		GROUP BY d.device_id, DATE(CONVERT_TZ(d.event_time, '+00:00', '+00:00'))
		ORDER BY d.device_id, gts_date`)
	if err != nil {
		return err
	}
	defer rows.Close()

	type item struct {
		DeviceID    string
		GTSDate     time.Time
		MeanTemp    float64
		SourceCount int
		StartYear   sql.NullInt64
		StartValue  sql.NullFloat64
	}
	var items []item
	for rows.Next() {
		var d TempDay
		if err := rows.Scan(&d.DeviceID, &d.GTSDate, &d.MeanTemp, &d.SourceCount, &d.StartYear, &d.StartValue); err != nil {
			return err
		}
		mean := 0.0
		if d.MeanTemp.Valid {
			mean = d.MeanTemp.Float64
		}
		items = append(items, item{
			DeviceID: d.DeviceID, GTSDate: d.GTSDate, MeanTemp: mean, SourceCount: d.SourceCount,
			StartYear: d.StartYear, StartValue: d.StartValue,
		})
	}
	if err := rows.Err(); err != nil {
		return err
	}

	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() {
		if err != nil {
			_ = tx.Rollback()
		}
	}()

	lastValueByDeviceYear := map[string]float64{}
	startOverrideByDeviceYear := map[string]float64{}

	for _, it := range items {
		year := it.GTSDate.Year()
		key := it.DeviceID + "|" + strconv.Itoa(year)

		if it.StartYear.Valid && int(it.StartYear.Int64) == year && it.StartValue.Valid {
			startOverrideByDeviceYear[key] = it.StartValue.Float64
		}

		prev, ok := lastValueByDeviceYear[key]
		if !ok {
			if v, has := startOverrideByDeviceYear[key]; has {
				prev = v
			} else {
				prev = 0
			}
		}

		contribution := it.MeanTemp
		if contribution < 0 {
			contribution = 0
		}
		gtsValue := prev + contribution
		lastValueByDeviceYear[key] = gtsValue

		_, err = tx.ExecContext(ctx, `
			INSERT INTO gts_daily (
				device_id, gts_year, gts_date, mean_temperature_c, contribution_c, gts_value, source_count
			) VALUES (?, ?, ?, ?, ?, ?, ?)
			ON DUPLICATE KEY UPDATE
				mean_temperature_c = VALUES(mean_temperature_c),
				contribution_c = VALUES(contribution_c),
				gts_value = VALUES(gts_value),
				source_count = VALUES(source_count)`,
			it.DeviceID, year, it.GTSDate.Format("2006-01-02"), it.MeanTemp, contribution, gtsValue, it.SourceCount,
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

	interval := parseDurationEnv("GTS_INTERVAL", "5m")
	log.Printf("gts-worker started interval=%s", interval)

	for {
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		err := upsertGTS(ctx, db)
		cancel()
		if err != nil {
			log.Printf("gts upsert failed: %v", err)
		} else {
			log.Printf("gts upsert finished")
		}
		time.Sleep(interval)
	}
}
