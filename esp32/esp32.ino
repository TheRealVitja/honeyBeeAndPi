#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <DNSServer.h>
#include <HX711.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <ArduinoOTA.h>
#include <time.h>

// =========================
// ESP32 IoT Telemetry v7
// - dynamic channels (up to 8 HX711)
// - logical hives (up to 4)
// - setup AP + config portal
// - multi-point calibration per channel
// - temp compensation per channel
// - topic derived from device_id
// - configurable OTA window
// - MQTT/WLAN test before save
// =========================

static const int MAX_CHANNELS = 8;
static const int MAX_HIVES = 4;
static const int MAX_CAL_POINTS = 5;

static const byte DNS_PORT = 53;
static const char* AP_SSID = "Waagen-Setup";
static const char* AP_PASS = "setup";
static const unsigned long CONFIG_TIMEOUT_MS = 10UL * 60UL * 1000UL;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;
static const unsigned long MQTT_CONNECT_TIMEOUT_MS = 10000UL;
static const unsigned long DEFAULT_OTA_WINDOW_MS = 120000UL;
static const unsigned long MAX_OTA_WINDOW_MS = 900000UL;

static const char* NTP_SERVER_1 = "pool.ntp.org";
static const char* NTP_SERVER_2 = "time.google.com";
static const long GMT_OFFSET_SEC = 0;
static const int DAYLIGHT_OFFSET_SEC = 0;

// Default wiring proposal for up to 8 HX711 channels.
// Change in portal if needed.
const int DEFAULT_DOUT_PINS[MAX_CHANNELS] = {16, 18, 21, 23, 26, 27, 32, 33};
const int DEFAULT_SCK_PINS[MAX_CHANNELS]  = {17, 19, 22, 25, 13, 14, 12, 15};

const int ONEWIRE_PIN = 4;
const int BATTERY_ADC_PIN = 34;

const float BATTERY_DIVIDER_MULTIPLIER = 2.0f;
const float ADC_REFERENCE_VOLT = 3.3f;
const int ADC_MAX = 4095;

const int SENSOR_WARMUP_MS = 350;
const float STABLE_STDDEV_THRESHOLD = 50.0f;
const float DRIFT_SLOPE_THRESHOLD = 10.0f;

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

HX711 hx[MAX_CHANNELS];
bool hxBegun[MAX_CHANNELS];

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds18b20(&oneWire);

struct CalibrationPoint {
  float raw;
  float kg;
};

struct ChannelCalibration {
  bool calibrated;
  int pointCount;
  CalibrationPoint points[MAX_CAL_POINTS];
  float slope;
  float intercept;
};

struct ChannelConfig {
  bool enabled;
  int doutPin;
  int sckPin;
  int hiveIndex; // 0..MAX_HIVES-1
  bool tempCompEnabled;
  float tempCompSlope; // kg / °C
  char name[20];
};

struct RuntimeConfig {
  String wifiSSID;
  String wifiPassword;
  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPassword;
  String deviceID;
  uint32_t sleepSeconds;
  uint32_t otaWindowSeconds;
  String otaPassword;
  int activeHiveCount;
  char hiveNames[MAX_HIVES][20];
};

struct ChannelReading {
  bool ready = false;
  bool stable = false;
  bool driftDetected = false;
  bool calibrated = false;
  int samples = 0;
  float rawAvg = NAN;
  float rawMin = NAN;
  float rawMax = NAN;
  float rawStdDev = NAN;
  float rawSlope = NAN;
  float weightKg = NAN;
  float compensatedWeightKg = NAN;
};

RuntimeConfig cfg;
ChannelConfig chCfg[MAX_CHANNELS];
ChannelCalibration chCal[MAX_CHANNELS];

unsigned long configPortalStartedAt = 0;
unsigned long otaStartedAt = 0;
bool otaActive = false;

String deriveTopicFromDeviceID(const String& deviceID) {
  return "devices/" + deviceID + "/telemetry";
}

String htmlHeader(const String& title) {
  String h;
  h += "<!doctype html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += "<title>" + title + "</title>";
  h += "<style>body{font-family:Arial,sans-serif;max-width:1100px;margin:20px auto;padding:0 12px;}";
  h += "input,button,textarea,select{width:100%;padding:10px;margin:6px 0 14px 0;box-sizing:border-box;}";
  h += "button{cursor:pointer;} .row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}";
  h += ".card{border:1px solid #ddd;padding:14px;border-radius:10px;margin-bottom:14px;}";
  h += ".ok{color:#0a7f2e;font-weight:bold;} .err{color:#b00020;font-weight:bold;}";
  h += "table{border-collapse:collapse;width:100%;margin:8px 0 16px 0;}th,td{border:1px solid #ddd;padding:8px;text-align:left;}";
  h += "code{background:#f2f2f2;padding:2px 4px;border-radius:4px;} a{display:inline-block;margin-right:12px;margin-bottom:10px;}</style></head><body>";
  h += "<h2>" + title + "</h2>";
  h += "<p><a href='/'>Home</a><a href='/channels'>Kanaele</a><a href='/calibration'>Kalibrierung</a><a href='/tempcomp'>Temp-Kompensation</a><a href='/ota'>OTA</a><a href='/scan'>WLAN Scan</a><a href='/diag'>Diagnose</a><a href='/reset'>Reset</a></p>";
  return h;
}

