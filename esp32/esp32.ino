#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HX711.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <math.h>

static const int MAX_CHANNELS = 8;
static const int MAX_HIVES = 3;
static const int MAX_CAL_POINTS = 5;

static const byte DNS_PORT = 53;
static const char* AP_SSID = "BeeLife";
static const char* AP_PASS = "setup";
static const unsigned long CONFIG_TIMEOUT_MS = 10UL * 60UL * 1000UL;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;
static const unsigned long MQTT_CONNECT_TIMEOUT_MS = 10000UL;
static const unsigned long DEFAULT_OTA_WINDOW_MS = 120000UL;
static const unsigned long MAX_OTA_WINDOW_MS = 900000UL;
static const unsigned long MIN_SLEEP_SECONDS = 10UL;

static const uint16_t MQTT_KEEPALIVE_SECONDS = 60;
static const uint16_t MQTT_SOCKET_TIMEOUT_SECONDS = 10;
static const uint16_t MQTT_BUFFER_SIZE = 4096;

static const char* NTP_SERVER_1 = "pool.ntp.org";
static const char* NTP_SERVER_2 = "time.google.com";
static const long GMT_OFFSET_SEC = 0;
static const int DAYLIGHT_OFFSET_SEC = 0;

const int DEFAULT_DOUT_PINS[MAX_CHANNELS] = {16, 18, 21, 23, 26, 27, 32, 33};
const int DEFAULT_SCK_PINS[MAX_CHANNELS]  = {17, 19, 22, 25, 13, 14, 12, 15};

const int ONEWIRE_PIN = 4;
const int BATTERY_ADC_PIN = 34;

const float BATTERY_DIVIDER_MULTIPLIER = 2.0f;
const float ADC_REFERENCE_VOLT = 3.3f;
const int ADC_MAX = 4095;

const int SENSOR_WARMUP_MS = 1000;
const float STABLE_STDDEV_THRESHOLD = 100.0f;
const float DRIFT_SLOPE_THRESHOLD = 10.0f;

#define DBG(fmt, ...) do { Serial.printf("[DBG] " fmt "\n", ##__VA_ARGS__); } while(0)

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
  float raw = 0.0f;
  float kg = 0.0f;
};

struct ChannelCalibration {
  int count = 0;
  CalibrationPoint points[MAX_CAL_POINTS];
  bool validModel = false;
  double scale = 1.0;
  double offset = 0.0;
  double tempCompKgPerC = 0.0;
};

struct ChannelConfig {
  bool enabled = false;
  int doutPin = -1;
  int sckPin = -1;
  int hiveIndex = 0;
  String channelName;
};

struct RuntimeConfig {
  String wifiSSID;
  String wifiPassword;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPassword;
  String deviceID;
  unsigned long sleepSeconds = 300;
  unsigned long otaWindowMs = DEFAULT_OTA_WINDOW_MS;
  int activeChannels = 2;
  int activeHives = 1;
  int gtsStartYear = 0;
  float gtsStartValue = 0.0f;
  bool gtsStartConfigured = false;
};

struct ChannelReading {
  bool ready = false;
  bool stable = false;
  bool driftDetected = false;
  bool calibrated = false;
  int samples = 0;
  float rawAvg = 0.0f;
  float rawMin = 0.0f;
  float rawMax = 0.0f;
  float rawStdDev = 0.0f;
  float rawSlope = 0.0f;
  double weightKg = 0.0;
  double compensatedWeightKg = 0.0;
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
  String html;
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" + title + "</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;max-width:900px}";
  html += "input,select,button{width:100%;padding:10px;margin:6px 0}";
  html += "label{font-weight:bold;display:block;margin-top:10px}";
  html += "a{display:inline-block;margin:8px 8px 8px 0}";
  html += ".box{border:1px solid #ccc;padding:12px;border-radius:8px;margin-bottom:16px}";
  html += "</style></head><body>";
  html += "<h2>" + title + "</h2>";
  return html;
}

void setDefaultConfig() {
  cfg = RuntimeConfig();
  cfg.mqttPort = 1883;
  cfg.sleepSeconds = 300;
  cfg.otaWindowMs = DEFAULT_OTA_WINDOW_MS;
  cfg.activeChannels = 2;
  cfg.activeHives = 1;
  cfg.gtsStartYear = 0;
  cfg.gtsStartValue = 0.0f;
  cfg.gtsStartConfigured = false;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    chCfg[i].enabled = i < cfg.activeChannels;
    chCfg[i].doutPin = DEFAULT_DOUT_PINS[i];
    chCfg[i].sckPin = DEFAULT_SCK_PINS[i];
    chCfg[i].hiveIndex = i / 2;
    chCfg[i].channelName = "Kanal " + String(i);
    chCal[i] = ChannelCalibration();
    hxBegun[i] = false;
  }
}

void saveBasicConfig() {
  prefs.begin("beeweight", false);
  prefs.putString("wifi_ssid", cfg.wifiSSID);
  prefs.putString("wifi_pass", cfg.wifiPassword);
  prefs.putString("mqtt_host", cfg.mqttHost);
  prefs.putUShort("mqtt_port", cfg.mqttPort);
  prefs.putString("mqtt_user", cfg.mqttUser);
  prefs.putString("mqtt_pass", cfg.mqttPassword);
  prefs.putString("device_id", cfg.deviceID);
  prefs.putULong("sleep_s", cfg.sleepSeconds);
  prefs.putULong("ota_ms", cfg.otaWindowMs);
  prefs.putInt("act_ch", cfg.activeChannels);
  prefs.putInt("act_hives", cfg.activeHives);
  prefs.putInt("gts_year", cfg.gtsStartYear);
  prefs.putFloat("gts_value", cfg.gtsStartValue);
  prefs.putBool("gts_cfg", cfg.gtsStartConfigured);
  prefs.end();
}

