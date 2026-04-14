CREATE TABLE IF NOT EXISTS device_telemetry (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_id VARCHAR(128) NOT NULL,
  event_time DATETIME(6) NULL,
  temperature_c DOUBLE NULL,
  battery_v DOUBLE NULL,
  rssi INT NULL,
  sleep_seconds INT NULL,
  firmware_version VARCHAR(128) NULL,
  active_hives INT NULL,
  gts_start_year INT NULL,
  gts_start_value DOUBLE NULL,
  raw_payload JSON NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_device_time (device_id, event_time),
  KEY idx_created_at (created_at)
);

CREATE TABLE IF NOT EXISTS hive_telemetry (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_telemetry_id BIGINT UNSIGNED NOT NULL,
  device_id VARCHAR(128) NOT NULL,
  hive_index INT NOT NULL,
  hive_name VARCHAR(128) NULL,
  channel_count INT NULL,
  weight_kg DOUBLE NULL,
  compensated_weight_kg DOUBLE NULL,
  event_time DATETIME(6) NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  CONSTRAINT fk_hive_device_telemetry
    FOREIGN KEY (device_telemetry_id) REFERENCES device_telemetry(id)
    ON DELETE CASCADE,
  KEY idx_hive_device_time (device_id, hive_index, event_time)
);

CREATE TABLE IF NOT EXISTS channel_telemetry (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_telemetry_id BIGINT UNSIGNED NOT NULL,
  device_id VARCHAR(128) NOT NULL,
  channel_index INT NOT NULL,
  channel_name VARCHAR(128) NULL,
  hive_index INT NULL,
  hive_name VARCHAR(128) NULL,
  dout_pin INT NULL,
  sck_pin INT NULL,
  ready_flag TINYINT(1) NULL,
  stable_flag TINYINT(1) NULL,
  drift_detected_flag TINYINT(1) NULL,
  calibrated_flag TINYINT(1) NULL,
  samples INT NULL,
  raw_avg DOUBLE NULL,
  raw_min DOUBLE NULL,
  raw_max DOUBLE NULL,
  raw_stddev DOUBLE NULL,
  raw_slope DOUBLE NULL,
  weight_kg DOUBLE NULL,
  compensated_weight_kg DOUBLE NULL,
  event_time DATETIME(6) NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  CONSTRAINT fk_channel_device_telemetry
    FOREIGN KEY (device_telemetry_id) REFERENCES device_telemetry(id)
    ON DELETE CASCADE,
  KEY idx_channel_device_time (device_id, channel_index, event_time)
);

CREATE TABLE IF NOT EXISTS gts_daily (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_id VARCHAR(128) NOT NULL,
  gts_year INT NOT NULL,
  gts_date DATE NOT NULL,
  mean_temperature_c DOUBLE NULL,
  contribution_c DOUBLE NOT NULL DEFAULT 0,
  gts_value DOUBLE NOT NULL,
  source_count INT NOT NULL DEFAULT 0,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uq_gts_device_date (device_id, gts_date),
  KEY idx_gts_device_year_date (device_id, gts_year, gts_date)
);

CREATE TABLE IF NOT EXISTS hive_trend_daily (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  device_id VARCHAR(128) NOT NULL,
  hive_index INT NOT NULL,
  hive_name VARCHAR(128) NULL,
  trend_date DATE NOT NULL,
  weight_first_kg DOUBLE NULL,
  weight_last_kg DOUBLE NULL,
  delta_day_kg DOUBLE NULL,
  trend_24h_kg DOUBLE NULL,
  trend_48h_kg DOUBLE NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uq_hive_trend_date (device_id, hive_index, trend_date),
  KEY idx_hive_trend_device_date (device_id, hive_index, trend_date)
);

CREATE OR REPLACE VIEW latest_hive_telemetry AS
SELECT h.*
FROM hive_telemetry h
JOIN (
  SELECT device_id, hive_index, MAX(event_time) AS max_event_time
  FROM hive_telemetry
  GROUP BY device_id, hive_index
) x
  ON x.device_id = h.device_id
 AND x.hive_index = h.hive_index
 AND x.max_event_time = h.event_time;

CREATE OR REPLACE VIEW latest_device_telemetry AS
SELECT d.*
FROM device_telemetry d
JOIN (
  SELECT device_id, MAX(event_time) AS max_event_time
  FROM device_telemetry
  GROUP BY device_id
) x
  ON x.device_id = d.device_id
 AND x.max_event_time = d.event_time;

CREATE OR REPLACE VIEW latest_gts_daily AS
SELECT g.*
FROM gts_daily g
JOIN (
  SELECT device_id, MAX(gts_date) AS max_gts_date
  FROM gts_daily
  GROUP BY device_id
) x
  ON x.device_id = g.device_id
 AND x.max_gts_date = g.gts_date;

CREATE OR REPLACE VIEW latest_hive_trend_daily AS
SELECT t.*
FROM hive_trend_daily t
JOIN (
  SELECT device_id, hive_index, MAX(trend_date) AS max_trend_date
  FROM hive_trend_daily
  GROUP BY device_id, hive_index
) x
  ON x.device_id = t.device_id
 AND x.hive_index = t.hive_index
 AND x.max_trend_date = t.trend_date;