void setDefaultConfig() {
  cfg.wifiSSID = "";
  cfg.wifiPassword = "";
  cfg.mqttHost = "";
  cfg.mqttPort = 1883;
  cfg.mqttUser = "";
  cfg.mqttPassword = "";
  cfg.deviceID = "waage-01";
  cfg.sleepSeconds = 300;
  cfg.otaWindowSeconds = DEFAULT_OTA_WINDOW_MS / 1000UL;
  cfg.otaPassword = "";
  cfg.activeHiveCount = 2;
  strlcpy(cfg.hiveNames[0], "Beute 0", sizeof(cfg.hiveNames[0]));
  strlcpy(cfg.hiveNames[1], "Beute 1", sizeof(cfg.hiveNames[1]));
  strlcpy(cfg.hiveNames[2], "Beute 2", sizeof(cfg.hiveNames[2]));
  strlcpy(cfg.hiveNames[3], "Beute 3", sizeof(cfg.hiveNames[3]));

  for (int i = 0; i < MAX_CHANNELS; i++) {
    chCfg[i].enabled = i < 4;
    chCfg[i].doutPin = DEFAULT_DOUT_PINS[i];
    chCfg[i].sckPin = DEFAULT_SCK_PINS[i];
    chCfg[i].hiveIndex = i / 2;
    chCfg[i].tempCompEnabled = false;
    chCfg[i].tempCompSlope = 0.0f;
    snprintf(chCfg[i].name, sizeof(chCfg[i].name), "Kanal %d", i);

    chCal[i].calibrated = false;
    chCal[i].pointCount = 0;
    chCal[i].slope = 1.0f;
    chCal[i].intercept = 0.0f;
    for (int p = 0; p < MAX_CAL_POINTS; p++) {
      chCal[i].points[p].raw = 0.0f;
      chCal[i].points[p].kg = 0.0f;
    }
  }
}

void saveBasicConfig() {
  prefs.begin("iotcfg", false);
  prefs.putString("wifi_ssid", cfg.wifiSSID);
  prefs.putString("wifi_pass", cfg.wifiPassword);
  prefs.putString("mqtt_host", cfg.mqttHost);
  prefs.putUInt("mqtt_port", cfg.mqttPort);
  prefs.putString("mqtt_user", cfg.mqttUser);
  prefs.putString("mqtt_pass", cfg.mqttPassword);
  prefs.putString("device_id", cfg.deviceID);
  prefs.putUInt("sleep_s", cfg.sleepSeconds);
  prefs.putUInt("ota_win_s", cfg.otaWindowSeconds);
  prefs.putString("ota_pass", cfg.otaPassword);
  prefs.putInt("hive_count", cfg.activeHiveCount);
  for (int h = 0; h < MAX_HIVES; h++) {
    prefs.putString(("hive_name_" + String(h)).c_str(), String(cfg.hiveNames[h]));
  }
  prefs.end();
}

void saveChannelConfig(int idx) {
  String base = "ch" + String(idx);
  prefs.begin("iotcfg", false);
  prefs.putBool((base + "_en").c_str(), chCfg[idx].enabled);
  prefs.putInt((base + "_dp").c_str(), chCfg[idx].doutPin);
  prefs.putInt((base + "_sp").c_str(), chCfg[idx].sckPin);
  prefs.putInt((base + "_hi").c_str(), chCfg[idx].hiveIndex);
  prefs.putBool((base + "_tc_en").c_str(), chCfg[idx].tempCompEnabled);
  prefs.putFloat((base + "_tc_m").c_str(), chCfg[idx].tempCompSlope);
  prefs.putString((base + "_name").c_str(), String(chCfg[idx].name));
  prefs.end();
}

void saveCalibrationToPrefs(int idx) {
  String base = "ch" + String(idx);
  prefs.begin("iotcfg", false);
  prefs.putBool((base + "_cal").c_str(), chCal[idx].calibrated);
  prefs.putInt((base + "_pc").c_str(), chCal[idx].pointCount);
  prefs.putFloat((base + "_m").c_str(), chCal[idx].slope);
  prefs.putFloat((base + "_b").c_str(), chCal[idx].intercept);
  for (int p = 0; p < MAX_CAL_POINTS; p++) {
    prefs.putFloat((base + "_pr" + String(p)).c_str(), chCal[idx].points[p].raw);
    prefs.putFloat((base + "_pk" + String(p)).c_str(), chCal[idx].points[p].kg);
  }
  prefs.end();
}

void resetConfig() {
  prefs.begin("iotcfg", false);
  prefs.clear();
  prefs.end();
  setDefaultConfig();
}