void saveChannelConfig(int idx) {
  prefs.begin("beeweight", false);
  String p = "ch" + String(idx) + "_";
  prefs.putBool((p + "en").c_str(), chCfg[idx].enabled);
  prefs.putInt((p + "dout").c_str(), chCfg[idx].doutPin);
  prefs.putInt((p + "sck").c_str(), chCfg[idx].sckPin);
  prefs.putInt((p + "hive").c_str(), chCfg[idx].hiveIndex);
  prefs.putString((p + "name").c_str(), chCfg[idx].channelName);
  prefs.end();
}

void saveCalibrationToPrefs(int idx) {
  prefs.begin("beeweight", false);
  String p = "cal" + String(idx) + "_";
  prefs.putInt((p + "cnt").c_str(), chCal[idx].count);
  prefs.putBool((p + "valid").c_str(), chCal[idx].validModel);
  prefs.putDouble((p + "scale").c_str(), chCal[idx].scale);
  prefs.putDouble((p + "offset").c_str(), chCal[idx].offset);
  prefs.putDouble((p + "tcomp").c_str(), chCal[idx].tempCompKgPerC);
  for (int i = 0; i < MAX_CAL_POINTS; i++) {
    prefs.putFloat((p + "raw" + String(i)).c_str(), chCal[idx].points[i].raw);
    prefs.putFloat((p + "kg" + String(i)).c_str(), chCal[idx].points[i].kg);
  }
  prefs.end();
}

bool recomputeCalibrationModelInMemory(int idx, String& errorMsg) {
  if (chCal[idx].count < 2) {
    chCal[idx].validModel = false;
    errorMsg = "Mindestens 2 Kalibrierpunkte noetig";
    return false;
  }

  double sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
  for (int i = 0; i < chCal[idx].count; i++) {
    double x = chCal[idx].points[i].raw;
    double y = chCal[idx].points[i].kg;
    sumX += x;
    sumY += y;
    sumXY += x * y;
    sumXX += x * x;
  }

  double n = (double)chCal[idx].count;
  double denom = n * sumXX - sumX * sumX;
  if (fabs(denom) < 0.0001) {
    chCal[idx].validModel = false;
    errorMsg = "Kalibriermodell nicht berechenbar";
    return false;
  }

  double scale_d = (n * sumXY - sumX * sumY) / denom;
  double offset_d = (sumY - scale_d * sumX) / n;
  chCal[idx].scale = scale_d;
  chCal[idx].offset = offset_d;
  chCal[idx].validModel = true;
  errorMsg = "";
  return true;
}

void resetConfig() {
  prefs.begin("beeweight", false);
  prefs.clear();
  prefs.end();
  setDefaultConfig();
}

float parseLocalizedFloat(String s) {
  s.trim();
  s.replace(",", ".");
  return s.toFloat();
}

bool loadConfig() {
  prefs.begin("beeweight", false);

  cfg.wifiSSID = prefs.getString("wifi_ssid", "");
  cfg.wifiPassword = prefs.getString("wifi_pass", "");
  cfg.mqttHost = prefs.getString("mqtt_host", "");
  cfg.mqttPort = prefs.getUShort("mqtt_port", 1883);
  cfg.mqttUser = prefs.getString("mqtt_user", "");
  cfg.mqttPassword = prefs.getString("mqtt_pass", "");
  cfg.deviceID = prefs.getString("device_id", "");
  cfg.sleepSeconds = prefs.getULong("sleep_s", 300);
  cfg.otaWindowMs = prefs.getULong("ota_ms", DEFAULT_OTA_WINDOW_MS);
  cfg.activeChannels = prefs.getInt("act_ch", 2);
  cfg.activeHives = prefs.getInt("act_hives", 1);
  cfg.gtsStartYear = prefs.getInt("gts_year", 0);
  cfg.gtsStartValue = prefs.getFloat("gts_value", 0.0f);
  cfg.gtsStartConfigured = prefs.getBool("gts_cfg", false);

  if (cfg.activeChannels < 1) cfg.activeChannels = 1;
  if (cfg.activeChannels > MAX_CHANNELS) cfg.activeChannels = MAX_CHANNELS;
  if (cfg.activeHives < 1) cfg.activeHives = 1;
  if (cfg.activeHives > MAX_HIVES) cfg.activeHives = MAX_HIVES;
  if (cfg.otaWindowMs > MAX_OTA_WINDOW_MS) cfg.otaWindowMs = MAX_OTA_WINDOW_MS;
  if (cfg.sleepSeconds < MIN_SLEEP_SECONDS) cfg.sleepSeconds = 300;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    String p = "ch" + String(i) + "_";
    chCfg[i].enabled = prefs.getBool((p + "en").c_str(), i < cfg.activeChannels);
    chCfg[i].doutPin = prefs.getInt((p + "dout").c_str(), DEFAULT_DOUT_PINS[i]);
    chCfg[i].sckPin = prefs.getInt((p + "sck").c_str(), DEFAULT_SCK_PINS[i]);
    chCfg[i].hiveIndex = prefs.getInt((p + "hive").c_str(), i / 2);
    chCfg[i].channelName = prefs.getString((p + "name").c_str(), "Kanal " + String(i));

    String cp = "cal" + String(i) + "_";
    chCal[i].count = prefs.getInt((cp + "cnt").c_str(), 0);
    chCal[i].validModel = prefs.getBool((cp + "valid").c_str(), false);
    chCal[i].scale = prefs.getDouble((cp + "scale").c_str(), 1.0);
    chCal[i].offset = prefs.getDouble((cp + "offset").c_str(), 0.0);
    chCal[i].tempCompKgPerC = prefs.getDouble((cp + "tcomp").c_str(), 0.0);
    for (int j = 0; j < MAX_CAL_POINTS; j++) {
      chCal[i].points[j].raw = prefs.getFloat((cp + "raw" + String(j)).c_str(), 0.0f);
      chCal[i].points[j].kg = prefs.getFloat((cp + "kg" + String(j)).c_str(), 0.0f);
    }
    String err;
    if (chCal[i].count >= 2) {
      recomputeCalibrationModelInMemory(i, err);
    } else {
      chCal[i].validModel = false;
    }
  }
  prefs.end();
  bool ok = cfg.wifiSSID.length() > 0 && cfg.mqttHost.length() > 0 && cfg.deviceID.length() > 0;
  return ok;
}

