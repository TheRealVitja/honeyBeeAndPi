package main

import (
	"context"
	"database/sql"
	"log"
	"os"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

type TrendRow struct {
	DeviceID      string
	HiveIndex     int
	HiveName      sql.NullString
	TrendDate     time.Time
	WeightFirstKG sql.NullFloat64
	WeightLastKG  sql.NullFloat64
	DeltaDayKG    sql.NullFloat64
	Trend24hKG    sql.NullFloat64
	Trend48hKG    sql.NullFloat64
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

func computeTrends(ctx context.Context, db *sql.DB) error {
	rows, err := db.QueryContext(ctx, `
		WITH daily AS (
			SELECT
				device_id,
				hive_index,
				DATE(event_time) AS trend_date,
				ANY_VALUE(hive_name) AS hive_name,
				SUBSTRING_INDEX(
					GROUP_CONCAT(weight_kg ORDER BY event_time ASC SEPARATOR ','),
					',', 1
				) AS weight_first_kg,
				SUBSTRING_INDEX(
					GROUP_CONCAT(weight_kg ORDER BY event_time DESC SEPARATOR ','),
					',', 1
				) AS weight_last_kg
			FROM hive_telemetry
			WHERE event_time IS NOT NULL
			  AND weight_kg IS NOT NULL
			GROUP BY device_id, hive_index, DATE(event_time)
		)
		SELECT
			d1.device_id,
			d1.hive_index,
			d1.hive_name,
			d1.trend_date,
			CAST(d1.weight_first_kg AS DOUBLE),
			CAST(d1.weight_last_kg AS DOUBLE),
			CAST(d1.weight_last_kg AS DOUBLE) - CAST(d1.weight_first_kg AS DOUBLE) AS delta_day_kg,
			CAST(d1.weight_last_kg AS DOUBLE) - CAST(d2.weight_last_kg AS DOUBLE) AS trend_24h_kg,
			CAST(d1.weight_last_kg AS DOUBLE) - CAST(d3.weight_last_kg AS DOUBLE) AS trend_48h_kg
		FROM daily d1
		LEFT JOIN daily d2
		  ON d2.device_id = d1.device_id
		 AND d2.hive_index = d1.hive_index
		 AND d2.trend_date = DATE_SUB(d1.trend_date, INTERVAL 1 DAY)
		LEFT JOIN daily d3
		  ON d3.device_id = d1.device_id
		 AND d3.hive_index = d1.hive_index
		 AND d3.trend_date = DATE_SUB(d1.trend_date, INTERVAL 2 DAY)
		ORDER BY d1.device_id, d1.hive_index, d1.trend_date`)
	if err != nil {
		return err
	}
	defer rows.Close()

	var items []TrendRow
	for rows.Next() {
		var t TrendRow
		if err := rows.Scan(&t.DeviceID, &t.HiveIndex, &t.HiveName, &t.TrendDate, &t.WeightFirstKG, &t.WeightLastKG, &t.DeltaDayKG, &t.Trend24hKG, &t.Trend48hKG); err != nil {
			return err
		}
		items = append(items, t)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer func() { if err != nil { _ = tx.Rollback() } }()

	for _, it := range items {
		_, err = tx.ExecContext(ctx, `
			INSERT INTO hive_trend_daily (
				device_id, hive_index, hive_name, trend_date, weight_first_kg, weight_last_kg,
				delta_day_kg, trend_24h_kg, trend_48h_kg
			) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
			ON DUPLICATE KEY UPDATE
				hive_name = VALUES(hive_name),
				weight_first_kg = VALUES(weight_first_kg),
				weight_last_kg = VALUES(weight_last_kg),
				delta_day_kg = VALUES(delta_day_kg),
				trend_24h_kg = VALUES(trend_24h_kg),
				trend_48h_kg = VALUES(trend_48h_kg)`,
			it.DeviceID,
			it.HiveIndex,
			it.HiveName,
			it.TrendDate.Format("2006-01-02"),
			it.WeightFirstKG,
			it.WeightLastKG,
			it.DeltaDayKG,
			it.Trend24hKG,
			it.Trend48hKG,
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

	interval := parseDurationEnv("TREND_INTERVAL", "5m")
	log.Printf("trend-worker started interval=%s", interval)

	for {
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		err := computeTrends(ctx, db)
		cancel()
		if err != nil {
			log.Printf("trend compute failed: %v", err)
		} else {
			log.Printf("trend compute finished")
		}
		time.Sleep(interval)
	}
}