bool loadConfig() {
  setDefaultConfig();

  prefs.begin("iotcfg", true);
  cfg.wifiSSID = prefs.getString("wifi_ssid", cfg.wifiSSID);
  cfg.wifiPassword = prefs.getString("wifi_pass", cfg.wifiPassword);
  cfg.mqttHost = prefs.getString("mqtt_host", cfg.mqttHost);
  cfg.mqttPort = (uint16_t)prefs.getUInt("mqtt_port", cfg.mqttPort);
  cfg.mqttUser = prefs.getString("mqtt_user", cfg.mqttUser);
  cfg.mqttPassword = prefs.getString("mqtt_pass", cfg.mqttPassword);
  cfg.deviceID = prefs.getString("device_id", cfg.deviceID);
  cfg.sleepSeconds = prefs.getUInt("sleep_s", cfg.sleepSeconds);
  cfg.otaWindowSeconds = prefs.getUInt("ota_win_s", cfg.otaWindowSeconds);
  cfg.otaPassword = prefs.getString("ota_pass", cfg.otaPassword);
  cfg.activeHiveCount = prefs.getInt("hive_count", cfg.activeHiveCount);
  if (cfg.activeHiveCount < 1) cfg.activeHiveCount = 1;
  if (cfg.activeHiveCount > MAX_HIVES) cfg.activeHiveCount = MAX_HIVES;

  for (int h = 0; h < MAX_HIVES; h++) {
    String n = prefs.getString(("hive_name_" + String(h)).c_str(), String(cfg.hiveNames[h]));
    strlcpy(cfg.hiveNames[h], n.c_str(), sizeof(cfg.hiveNames[h]));
  }

  for (int i = 0; i < MAX_CHANNELS; i++) {
    String base = "ch" + String(i);
    chCfg[i].enabled = prefs.getBool((base + "_en").c_str(), chCfg[i].enabled);
    chCfg[i].doutPin = prefs.getInt((base + "_dp").c_str(), chCfg[i].doutPin);
    chCfg[i].sckPin = prefs.getInt((base + "_sp").c_str(), chCfg[i].sckPin);
    chCfg[i].hiveIndex = prefs.getInt((base + "_hi").c_str(), chCfg[i].hiveIndex);
    if (chCfg[i].hiveIndex < 0) chCfg[i].hiveIndex = 0;
    if (chCfg[i].hiveIndex >= MAX_HIVES) chCfg[i].hiveIndex = MAX_HIVES - 1;
    chCfg[i].tempCompEnabled = prefs.getBool((base + "_tc_en").c_str(), chCfg[i].tempCompEnabled);
    chCfg[i].tempCompSlope = prefs.getFloat((base + "_tc_m").c_str(), chCfg[i].tempCompSlope);
    String cname = prefs.getString((base + "_name").c_str(), String(chCfg[i].name));
    strlcpy(chCfg[i].name, cname.c_str(), sizeof(chCfg[i].name));

    chCal[i].calibrated = prefs.getBool((base + "_cal").c_str(), chCal[i].calibrated);
    chCal[i].pointCount = prefs.getInt((base + "_pc").c_str(), chCal[i].pointCount);
    if (chCal[i].pointCount < 0) chCal[i].pointCount = 0;
    if (chCal[i].pointCount > MAX_CAL_POINTS) chCal[i].pointCount = MAX_CAL_POINTS;
    chCal[i].slope = prefs.getFloat((base + "_m").c_str(), chCal[i].slope);
    chCal[i].intercept = prefs.getFloat((base + "_b").c_str(), chCal[i].intercept);
    for (int p = 0; p < MAX_CAL_POINTS; p++) {
      chCal[i].points[p].raw = prefs.getFloat((base + "_pr" + String(p)).c_str(), 0.0f);
      chCal[i].points[p].kg = prefs.getFloat((base + "_pk" + String(p)).c_str(), 0.0f);
    }
  }
  prefs.end();

  if (cfg.otaWindowSeconds > (MAX_OTA_WINDOW_MS / 1000UL)) {
    cfg.otaWindowSeconds = MAX_OTA_WINDOW_MS / 1000UL;
  }

  return cfg.wifiSSID.length() > 0 && cfg.mqttHost.length() > 0 && cfg.deviceID.length() > 0;
}