bool connectWiFiWithCredentials(const String& ssid, const String& password, unsigned long timeoutMs) {
  DBG("WiFi connect start ssid=%s timeoutMs=%lu", ssid.c_str(), timeoutMs);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
  }
  DBG("WiFi connect result status=%d ip=%s", WiFi.status(), WiFi.localIP().toString().c_str());
  return WiFi.status() == WL_CONNECTED;
}

bool connectWiFi() {
  return connectWiFiWithCredentials(cfg.wifiSSID, cfg.wifiPassword, WIFI_CONNECT_TIMEOUT_MS);
}

bool mqttConnectWithCredentials(PubSubClient& client, const String& host, uint16_t port,
                                const String& user, const String& password,
                                const String& clientID, unsigned long timeoutMs) {
  DBG("MQTT connect start host=%s port=%u clientID=%s timeoutMs=%lu", host.c_str(), port, clientID.c_str(), timeoutMs);
  client.setServer(host.c_str(), port);
  client.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  client.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  client.setBufferSize(MQTT_BUFFER_SIZE);

  unsigned long start = millis();
  while (!client.connected() && millis() - start < timeoutMs) {
    bool ok = false;
    if (user.length()) ok = client.connect(clientID.c_str(), user.c_str(), password.c_str());
    else ok = client.connect(clientID.c_str());
    DBG("MQTT connect attempt connected=%d state=%d", client.connected() ? 1 : 0, client.state());
    if (!ok) delay(500);
  }
  DBG("MQTT connect end connected=%d state=%d", client.connected() ? 1 : 0, client.state());
  return client.connected();
}

bool connectMQTT() {
  return mqttConnectWithCredentials(mqttClient, cfg.mqttHost, cfg.mqttPort, cfg.mqttUser, cfg.mqttPassword, cfg.deviceID + "-pub", MQTT_CONNECT_TIMEOUT_MS);
}

String testMQTTConnection(const String& wifiSSID, const String& wifiPass,
                          const String& mqttHost, uint16_t mqttPort,
                          const String& mqttUser, const String& mqttPass,
                          const String& deviceID) {
  DBG("Test MQTT start device=%s host=%s port=%u", deviceID.c_str(), mqttHost.c_str(), mqttPort);
  WiFi.disconnect(true, true);
  delay(200);

  if (!connectWiFiWithCredentials(wifiSSID, wifiPass, WIFI_CONNECT_TIMEOUT_MS)) {
    DBG("Test MQTT WiFi failed");
    return "WLAN-Verbindung fehlgeschlagen";
  }

  WiFiClient testWiFiClient;
  PubSubClient testClient(testWiFiClient);
  testClient.setServer(mqttHost.c_str(), mqttPort);
  testClient.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  testClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  testClient.setBufferSize(MQTT_BUFFER_SIZE);

  unsigned long start = millis();
  while (!testClient.connected() && millis() - start < MQTT_CONNECT_TIMEOUT_MS) {
    bool ok = false;
    if (mqttUser.length()) ok = testClient.connect((deviceID + "-test").c_str(), mqttUser.c_str(), mqttPass.c_str());
    else ok = testClient.connect((deviceID + "-test").c_str());
    DBG("Test MQTT connect attempt connected=%d state=%d", testClient.connected() ? 1 : 0, testClient.state());
    if (!ok) delay(300);
  }

  if (!testClient.connected()) {
    DBG("Test MQTT connect failed");
    WiFi.disconnect(true, true);
    delay(100);
    return "MQTT-Verbindung fehlgeschlagen";
  }

  String topic = "devices/" + deviceID + "/telemetry/config-test";
  String payload = "{\"device_id\":\"" + deviceID + "\",\"type\":\"config-test\"}";
  bool published = testClient.publish(topic.c_str(), payload.c_str(), false);
  DBG("Test MQTT publish topic=%s ok=%d", topic.c_str(), published ? 1 : 0);

  testClient.loop();
  delay(100);

  if (testClient.connected()) {
    DBG("Test MQTT disconnect");
    testClient.disconnect();
    delay(100);
  }

  WiFi.disconnect(true, true);
  delay(100);

  if (!published) {
    return "MQTT verbunden, Test-Publish fehlgeschlagen";
  }

  DBG("Test MQTT success");
  return "";
}

void setupScales() {
  DBG("setupScales activeChannels=%d", cfg.activeChannels);
  delay(SENSOR_WARMUP_MS);
  for (int i = 0; i < cfg.activeChannels; i++) {
    if (!chCfg[i].enabled || chCfg[i].doutPin < 0 || chCfg[i].sckPin < 0) {
      hxBegun[i] = false;
      DBG("Channel %d disabled", i);
      continue;
    }
    hx[i].begin(chCfg[i].doutPin, chCfg[i].sckPin);
    hxBegun[i] = true;
    DBG("Channel %d begin dout=%d sck=%d hive=%d", i, chCfg[i].doutPin, chCfg[i].sckPin, chCfg[i].hiveIndex);
  }
}

float readHXRawAvg(int idx, int samples) {
  if (idx < 0 || idx >= cfg.activeChannels || !hxBegun[idx]) return 0.0f;
  float sum = 0.0f;
  int got = 0;
  unsigned long start = millis();
  while (got < samples && millis() - start < 5000) {
    if (hx[idx].is_ready()) {
      sum += (float)hx[idx].read();
      got++;
    } else {
      delay(5);
    }
  }
  if (got == 0) return 0.0f;
  return sum / (float)got;
}

bool recomputeCalibrationModel(int idx, String& errorMsg) {
  if (chCal[idx].count < 2) {
    chCal[idx].validModel = false;
    errorMsg = "Mindestens 2 Kalibrierpunkte noetig";
    return false;
  }

  double sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
  for (int i = 0; i < chCal[idx].count; i++) {
    double x = chCal[idx].points[i].raw;
    double y = chCal[idx].points[i].kg;
    sumX += x;
    sumY += y;
    sumXY += x * y;
    sumXX += x * x;
  }

  double n = (double)chCal[idx].count;
  double denom = n * sumXX - sumX * sumX;
  if (fabs(denom) < 0.0001) {
    chCal[idx].validModel = false;
    errorMsg = "Kalibriermodell nicht berechenbar";
    return false;
  }

  double scale_d = (n * sumXY - sumX * sumY) / denom;
  double offset_d = (sumY - scale_d * sumX) / n;
  chCal[idx].scale = scale_d;
  chCal[idx].offset = offset_d;
  chCal[idx].validModel = true;
  saveCalibrationToPrefs(idx);
  errorMsg = "";
  return true;
}

void removeCalibrationPoint(int idx, int pointIdx) {
  if (pointIdx < 0 || pointIdx >= chCal[idx].count) return;
  for (int i = pointIdx; i < chCal[idx].count - 1; i++) {
    chCal[idx].points[i] = chCal[idx].points[i + 1];
  }
  chCal[idx].count--;
  String err;
  recomputeCalibrationModel(idx, err);
  saveCalibrationToPrefs(idx);
}

void clearCalibration(int idx) {
  chCal[idx] = ChannelCalibration();
  saveCalibrationToPrefs(idx);
}

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_ADC_PIN);
  float v = ((float)raw / (float)ADC_MAX) * ADC_REFERENCE_VOLT * BATTERY_DIVIDER_MULTIPLIER;
  return v;
}

float readTemperatureC() {
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C || isnan(t)) return NAN;
  return t;
}

float computeStdDev(float* vals, int n, float mean) {
  if (n <= 1) return 0.0f;
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    float d = vals[i] - mean;
    sum += d * d;
  }
  return sqrt(sum / (float)n);
}

ChannelReading readChannel(int idx, float temperatureC) {
  ChannelReading r;
  if (!hxBegun[idx]) return r;

  const int SAMPLES = 10;
  float vals[SAMPLES];
  int got = 0;
  unsigned long start = millis();
  while (got < SAMPLES && millis() - start < 4000) {
    if (hx[idx].is_ready()) {
      vals[got++] = (float)hx[idx].read();
    } else {
      delay(5);
    }
  }
  if (got < 2) return r;

  r.ready = true;
  r.samples = got;
  r.rawMin = vals[0];
  r.rawMax = vals[0];
  float sum = 0.0f;
  for (int i = 0; i < got; i++) {
    sum += vals[i];
    if (vals[i] < r.rawMin) r.rawMin = vals[i];
    if (vals[i] > r.rawMax) r.rawMax = vals[i];
  }
  r.rawAvg = sum / (float)got;
  r.rawStdDev = computeStdDev(vals, got, r.rawAvg);
  r.rawSlope = (vals[got - 1] - vals[0]) / (float)(got - 1);
  r.stable = r.rawStdDev <= STABLE_STDDEV_THRESHOLD;
  r.driftDetected = fabs(r.rawSlope) >= DRIFT_SLOPE_THRESHOLD;
  r.calibrated = chCal[idx].validModel;
  DBG("CH %d cal valid=%d scale=%.8f offset=%.4f rawAvg=%.2f",
      idx, chCal[idx].validModel ? 1 : 0, chCal[idx].scale, chCal[idx].offset, r.rawAvg);
  if (r.ready && r.calibrated) {
    r.weightKg = chCal[idx].scale * r.rawAvg + chCal[idx].offset;
    r.compensatedWeightKg = r.weightKg;
    if (!isnan(temperatureC)) {
      r.compensatedWeightKg -= ((temperatureC - 20.0f) * chCal[idx].tempCompKgPerC);
    }
  }
  return r;
}

String formatTimestampISO8601() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    return "";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