bool connectWiFiWithCredentials(const String& ssid, const String& password, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectWiFi() {
  return connectWiFiWithCredentials(cfg.wifiSSID, cfg.wifiPassword, WIFI_CONNECT_TIMEOUT_MS);
}

bool mqttConnectWithCredentials(PubSubClient& client, const String& host, uint16_t port, const String& user, const String& password, const String& clientID, unsigned long timeoutMs) {
  client.setServer(host.c_str(), port);
  unsigned long start = millis();
  while (!client.connected() && millis() - start < timeoutMs) {
    bool ok = false;
    if (user.length()) ok = client.connect(clientID.c_str(), user.c_str(), password.c_str());
    else ok = client.connect(clientID.c_str());
    if (!ok) delay(500);
  }
  return client.connected();
}

bool connectMQTT() {
  return mqttConnectWithCredentials(mqttClient, cfg.mqttHost, cfg.mqttPort, cfg.mqttUser, cfg.mqttPassword, cfg.deviceID + "-pub", MQTT_CONNECT_TIMEOUT_MS);
}

String testMQTTConnection(const String& wifiSSID, const String& wifiPass, const String& mqttHost, uint16_t mqttPort, const String& mqttUser, const String& mqttPass, const String& deviceID) {
  WiFi.disconnect(true, true);
  delay(200);

  if (!connectWiFiWithCredentials(wifiSSID, wifiPass, WIFI_CONNECT_TIMEOUT_MS)) return "WLAN-Verbindung fehlgeschlagen";

  WiFiClient testWiFiClient;
  PubSubClient testClient(testWiFiClient);
  bool mqttOK = mqttConnectWithCredentials(testClient, mqttHost, mqttPort, mqttUser, mqttPass, deviceID + "-test", MQTT_CONNECT_TIMEOUT_MS);
  if (!mqttOK) {
    WiFi.disconnect(true, true);
    return "MQTT-Verbindung fehlgeschlagen";
  }

  String topic = deriveTopicFromDeviceID(deviceID) + "/config-test";
  String payload = "{\"device_id\":\"" + deviceID + "\",\"type\":\"config-test\"}";
  bool published = testClient.publish(topic.c_str(), payload.c_str(), false);
  testClient.loop();
  testClient.disconnect();
  WiFi.disconnect(true, true);
  if (!published) return "MQTT verbunden, Test-Publish fehlgeschlagen";
  return "";
}

void setupScales() {
  for (int i = 0; i < MAX_CHANNELS; i++) {
    hxBegun[i] = false;
    if (chCfg[i].enabled) {
      hx[i].begin(chCfg[i].doutPin, chCfg[i].sckPin);
      hx[i].set_gain(128);
      hxBegun[i] = true;
    }
  }
}

float readHXRawAvg(int idx, int samples) {
  if (idx < 0 || idx >= MAX_CHANNELS || !chCfg[idx].enabled || !hxBegun[idx]) return NAN;
  long total = 0;
  int valid = 0;
  for (int i = 0; i < samples; i++) {
    if (hx[idx].is_ready()) {
      total += hx[idx].read();
      valid++;
    }
    delay(20);
  }
  if (valid == 0) return NAN;
  return (float)total / (float)valid;
}

bool recomputeCalibrationModel(int idx, String& errorMsg) {
  if (idx < 0 || idx >= MAX_CHANNELS) {
    errorMsg = "ungueltiger Kanal";
    return false;
  }
  if (chCal[idx].pointCount < 2) {
    errorMsg = "mindestens 2 Punkte noetig";
    chCal[idx].calibrated = false;
    return false;
  }

  float sumX = 0.0f, sumY = 0.0f, sumXY = 0.0f, sumXX = 0.0f;
  int n = chCal[idx].pointCount;
  for (int i = 0; i < n; i++) {
    float x = chCal[idx].points[i].raw;
    float y = chCal[idx].points[i].kg;
    sumX += x; sumY += y; sumXY += x * y; sumXX += x * x;
  }

  float denom = (n * sumXX) - (sumX * sumX);
  if (fabsf(denom) < 0.000001f) {
    errorMsg = "Regression nicht moeglich";
    chCal[idx].calibrated = false;
    return false;
  }

  chCal[idx].slope = ((n * sumXY) - (sumX * sumY)) / denom;
  chCal[idx].intercept = (sumY - chCal[idx].slope * sumX) / n;
  chCal[idx].calibrated = true;
  saveCalibrationToPrefs(idx);
  return true;
}

void removeCalibrationPoint(int idx, int pointIdx) {
  if (idx < 0 || idx >= MAX_CHANNELS || pointIdx < 0 || pointIdx >= chCal[idx].pointCount) return;
  for (int i = pointIdx; i < chCal[idx].pointCount - 1; i++) {
    chCal[idx].points[i] = chCal[idx].points[i + 1];
  }
  chCal[idx].pointCount--;
  if (chCal[idx].pointCount < 2) chCal[idx].calibrated = false;
  String err;
  if (chCal[idx].pointCount >= 2) recomputeCalibrationModel(idx, err);
  saveCalibrationToPrefs(idx);
}

void clearCalibration(int idx) {
  if (idx < 0 || idx >= MAX_CHANNELS) return;
  chCal[idx].pointCount = 0;
  chCal[idx].calibrated = false;
  chCal[idx].slope = 1.0f;
  chCal[idx].intercept = 0.0f;
  for (int i = 0; i < MAX_CAL_POINTS; i++) {
    chCal[idx].points[i].raw = 0.0f;
    chCal[idx].points[i].kg = 0.0f;
  }
  saveCalibrationToPrefs(idx);
}

float readBatteryVoltage() {
  analogReadResolution(12);
  uint16_t raw = analogRead(BATTERY_ADC_PIN);
  float v = ((float)raw / (float)ADC_MAX) * ADC_REFERENCE_VOLT;
  return v * BATTERY_DIVIDER_MULTIPLIER;
}

float readTemperatureC() {
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) return NAN;
  return t;
}

float computeStdDev(float* vals, int n, float mean) {
  if (n <= 1) return 0.0f;
  float acc = 0.0f;
  for (int i = 0; i < n; i++) {
    float d = vals[i] - mean;
    acc += d * d;
  }
  return sqrtf(acc / (float)n);
}

ChannelReading readChannel(int idx, float temperatureC) {
  ChannelReading r;
  if (idx < 0 || idx >= MAX_CHANNELS || !chCfg[idx].enabled || !hxBegun[idx]) return r;

  const int N = 10;
  float vals[N];
  int n = 0;

  delay(SENSOR_WARMUP_MS);
  for (int i = 0; i < N; i++) {
    if (hx[idx].is_ready()) vals[n++] = (float)hx[idx].read();
    delay(20);
  }

  r.samples = n;
  r.calibrated = chCal[idx].calibrated;
  if (n < 3) return r;

  r.ready = true;
  r.rawMin = vals[0];
  r.rawMax = vals[0];
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    sum += vals[i];
    if (vals[i] < r.rawMin) r.rawMin = vals[i];
    if (vals[i] > r.rawMax) r.rawMax = vals[i];
  }
  r.rawAvg = sum / (float)n;
  r.rawStdDev = computeStdDev(vals, n, r.rawAvg);
  r.rawSlope = (vals[n - 1] - vals[0]) / (float)(n - 1);
  r.stable = r.rawStdDev < STABLE_STDDEV_THRESHOLD;
  r.driftDetected = fabsf(r.rawSlope) > DRIFT_SLOPE_THRESHOLD;

  if (chCal[idx].calibrated) {
    r.weightKg = chCal[idx].slope * r.rawAvg + chCal[idx].intercept;
    r.compensatedWeightKg = r.weightKg;
    if (!isnan(temperatureC) && chCfg[idx].tempCompEnabled) {
      const float refTempC = 20.0f;
      float delta = (temperatureC - refTempC) * chCfg[idx].tempCompSlope;
      r.compensatedWeightKg = r.weightKg - delta;
    }
  }
  return r;
}