String buildTelemetryPayload(float temperatureC, float batteryV, int rssi, ChannelReading* readings) {
  DynamicJsonDocument doc(8192);
  doc["schema"] = "telemetry.v4";
  doc["device_id"] = cfg.deviceID;
  String ts = formatTimestampISO8601();
  if (ts.length()) doc["timestamp"] = ts;
  if (!isnan(temperatureC)) doc["temperature_c"] = (double)temperatureC;
  doc["battery_v"] = (double)batteryV;
  doc["rssi"] = rssi;
  doc["sleep_seconds"] = (int)cfg.sleepSeconds;
  doc["firmware_version"] = "esp32-telemetry-v7-full-debug-v4-edit-cal";
  doc["active_hives"] = cfg.activeHives;

  if (cfg.gtsStartConfigured) {
    doc["gts_start_year"] = cfg.gtsStartYear;
    doc["gts_start_value"] = cfg.gtsStartValue;
  }

  JsonArray channels = doc.createNestedArray("channels");
  double hiveWeight[MAX_HIVES] = {0};
  double hiveCompWeight[MAX_HIVES] = {0};
  int hiveChannelCount[MAX_HIVES] = {0};

  for (int i = 0; i < cfg.activeChannels; i++) {
    JsonObject ch = channels.createNestedObject();
    ch["channel_index"] = i;
    ch["channel_name"] = chCfg[i].channelName;
    ch["hive_index"] = chCfg[i].hiveIndex;
    ch["hive_name"] = "Beute " + String(chCfg[i].hiveIndex);
    ch["dout_pin"] = chCfg[i].doutPin;
    ch["sck_pin"] = chCfg[i].sckPin;
    ch["ready"] = readings[i].ready;
    ch["stable"] = readings[i].stable;
    ch["drift_detected"] = readings[i].driftDetected;
    ch["calibrated"] = readings[i].calibrated;
    ch["samples"] = readings[i].samples;
    ch["raw_avg"] = (double)readings[i].rawAvg;
    ch["raw_min"] = (double)readings[i].rawMin;
    ch["raw_max"] = (double)readings[i].rawMax;
    ch["raw_stddev"] = (double)readings[i].rawStdDev;
    ch["raw_slope"] = (double)readings[i].rawSlope;
    ch["weight_kg"] = readings[i].weightKg;
    ch["compensated_weight_kg"] = readings[i].compensatedWeightKg;
    ch["cal_scale"] = chCal[i].scale;
    ch["cal_offset"] = chCal[i].offset;

    int hiveIdx = chCfg[i].hiveIndex;
    if (hiveIdx >= 0 && hiveIdx < MAX_HIVES) {
      hiveWeight[hiveIdx] += readings[i].weightKg;
      hiveCompWeight[hiveIdx] += readings[i].compensatedWeightKg;
      hiveChannelCount[hiveIdx]++;
    }
  }

  JsonArray hives = doc.createNestedArray("hives");
  for (int h = 0; h < cfg.activeHives; h++) {
    JsonObject obj = hives.createNestedObject();
    obj["hive_index"] = h;
    obj["hive_name"] = "Beute " + String(h);
    obj["channel_count"] = hiveChannelCount[h];
    obj["weight_kg"] = hiveWeight[h];
    obj["compensated_weight_kg"] = hiveCompWeight[h];
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void setupTimeIfPossible() {
  DBG("Configuring NTP");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
}

void configureOTA() {
  DBG("Configuring OTA hostname=%s", cfg.deviceID.c_str());
  ArduinoOTA.setHostname(cfg.deviceID.c_str());
  ArduinoOTA.onStart([]() { DBG("OTA start"); });
  ArduinoOTA.onEnd([]() { DBG("OTA end"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    (void)progress; (void)total;
  });
  ArduinoOTA.onError([](ota_error_t error) {
    DBG("OTA error=%d", (int)error);
  });
  ArduinoOTA.begin();
  otaStartedAt = millis();
  otaActive = true;
}

void enterDeepSleep() {
  DBG("enterDeepSleep sleepSeconds=%lu", cfg.sleepSeconds);
  esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepSeconds * 1000000ULL);
  DBG("Deep sleep start now");
  Serial.flush();
  esp_deep_sleep_start();
}

void handleRoot() {
  String html = htmlHeader("BeeLife Setup");
  html += "<div class='box'><form method='POST' action='/save'>";
  html += "<label>WLAN SSID</label><input name='wifi_ssid' value='" + cfg.wifiSSID + "'>";
  html += "<label>WLAN Passwort</label><input name='wifi_pass' type='password' value='" + cfg.wifiPassword + "'>";
  html += "<label>MQTT Host</label><input name='mqtt_host' value='" + cfg.mqttHost + "'>";
  html += "<label>MQTT Port</label><input name='mqtt_port' value='" + String(cfg.mqttPort) + "'>";
  html += "<label>MQTT User</label><input name='mqtt_user' value='" + cfg.mqttUser + "'>";
  html += "<label>MQTT Passwort</label><input name='mqtt_pass' type='password' value='" + cfg.mqttPassword + "'>";
  html += "<label>Device ID</label><input name='device_id' value='" + cfg.deviceID + "'>";
  html += "<label>Sleep Sekunden</label><input name='sleep_seconds' value='" + String(cfg.sleepSeconds) + "'>";
  html += "<label>OTA Fenster (ms)</label><input name='ota_window_ms' value='" + String(cfg.otaWindowMs) + "'>";
  html += "<label>Aktive Kanaele</label><input name='active_channels' value='" + String(cfg.activeChannels) + "'>";
  html += "<label>Aktive Beuten</label><input name='active_hives' value='" + String(cfg.activeHives) + "'>";
  html += "<label>GTS Startjahr (optional)</label><input name='gts_year' value='" + String(cfg.gtsStartYear) + "'>";
  html += "<label>GTS Startwert (optional)</label><input name='gts_value' value='" + String(cfg.gtsStartValue, 2) + "'>";
  html += "<button type='submit'>Speichern</button></form></div>";
  html += "<a href='/channels'>Kanaele</a>";
  html += "<a href='/calibration'>Kalibrierung</a>";
  html += "<a href='/tempcomp'>Temperaturkompensation</a>";
  html += "<a href='/diag'>Diagnose</a>";
  html += "<a href='/scan'>WLAN Scan</a>";
  html += "<a href='/ota'>OTA Info</a>";
  html += "<a href='/reset'>Reset</a>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  configPortalStartedAt = millis();
  DBG("handleSave start");
  String wifiSSID = server.arg("wifi_ssid");
  String wifiPass = server.arg("wifi_pass");
  String mqttHost = server.arg("mqtt_host");
  uint16_t mqttPort = (uint16_t)server.arg("mqtt_port").toInt();
  if (mqttPort == 0) mqttPort = 1883;
  String mqttUser = server.arg("mqtt_user");
  String mqttPass = server.arg("mqtt_pass");
  String deviceID = server.arg("device_id");
  unsigned long sleepSeconds = (unsigned long)server.arg("sleep_seconds").toInt();
  if (sleepSeconds < MIN_SLEEP_SECONDS) sleepSeconds = 300;
  unsigned long otaWindowMs = (unsigned long)server.arg("ota_window_ms").toInt();
  if (otaWindowMs > MAX_OTA_WINDOW_MS) otaWindowMs = MAX_OTA_WINDOW_MS;
  int activeChannels = server.arg("active_channels").toInt();
  int activeHives = server.arg("active_hives").toInt();
  if (activeChannels < 1) activeChannels = 1;
  if (activeChannels > MAX_CHANNELS) activeChannels = MAX_CHANNELS;
  if (activeHives < 1) activeHives = 1;
  if (activeHives > MAX_HIVES) activeHives = MAX_HIVES;

  String gtsYearStr = server.arg("gts_year");
  String gtsValueStr = server.arg("gts_value");
  if ((gtsYearStr.length() == 0) != (gtsValueStr.length() == 0)) {
    DBG("handleSave invalid GTS fields");
    server.send(400, "text/plain", "GTS Startjahr und Startwert muessen beide gesetzt oder beide leer sein");
    return;
  }

  String mqttErr = testMQTTConnection(wifiSSID, wifiPass, mqttHost, mqttPort, mqttUser, mqttPass, deviceID);
  if (mqttErr.length()) {
    DBG("handleSave mqttErr=%s", mqttErr.c_str());
    server.send(400, "text/plain", mqttErr);
    return;
  }

  cfg.wifiSSID = wifiSSID;
  cfg.wifiPassword = wifiPass;
  cfg.mqttHost = mqttHost;
  cfg.mqttPort = mqttPort;
  cfg.mqttUser = mqttUser;
  cfg.mqttPassword = mqttPass;
  cfg.deviceID = deviceID;
  cfg.sleepSeconds = sleepSeconds;
  cfg.otaWindowMs = otaWindowMs;
  cfg.activeChannels = activeChannels;
  cfg.activeHives = activeHives;

  if (gtsYearStr.length()) {
    cfg.gtsStartYear = gtsYearStr.toInt();
    cfg.gtsStartValue = gtsValueStr.toFloat();
    cfg.gtsStartConfigured = true;
  } else {
    cfg.gtsStartYear = 0;
    cfg.gtsStartValue = 0.0f;
    cfg.gtsStartConfigured = false;
  }

  for (int i = 0; i < MAX_CHANNELS; i++) {
    chCfg[i].enabled = i < cfg.activeChannels;
    saveChannelConfig(i);
  }
  saveBasicConfig();

  DBG("handleSave saved config sleepSeconds=%lu otaWindowMs=%lu", cfg.sleepSeconds, cfg.otaWindowMs);

  server.send(200, "text/plain", "Konfiguration gespeichert. Neustart...");
  delay(1000);
  ESP.restart();
}

void handleChannelsPage() {
  String html = htmlHeader("Kanaele");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    html += "<div class='box'><form method='POST' action='/channel-save'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<h3>Kanal " + String(i) + "</h3>";
    html += "<label>Enabled</label><select name='enabled'><option value='1'";
    if (chCfg[i].enabled) html += " selected";
    html += ">Ja</option><option value='0'";
    if (!chCfg[i].enabled) html += " selected";
    html += ">Nein</option></select>";
    html += "<label>Name</label><input name='name' value='" + chCfg[i].channelName + "'>";
    html += "<label>DOUT Pin</label><input name='dout' value='" + String(chCfg[i].doutPin) + "'>";
    html += "<label>SCK Pin</label><input name='sck' value='" + String(chCfg[i].sckPin) + "'>";
    html += "<label>Beute Index</label><input name='hive' value='" + String(chCfg[i].hiveIndex) + "'>";
    html += "<button type='submit'>Speichern</button></form></div>";
  }
  html += "<a href='/'>Zurueck</a></body></html>";
  server.send(200, "text/html", html);
}

void handleChannelSave() {
  configPortalStartedAt = millis();
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= MAX_CHANNELS) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }
  chCfg[idx].enabled = server.arg("enabled") == "1";
  chCfg[idx].channelName = server.arg("name");
  chCfg[idx].doutPin = server.arg("dout").toInt();
  chCfg[idx].sckPin = server.arg("sck").toInt();
  chCfg[idx].hiveIndex = server.arg("hive").toInt();
  if (chCfg[idx].hiveIndex < 0) chCfg[idx].hiveIndex = 0;
  if (chCfg[idx].hiveIndex >= MAX_HIVES) chCfg[idx].hiveIndex = MAX_HIVES - 1;
  saveChannelConfig(idx);
  server.sendHeader("Location", "/channels");
  server.send(302, "text/plain", "OK");
}