String formatTimestampISO8601() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) return "";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

String buildTelemetryPayload(float temperatureC, float batteryV, int rssi, ChannelReading* readings) {
  DynamicJsonDocument doc(8192);
  doc["schema"] = "telemetry.v4";
  doc["device_id"] = cfg.deviceID;
  doc["timestamp"] = formatTimestampISO8601();
  if (!isnan(temperatureC)) doc["temperature_c"] = temperatureC;
  doc["battery_v"] = batteryV;
  doc["rssi"] = rssi;
  doc["sleep_seconds"] = cfg.sleepSeconds;
  doc["firmware_version"] = "esp32-telemetry-v7-dynamic";
  doc["active_hives"] = cfg.activeHiveCount;

  JsonArray channels = doc.createNestedArray("channels");
  float hiveWeight[MAX_HIVES] = {0,0,0,0};
  float hiveCompWeight[MAX_HIVES] = {0,0,0,0};
  bool hiveHasWeight[MAX_HIVES] = {false,false,false,false};
  bool hiveHasComp[MAX_HIVES] = {false,false,false,false};
  int hiveChannelCount[MAX_HIVES] = {0,0,0,0};

  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!chCfg[i].enabled) continue;
    JsonObject c = channels.createNestedObject();
    c["channel_index"] = i;
    c["channel_name"] = chCfg[i].name;
    c["hive_index"] = chCfg[i].hiveIndex;
    c["hive_name"] = cfg.hiveNames[chCfg[i].hiveIndex];
    c["dout_pin"] = chCfg[i].doutPin;
    c["sck_pin"] = chCfg[i].sckPin;
    if (!isnan(readings[i].weightKg)) c["weight_kg"] = readings[i].weightKg;
    if (!isnan(readings[i].compensatedWeightKg)) c["compensated_weight_kg"] = readings[i].compensatedWeightKg;
    if (!isnan(readings[i].rawAvg)) c["raw_avg"] = readings[i].rawAvg;
    if (!isnan(readings[i].rawMin)) c["raw_min"] = readings[i].rawMin;
    if (!isnan(readings[i].rawMax)) c["raw_max"] = readings[i].rawMax;
    if (!isnan(readings[i].rawStdDev)) c["raw_stddev"] = readings[i].rawStdDev;
    if (!isnan(readings[i].rawSlope)) c["raw_slope"] = readings[i].rawSlope;
    c["samples"] = readings[i].samples;
    c["stable"] = readings[i].stable;
    c["drift_detected"] = readings[i].driftDetected;
    c["calibrated"] = readings[i].calibrated;
    c["ready"] = readings[i].ready;

    int h = chCfg[i].hiveIndex;
    hiveChannelCount[h]++;
    if (!isnan(readings[i].weightKg)) {
      hiveWeight[h] += readings[i].weightKg;
      hiveHasWeight[h] = true;
    }
    if (!isnan(readings[i].compensatedWeightKg)) {
      hiveCompWeight[h] += readings[i].compensatedWeightKg;
      hiveHasComp[h] = true;
    }
  }

  JsonArray hives = doc.createNestedArray("hives");
  for (int h = 0; h < cfg.activeHiveCount; h++) {
    JsonObject hive = hives.createNestedObject();
    hive["hive_index"] = h;
    hive["hive_name"] = cfg.hiveNames[h];
    hive["channel_count"] = hiveChannelCount[h];
    if (hiveHasWeight[h]) hive["weight_kg"] = hiveWeight[h];
    if (hiveHasComp[h]) hive["compensated_weight_kg"] = hiveCompWeight[h];
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void setupTimeIfPossible() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 500)) return;
    delay(200);
  }
}

void configureOTA() {
  if (cfg.otaWindowSeconds == 0) {
    otaActive = false;
    return;
  }
  ArduinoOTA.setHostname(cfg.deviceID.c_str());
  if (cfg.otaPassword.length()) ArduinoOTA.setPassword(cfg.otaPassword.c_str());
  ArduinoOTA.onStart([]() { otaActive = true; });
  ArduinoOTA.onEnd([]() { otaActive = false; });
  ArduinoOTA.onError([](ota_error_t error) { (void)error; });
  ArduinoOTA.begin();
  otaStartedAt = millis();
  otaActive = true;
}

void enterDeepSleep() {
  esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepSeconds * 1000000ULL);
  delay(100);
  esp_deep_sleep_start();
}

// -------- Web handlers --------