void handleCalibrationPage() {
  String html = htmlHeader("Kalibrierung");
  for (int i = 0; i < cfg.activeChannels; i++) {
    html += "<div class='box'><h3>Kanal " + String(i) + "</h3>";
    html += "<p>Valid: " + String(chCal[i].validModel ? "Ja" : "Nein") + " | Punkte: " + String(chCal[i].count) + " | Scale: " + String(chCal[i].scale, 8) + " | Offset: " + String(chCal[i].offset, 4) + "</p>";
    html += "<form method='POST' action='/calibration-capture'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<label>Referenzgewicht (kg)</label><input name='kg'>";
    html += "<button type='submit'>Punkt aufnehmen</button></form>";
    html += "<form method='POST' action='/calibration-clear'><input type='hidden' name='idx' value='" + String(i) + "'><button type='submit'>Kalibrierung loeschen</button></form>";
    html += "<ul>";
    for (int j = 0; j < chCal[i].count; j++) {
    html += "<li>";
    html += "raw=" + String(chCal[i].points[j].raw, 2) + " kg=" + String(chCal[i].points[j].kg, 3);
    html += "<form method='POST' action='/calibration-edit' style='margin-top:6px'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<input type='hidden' name='p' value='" + String(j) + "'>";
    html += "<label>Referenzgewicht Punkt " + String(j) + " (kg)</label>";
    html += "<input name='kg' value='" + String(chCal[i].points[j].kg, 3) + "'>";
    html += "<button type='submit'>Gewicht aktualisieren</button>";
    html += "</form>";
    html += "<a href='/calibration-delete?idx=" + String(i) + "&p=" + String(j) + "'>loeschen</a>";
    html += "</li>";
  }
    html += "</ul></div>";
  }
  html += "<a href='/'>Zurueck</a></body></html>";
  server.send(200, "text/html", html);
}

void handleCalibrationCapture() {
  configPortalStartedAt = millis();
  int idx = server.arg("idx").toInt();
  float kg = parseLocalizedFloat(server.arg("kg"));
  if (idx < 0 || idx >= cfg.activeChannels) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }
  if (chCal[idx].count >= MAX_CAL_POINTS) {
    server.send(400, "text/plain", "Maximale Punkte erreicht");
    return;
  }

  float raw = readHXRawAvg(idx, 10);
  chCal[idx].points[chCal[idx].count].raw = raw;
  chCal[idx].points[chCal[idx].count].kg = kg;
  chCal[idx].count++;

  String err;
  recomputeCalibrationModel(idx, err);
  saveCalibrationToPrefs(idx);



  server.sendHeader("Location", "/calibration");
  server.send(302, "text/plain", "OK");
}