void handleRoot() {
  String html = htmlHeader("ESP32 Setup");
  html += "<div class='card'><form method='POST' action='/save'>";
  html += "<label>WLAN SSID</label><input name='wifi_ssid' value='" + cfg.wifiSSID + "'>";
  html += "<label>WLAN Passwort</label><input type='password' name='wifi_pass' value='" + cfg.wifiPassword + "'>";
  html += "<label>MQTT Host</label><input name='mqtt_host' value='" + cfg.mqttHost + "'>";
  html += "<label>MQTT Port</label><input name='mqtt_port' value='" + String(cfg.mqttPort) + "'>";
  html += "<label>MQTT User</label><input name='mqtt_user' value='" + cfg.mqttUser + "'>";
  html += "<label>MQTT Passwort</label><input type='password' name='mqtt_pass' value='" + cfg.mqttPassword + "'>";
  html += "<label>Device ID</label><input name='device_id' value='" + cfg.deviceID + "'>";
  html += "<label>Sleep Sekunden</label><input name='sleep_s' value='" + String(cfg.sleepSeconds) + "'>";
  html += "<label>OTA Fenster in Sekunden (0 = deaktiviert)</label><input name='ota_win_s' value='" + String(cfg.otaWindowSeconds) + "'>";
  html += "<label>Anzahl Beuten</label><input name='hive_count' value='" + String(cfg.activeHiveCount) + "'>";
  for (int h = 0; h < MAX_HIVES; h++) {
    html += "<label>Beute " + String(h) + " Name</label><input name='hive_name_" + String(h) + "' value='" + String(cfg.hiveNames[h]) + "'>";
  }
  html += "<label>OTA Passwort (optional)</label><input type='password' name='ota_pass' value='" + cfg.otaPassword + "'>";
  html += "<p>MQTT Topic: <code>" + deriveTopicFromDeviceID(cfg.deviceID.length() ? cfg.deviceID : String("waage-01")) + "</code></p>";
  html += "<button type='submit'>MQTT/WLAN testen und speichern</button></form></div>";

  html += "<div class='card'><p>Fuer zwei Waagen pro Beute ordnest du einfach zwei Kanaele derselben Beute zu. Die Firmware summiert dann die kanalweise kalibrierten Gewichte pro Beute.</p></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String wifiSSID = server.arg("wifi_ssid");
  String wifiPass = server.arg("wifi_pass");
  String mqttHost = server.arg("mqtt_host");
  uint16_t mqttPort = (uint16_t)server.arg("mqtt_port").toInt();
  String mqttUser = server.arg("mqtt_user");
  String mqttPass = server.arg("mqtt_pass");
  String deviceID = server.arg("device_id");
  uint32_t sleepS = (uint32_t)server.arg("sleep_s").toInt();
  uint32_t otaWinS = (uint32_t)server.arg("ota_win_s").toInt();
  int hiveCount = server.arg("hive_count").toInt();
  String otaPass = server.arg("ota_pass");

  if (wifiSSID.isEmpty() || mqttHost.isEmpty() || mqttPort == 0 || deviceID.isEmpty() || sleepS == 0) {
    server.send(400, "text/plain", "Ungueltige Eingaben");
    return;
  }
  if (otaWinS > (MAX_OTA_WINDOW_MS / 1000UL)) {
    server.send(400, "text/plain", "OTA-Fenster zu gross");
    return;
  }
  if (hiveCount < 1 || hiveCount > MAX_HIVES) {
    server.send(400, "text/plain", "ungueltige Beuten-Anzahl");
    return;
  }

  String testError = testMQTTConnection(wifiSSID, wifiPass, mqttHost, mqttPort, mqttUser, mqttPass, deviceID);
  if (testError.length()) {
    String html = htmlHeader("Test fehlgeschlagen");
    html += "<div class='card'><p class='err'>" + testError + "</p><p>Die Konfiguration wurde nicht gespeichert.</p></div></body></html>";
    server.send(400, "text/html", html);
    return;
  }

  cfg.wifiSSID = wifiSSID;
  cfg.wifiPassword = wifiPass;
  cfg.mqttHost = mqttHost;
  cfg.mqttPort = mqttPort;
  cfg.mqttUser = mqttUser;
  cfg.mqttPassword = mqttPass;
  cfg.deviceID = deviceID;
  cfg.sleepSeconds = sleepS;
  cfg.otaWindowSeconds = otaWinS;
  cfg.activeHiveCount = hiveCount;
  cfg.otaPassword = otaPass;
  for (int h = 0; h < MAX_HIVES; h++) {
    String name = server.arg("hive_name_" + String(h));
    if (name.length() == 0) name = "Beute " + String(h);
    strlcpy(cfg.hiveNames[h], name.c_str(), sizeof(cfg.hiveNames[h]));
  }
  saveBasicConfig();

  String html = htmlHeader("Gespeichert");
  html += "<div class='card'><p class='ok'>WLAN- und MQTT-Test erfolgreich.</p><p>Konfiguration gespeichert. Neustart...</p></div></body></html>";
  server.send(200, "text/html", html);
  delay(1500);
  ESP.restart();
}

void handleChannelsPage() {
  String html = htmlHeader("Kanaele");
  html += "<div class='card'><p>Bis zu 8 HX711-Kanaele. Aktiviere nur die Kanaele, die wirklich angeschlossen sind. Weise danach jeden Kanal einer Beute zu.</p></div>";
  for (int i = 0; i < MAX_CHANNELS; i++) {
    html += "<div class='card'><h3>Kanal " + String(i) + "</h3>";
    html += "<form method='POST' action='/channel_save'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<label>Name</label><input name='name' value='" + String(chCfg[i].name) + "'>";
    html += "<label>Aktiv (1 oder 0)</label><input name='enabled' value='" + String(chCfg[i].enabled ? 1 : 0) + "'>";
    html += "<label>DOUT Pin</label><input name='dout' value='" + String(chCfg[i].doutPin) + "'>";
    html += "<label>SCK Pin</label><input name='sck' value='" + String(chCfg[i].sckPin) + "'>";
    html += "<label>Beute Index</label><input name='hive' value='" + String(chCfg[i].hiveIndex) + "'>";
    html += "<button type='submit'>Kanal speichern</button></form></div>";
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleChannelSave() {
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_CHANNELS) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }
  chCfg[idx].enabled = server.arg("enabled").toInt() == 1;
  chCfg[idx].doutPin = server.arg("dout").toInt();
  chCfg[idx].sckPin = server.arg("sck").toInt();
  chCfg[idx].hiveIndex = server.arg("hive").toInt();
  if (chCfg[idx].hiveIndex < 0) chCfg[idx].hiveIndex = 0;
  if (chCfg[idx].hiveIndex >= MAX_HIVES) chCfg[idx].hiveIndex = MAX_HIVES - 1;
  String name = server.arg("name");
  if (name.length() == 0) name = "Kanal " + String(idx);
  strlcpy(chCfg[idx].name, name.c_str(), sizeof(chCfg[idx].name));
  saveChannelConfig(idx);
  server.sendHeader("Location", "/channels", true);
  server.send(302, "text/plain", "");
}

void handleCalibrationPage() {
  String html = htmlHeader("Mehrpunkt-Kalibrierung");
  html += "<div class='card'><p>Jeder Kanal wird einzeln kalibriert. Danach summiert die Firmware die kalibrierten Kanalgewichte pro Beute.</p></div>";
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!chCfg[i].enabled) continue;
    html += "<div class='card'><h3>" + String(chCfg[i].name) + " (Kanal " + String(i) + ", Beute " + String(chCfg[i].hiveIndex) + ")</h3>";
    html += "<p>calibrated=" + String(chCal[i].calibrated ? "true" : "false") + ", points=" + String(chCal[i].pointCount) + ", slope=" + String(chCal[i].slope, 10) + ", intercept=" + String(chCal[i].intercept, 6) + "</p>";
    html += "<table><tr><th>#</th><th>Raw</th><th>kg</th><th>Aktion</th></tr>";
    for (int p = 0; p < chCal[i].pointCount; p++) {
      html += "<tr><td>" + String(p) + "</td><td>" + String(chCal[i].points[p].raw, 3) + "</td><td>" + String(chCal[i].points[p].kg, 6) + "</td>";
      html += "<td><a href='/calibration_delete?idx=" + String(i) + "&point=" + String(p) + "'>Loeschen</a></td></tr>";
    }
    html += "</table>";
    html += "<form method='POST' action='/calibration_add'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<label>Raw (leer lassen fuer Live-Lesung)</label><input name='raw'>";
    html += "<label>Referenzgewicht kg</label><input name='kg'>";
    html += "<button type='submit'>Kalibrierpunkt hinzufuegen</button></form>";
    html += "<p><a href='/calibration_capture?idx=" + String(i) + "'>Aktuellen Raw-Wert anzeigen</a> ";
    html += "<a href='/calibration_clear?idx=" + String(i) + "'>Alle Punkte loeschen</a></p></div>";
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleCalibrationCapture() {
  int idx = server.arg("idx").toInt();
  float raw = readHXRawAvg(idx, 15);
  String html = htmlHeader("Live-Raw");
  html += "<div class='card'><p>Kanal " + String(idx) + " Raw Avg: <code>" + String(raw, 3) + "</code></p></div></body></html>";
  server.send(200, "text/html", html);
}

void handleCalibrationAdd() {
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_CHANNELS || !chCfg[idx].enabled) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }
  if (chCal[idx].pointCount >= MAX_CAL_POINTS) {
    server.send(400, "text/plain", "maximale Punktzahl erreicht");
    return;
  }
  float raw = server.arg("raw").length() ? server.arg("raw").toFloat() : readHXRawAvg(idx, 15);
  float kg = server.arg("kg").toFloat();
  if (isnan(raw)) {
    server.send(400, "text/plain", "ungueltiger Raw-Wert");
    return;
  }

  chCal[idx].points[chCal[idx].pointCount].raw = raw;
  chCal[idx].points[chCal[idx].pointCount].kg = kg;
  chCal[idx].pointCount++;

  String err;
  bool ok = recomputeCalibrationModel(idx, err);

  String html = htmlHeader("Kalibrierpunkt gespeichert");
  html += "<div class='card'><p>Kanal " + String(idx) + ": Punkt raw=" + String(raw, 3) + ", kg=" + String(kg, 6) + " gespeichert.</p>";
  if (ok) html += "<p>Regression aktualisiert.<br>slope=" + String(chCal[idx].slope, 10) + "<br>intercept=" + String(chCal[idx].intercept, 6) + "</p>";
  else {
    html += "<p>Modell noch nicht aktiv: " + err + "</p>";
    saveCalibrationToPrefs(idx);
  }
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleCalibrationDelete() {
  int idx = server.arg("idx").toInt();
  int pointIdx = server.arg("point").toInt();
  removeCalibrationPoint(idx, pointIdx);
  server.sendHeader("Location", "/calibration", true);
  server.send(302, "text/plain", "");
}

void handleCalibrationClear() {
  int idx = server.arg("idx").toInt();
  clearCalibration(idx);
  server.sendHeader("Location", "/calibration", true);
  server.send(302, "text/plain", "");
}

void handleTempCompPage() {
  String html = htmlHeader("Temperaturkompensation");
  html += "<div class='card'><p>Temperaturkompensation wird pro Kanal gepflegt. Die Summen pro Beute ergeben sich danach automatisch.</p></div>";
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!chCfg[i].enabled) continue;
    html += "<div class='card'><h3>" + String(chCfg[i].name) + " (Kanal " + String(i) + ")</h3>";
    html += "<form method='POST' action='/tempcomp_save'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<label>Aktivieren (1 oder 0)</label><input name='enabled' value='" + String(chCfg[i].tempCompEnabled ? 1 : 0) + "'>";
    html += "<label>Slope kg/°C</label><input name='slope' value='" + String(chCfg[i].tempCompSlope, 8) + "'>";
    html += "<button type='submit'>Speichern</button></form></div>";
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleTempCompSave() {
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_CHANNELS) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }
  chCfg[idx].tempCompEnabled = server.arg("enabled").toInt() == 1;
  chCfg[idx].tempCompSlope = server.arg("slope").toFloat();
  saveChannelConfig(idx);
  server.sendHeader("Location", "/tempcomp", true);
  server.send(302, "text/plain", "");
}