void handleCalibrationDelete() {
  configPortalStartedAt = millis();
  int idx = server.arg("idx").toInt();
  int p = server.arg("p").toInt();
  removeCalibrationPoint(idx, p);

  server.sendHeader("Location", "/calibration");
  server.send(302, "text/plain", "OK");
}

void handleCalibrationClear() {
  configPortalStartedAt = millis();
  int idx = server.arg("idx").toInt();
  clearCalibration(idx);

  server.sendHeader("Location", "/calibration");
  server.send(302, "text/plain", "OK");
}

void handleTempCompPage() {
  String html = htmlHeader("Temperaturkompensation");
  for (int i = 0; i < cfg.activeChannels; i++) {
    html += "<div class='box'><form method='POST' action='/tempcomp-save'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<h3>Kanal " + String(i) + "</h3>";
    html += "<label>kg pro °C</label><input name='tcomp' value='" + String(chCal[i].tempCompKgPerC, 5) + "'>";
    html += "<button type='submit'>Speichern</button></form></div>";
  }
  html += "<a href='/'>Zurueck</a></body></html>";
  server.send(200, "text/html", html);
}

void handleTempCompSave() {
  configPortalStartedAt = millis();
  int idx = server.arg("idx").toInt();
  if (idx < 0 || idx >= cfg.activeChannels) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }
  chCal[idx].tempCompKgPerC = server.arg("tcomp").toFloat();
  saveCalibrationToPrefs(idx);
  server.sendHeader("Location", "/tempcomp");
  server.send(302, "text/plain", "OK");
}

void handleOTAInfo() {
  String html = htmlHeader("OTA");
  html += "<p>OTA aktiv: " + String(otaActive ? "Ja" : "Nein") + "</p>";
  html += "<p>OTA Fenster ms: " + String(cfg.otaWindowMs) + "</p>";
  html += "<a href='/'>Zurueck</a></body></html>";
  server.send(200, "text/html", html);
}

void handleScan() {
  String html = htmlHeader("WLAN Scan");
  int n = WiFi.scanNetworks();
  html += "<ul>";
  for (int i = 0; i < n; i++) {
    html += "<li>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</li>";
  }
  html += "</ul><a href='/'>Zurueck</a></body></html>";
  server.send(200, "text/html", html);
}

void handleDiag() {
  float t = readTemperatureC();
  float b = readBatteryVoltage();
  String html = htmlHeader("Diagnose");
  html += "<div class='box'>";
  html += "<p>WiFi: " + String(WiFi.status() == WL_CONNECTED ? "verbunden" : "nicht verbunden") + "</p>";
  html += "<p>MQTT: " + String(mqttClient.connected() ? "verbunden" : "nicht verbunden") + "</p>";
  html += "<p>Temperatur: " + String(t, 2) + " °C</p>";
  html += "<p>Batterie: " + String(b, 3) + " V</p>";
  for (int i = 0; i < cfg.activeChannels; i++) {
    float raw = readHXRawAvg(i, 5);
    html += "<p>Kanal " + String(i) + " raw avg: " + String(raw, 2) + "</p>";
  }
  html += "</div><a href='/'>Zurueck</a></body></html>";
  server.send(200, "text/html", html);
}

void handleReset() {
  resetConfig();
  server.send(200, "text/plain", "Konfiguration geloescht. Neustart...");
  delay(1000);
  ESP.restart();
}

void updateCalibrationPointKg(int idx, int pointIdx, float kg) {
  if (idx < 0 || idx >= MAX_CHANNELS) return;
  if (pointIdx < 0 || pointIdx >= chCal[idx].count) return;
  if (kg <= 0.0f) return;

  chCal[idx].points[pointIdx].kg = kg;

  String err;
  recomputeCalibrationModel(idx, err);
  saveCalibrationToPrefs(idx);
}

void handleCalibrationEdit() {
  configPortalStartedAt = millis();
  int idx = server.arg("idx").toInt();
  int p = server.arg("p").toInt();
  float kg = parseLocalizedFloat(server.arg("kg"));

  if (idx < 0 || idx >= cfg.activeChannels) {
    server.send(400, "text/plain", "ungueltiger Kanal");
    return;
  }

  if (p < 0 || p >= chCal[idx].count) {
    server.send(400, "text/plain", "ungueltiger Punkt");
    return;
  }

  if (kg <= 0.0f) {
    server.send(400, "text/plain", "Referenzgewicht muss > 0 sein");
    return;
  }

  updateCalibrationPointKg(idx, p, kg);

  server.sendHeader("Location", "/calibration");
  server.send(302, "text/plain", "OK");
}