void handleOTAInfo() {
  String html = htmlHeader("OTA");
  html += "<div class='card'><p>OTA Hostname: <code>" + cfg.deviceID + "</code></p>";
  html += "<p>OTA aktiv: <code>" + String(otaActive ? "true" : "false") + "</code></p>";
  html += "<p>OTA Fenster: <code>" + String(cfg.otaWindowSeconds) + " Sekunden</code>.</p></div></body></html>";
  server.send(200, "text/html", html);
}

void handleScan() {
  String html = htmlHeader("WLAN Scan");
  int n = WiFi.scanNetworks();
  html += "<div class='card'>";
  if (n <= 0) html += "<p>Keine Netzwerke gefunden.</p>";
  else {
    html += "<ul>";
    for (int i = 0; i < n; i++) {
      html += "<li>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)";
      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) html += " offen";
      html += "</li>";
    }
    html += "</ul>";
  }
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleDiag() {
  String html = htmlHeader("Diagnose");
  html += "<div class='card'><table><tr><th>Kanal</th><th>Name</th><th>Aktiv</th><th>DOUT</th><th>SCK</th><th>Beute</th><th>Kalibriert</th><th>Punkte</th></tr>";
  for (int i = 0; i < MAX_CHANNELS; i++) {
    html += "<tr><td>" + String(i) + "</td><td>" + String(chCfg[i].name) + "</td><td>" + String(chCfg[i].enabled ? "ja" : "nein") + "</td><td>" + String(chCfg[i].doutPin) + "</td><td>" + String(chCfg[i].sckPin) + "</td><td>" + String(chCfg[i].hiveIndex) + "</td><td>" + String(chCal[i].calibrated ? "ja" : "nein") + "</td><td>" + String(chCal[i].pointCount) + "</td></tr>";
  }
  html += "</table></div></body></html>";
  server.send(200, "text/html", html);
}

void handleReset() {
  resetConfig();
  server.send(200, "text/html", htmlHeader("Reset") + "<div class='card'><p>Konfiguration geloescht. Neustart...</p></div></body></html>");
  delay(1500);
  ESP.restart();
}

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  configPortalStartedAt = millis();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/channels", HTTP_GET, handleChannelsPage);
  server.on("/channel_save", HTTP_POST, handleChannelSave);
  server.on("/calibration", HTTP_GET, handleCalibrationPage);
  server.on("/calibration_capture", HTTP_GET, handleCalibrationCapture);
  server.on("/calibration_add", HTTP_POST, handleCalibrationAdd);
  server.on("/calibration_delete", HTTP_GET, handleCalibrationDelete);
  server.on("/calibration_clear", HTTP_GET, handleCalibrationClear);
  server.on("/tempcomp", HTTP_GET, handleTempCompPage);
  server.on("/tempcomp_save", HTTP_POST, handleTempCompSave);
  server.on("/ota", HTTP_GET, handleOTAInfo);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/diag", HTTP_GET, handleDiag);
  server.on("/reset", HTTP_GET, handleReset);
  server.onNotFound(handleRoot);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  loadConfig();
  ds18b20.begin();
  setupScales();

  bool hasStored = cfg.wifiSSID.length() > 0 && cfg.mqttHost.length() > 0 && cfg.deviceID.length() > 0;
  if (!hasStored) {
    Serial.println("No config found. Starting config portal.");
    startConfigPortal();
    return;
  }

  if (!connectWiFi()) {
    Serial.println("WiFi connect failed. Starting config portal.");
    startConfigPortal();
    return;
  }

  setupTimeIfPossible();

  if (!connectMQTT()) {
    Serial.println("MQTT connect failed. Starting config portal.");
    startConfigPortal();
    return;
  }

  configureOTA();

  float tempC = readTemperatureC();
  float batteryV = readBatteryVoltage();
  int rssi = WiFi.RSSI();

  ChannelReading readings[MAX_CHANNELS];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    readings[i] = readChannel(i, tempC);
  }

  String topic = deriveTopicFromDeviceID(cfg.deviceID);
  String payload = buildTelemetryPayload(tempC, batteryV, rssi, readings);

  Serial.println("Publishing topic:");
  Serial.println(topic);
  Serial.println("Publishing payload:");
  Serial.println(payload);

  bool published = mqttClient.publish(topic.c_str(), payload.c_str(), true);
  mqttClient.loop();
  delay(300);

  if (!published) {
    Serial.println("Publish failed. Starting config portal.");
    startConfigPortal();
    return;
  }

  Serial.println("Publish OK.");
  if (!otaActive) {
    enterDeepSleep();
  }
}

void loop() {
  if (WiFi.getMode() == WIFI_AP) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (millis() - configPortalStartedAt > CONFIG_TIMEOUT_MS) ESP.restart();
    delay(5);
    return;
  }

  if (otaActive) {
    ArduinoOTA.handle();
    mqttClient.loop();
    if (millis() - otaStartedAt > (cfg.otaWindowSeconds * 1000UL)) {
      otaActive = false;
      enterDeepSleep();
    }
    delay(10);
    return;
  }

  mqttClient.loop();
  delay(100);
}