void startConfigPortal() {
  DBG("ENTERING CONFIG PORTAL");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  DBG("AP started ssid=%s ip=%s", AP_SSID, WiFi.softAPIP().toString().c_str());
  delay(200);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/channels", handleChannelsPage);
  server.on("/channel-save", HTTP_POST, handleChannelSave);
  server.on("/calibration", handleCalibrationPage);
  server.on("/calibration-capture", HTTP_POST, handleCalibrationCapture);
  server.on("/calibration-edit", HTTP_POST, handleCalibrationEdit);
  server.on("/calibration-delete", HTTP_GET, handleCalibrationDelete);
  server.on("/calibration-clear", HTTP_POST, handleCalibrationClear);
  server.on("/tempcomp", handleTempCompPage);
  server.on("/tempcomp-save", HTTP_POST, handleTempCompSave);
  server.on("/ota", handleOTAInfo);
  server.on("/scan", handleScan);
  server.on("/diag", handleDiag);
  server.on("/reset", handleReset);
  server.begin();

  configPortalStartedAt = millis();
  while (millis() - configPortalStartedAt < CONFIG_TIMEOUT_MS) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2);
  }
  DBG("Config portal timeout -> restart");
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  DBG("BOOT");
  analogReadResolution(12);
  ds18b20.begin();

  pinMode(0, INPUT_PULLUP);
  delay(50);
  bool bootPressed = (digitalRead(0) == LOW);

  setDefaultConfig();
  bool haveConfig = loadConfig();

  DBG("CFG haveConfig=%d", haveConfig ? 1 : 0);
  DBG("CFG deviceID=%s", cfg.deviceID.c_str());
  DBG("CFG wifiSSID=%s", cfg.wifiSSID.c_str());
  DBG("CFG mqttHost=%s", cfg.mqttHost.c_str());
  DBG("CFG mqttPort=%u", cfg.mqttPort);
  DBG("CFG sleepSeconds=%lu", cfg.sleepSeconds);
  DBG("CFG otaWindowMs=%lu", cfg.otaWindowMs);
  DBG("CFG activeChannels=%d activeHives=%d", cfg.activeChannels, cfg.activeHives);

  setupScales();
  DBG("Scales initialized before portal decision");

  if (bootPressed) {
    DBG("BOOT button pressed -> force config portal");
    startConfigPortal();
    return;
  }

  if (!haveConfig) {
    DBG("RETURN PATH: no config");
    startConfigPortal();
    return;
  }

  const int WIFI_RETRY_MAX = 3;
  int wifiRetry = 0;
  bool wifiOk = false;
  for (wifiRetry = 0; wifiRetry < WIFI_RETRY_MAX; ++wifiRetry) {
    if (connectWiFi()) {
      wifiOk = true;
      break;
    }
    DBG("WiFi retry %d/%d fehlgeschlagen", wifiRetry + 1, WIFI_RETRY_MAX);
    WiFi.disconnect(true, true);
    delay(300);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(200);
    delay(1000);
  }
  if (!wifiOk) {
    DBG("RETURN PATH: wifi failed after %d retries", WIFI_RETRY_MAX);
    startConfigPortal();
    return;
  }
  DBG("STEP 1: WiFi connected");

  setupTimeIfPossible();
  DBG("STEP 2: time configured");

  float temperatureC = readTemperatureC();
  float batteryV = readBatteryVoltage();
  int rssi = WiFi.RSSI();
  DBG("STEP 3: temp=%.2f battery=%.3f rssi=%d", temperatureC, batteryV, rssi);

  ChannelReading readings[MAX_CHANNELS];
  for (int i = 0; i < cfg.activeChannels; i++) {
    readings[i] = readChannel(i, temperatureC);
    DBG("CH %d ready=%d stable=%d drift=%d weight=%.3f comp=%.3f",
        i,
        readings[i].ready ? 1 : 0,
        readings[i].stable ? 1 : 0,
        readings[i].driftDetected ? 1 : 0,
        readings[i].weightKg,
        readings[i].compensatedWeightKg);
        
  }

  const int MQTT_RETRY_MAX = 3;
  int mqttRetry = 0;
  bool mqttOk = false;
  for (mqttRetry = 0; mqttRetry < MQTT_RETRY_MAX; ++mqttRetry) {
    if (connectMQTT()) {
      mqttOk = true;
      break;
    }
    DBG("MQTT retry %d/%d fehlgeschlagen", mqttRetry + 1, MQTT_RETRY_MAX);
    if (mqttClient.connected()) {
      mqttClient.disconnect();
      delay(200);
    }
    delay(1000);
  }
  if (!mqttOk) {
    DBG("RETURN PATH: mqtt failed after %d retries", MQTT_RETRY_MAX);
    startConfigPortal();
    return;
  }
  DBG("STEP 4: MQTT connected");

  String topic = deriveTopicFromDeviceID(cfg.deviceID);
  String payload = buildTelemetryPayload(temperatureC, batteryV, rssi, readings);
  DBG("STEP 5: payload bytes=%u topic=%s", payload.length(), topic.c_str());

  bool published = mqttClient.publish(topic.c_str(), payload.c_str(), true);

  for (int i = 0; i < 25; i++) {
    mqttClient.loop();
    delay(20);
  }

  if (!published) {
    DBG("RETURN PATH: publish failed state=%d", mqttClient.state());

    bool publishRetryOk = false;
    for (int retry = 0; retry < 2; ++retry) {
      DBG("Publish retry %d/2", retry + 1);
      if (!mqttClient.connected()) {
        if (!connectMQTT()) {
          DBG("Publish retry reconnect failed state=%d", mqttClient.state());
          continue;
        }
      }
      published = mqttClient.publish(topic.c_str(), payload.c_str(), true);
      for (int i = 0; i < 25; i++) {
        mqttClient.loop();
        delay(20);
      }
      if (published) {
        publishRetryOk = true;
        break;
      }
    }

    if (!publishRetryOk) {
      startConfigPortal();
      return;
    }
  }

  DBG("STEP 6: publish ok");

  if (cfg.otaWindowMs > 0) {
    DBG("STEP 7: entering OTA window ms=%lu", cfg.otaWindowMs);
    configureOTA();
    unsigned long waitStart = millis();
    while (millis() - waitStart < cfg.otaWindowMs) {
      ArduinoOTA.handle();
      if (mqttClient.connected()) mqttClient.loop();
      delay(10);
    }
    DBG("STEP 8: OTA window finished");
  } else {
    DBG("STEP 7: OTA disabled");
  }

  DBG("STEP 9: disconnect before sleep");

  if (mqttClient.connected()) {
    DBG("MQTT flush before disconnect");
    for (int i = 0; i < 20; i++) {
      mqttClient.loop();
      delay(20);
    }

    DBG("MQTT disconnect");
    mqttClient.disconnect();
    delay(500);
  }

  DBG("WiFi disconnect");
  WiFi.disconnect(true, true);
  delay(500);

  DBG("WiFi off");
  WiFi.mode(WIFI_OFF);
  delay(200);

  DBG("STEP 10: entering deep sleep");
  enterDeepSleep();
}

void loop() {
  if (otaActive) {
    ArduinoOTA.handle();
    if (mqttClient.connected()) mqttClient.loop();
  }
}