#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

// =====================================================
// WIFI + FIREBASE
// =====================================================
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* FIREBASE_DB_URL = "https://erekearduino-default-rtdb.firebaseio.com";
const char* FIREBASE_AUTH   = ""; // если база открыта - оставить пустым

const char* DEVICE_ID        = "esp32-labchip-01";
const char* FIRMWARE_VERSION = "2.3-labchip-stage2-final-control";

// =====================================================
// OUTPUT PINS
// =====================================================
#define PIN_HEATER 25
#define PIN_VENT   26
#define PIN_PUMP   27
#define PIN_MIXER  16
#define PIN_VALVE  17

// =====================================================
// SETTINGS
// =====================================================
struct Settings {
  float gasTempMin;
  float gasTempMax;
  float ch4Low;
  float h2sHigh;
  float o2High;
  float phMin;
  float phMax;
  float ecMax;
  float doMin;
  float ammoniumHigh;
  float nitrateHigh;
  float vfaHigh;
};

Settings cfg = {
  35.0f,   // gasTempMin
  39.0f,   // gasTempMax
  50.0f,   // ch4Low
  250.0f,  // h2sHigh
  1.2f,    // o2High
  6.8f,    // phMin
  7.8f,    // phMax
  4.5f,    // ecMax
  0.8f,    // doMin
  180.0f,  // ammoniumHigh
  140.0f,  // nitrateHigh
  3.0f     // vfaHigh
};

// =====================================================
// MEASUREMENT
// =====================================================
struct Measurement {
  unsigned long timestampMs;

  // gas
  float ch4;
  float co2;
  float h2sPpm;
  float o2;
  float gasTemperature;
  float humidity;
  float pressure;

  // liquid
  float ph;
  float orp;
  float ec;
  float dissolvedOxygen;
  float liquidTemperature;
  float ammonium;
  float nitrate;
  float vfa;

  // optical / ML
  float nirOrganicIndex;
  float biomassQualityIndex;
  float spectralScore;
  float opticalDensity;
  float colorIndex;
  float turbidityIndex;

  // analytics
  float maturityIndex;
  float fertilizerSuitabilityIndex;
  float sampleQualityScore;
  float toxicityRisk;
  float imbalanceRisk;

  String qualityBand;
  String recommendation;
};

Measurement latest;

// =====================================================
// GLOBAL STATE
// =====================================================
WiFiClientSecure secureClient;

String systemMode = "AUTO";

bool heaterState = false;
bool ventState   = false;
bool pumpState   = false;
bool mixerState  = false;
bool valveState  = false;

bool firebaseOk = false;

// =====================================================
// TIMERS
// =====================================================
unsigned long lastWiFiRetry      = 0;
unsigned long lastReadSensors    = 0;
unsigned long lastPushCurrent    = 0;
unsigned long lastPushHistory    = 0;
unsigned long lastPushHeartbeat  = 0;
unsigned long lastPollControl    = 0;
unsigned long lastPollSettings   = 0;

const unsigned long WIFI_RETRY_INTERVAL     = 5000;
const unsigned long READ_INTERVAL           = 1000;
const unsigned long PUSH_CURRENT_INTERVAL   = 1500;
const unsigned long PUSH_HISTORY_INTERVAL   = 5000;
const unsigned long PUSH_HEARTBEAT_INTERVAL = 3000;
const unsigned long POLL_CONTROL_INTERVAL   = 800;
const unsigned long POLL_SETTINGS_INTERVAL  = 2500;

// =====================================================
// HELPERS
// =====================================================
float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

float randomf(float a, float b) {
  long r = random(0, 10000);
  return a + (b - a) * (float)r / 10000.0f;
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", " ");
  s.replace("\r", " ");
  return s;
}

String firebaseUrl(const String& path) {
  String base = String(FIREBASE_DB_URL);
  if (base.endsWith("/")) base.remove(base.length() - 1);

  String url = base + path + ".json";
  if (String(FIREBASE_AUTH).length() > 0) {
    url += "?auth=" + String(FIREBASE_AUTH);
  }
  return url;
}

bool firebasePut(const String& path, const String& json) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  secureClient.setInsecure();

  if (!http.begin(secureClient, firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");

  int code = http.PUT(json);
  http.end();

  return (code >= 200 && code < 300);
}

bool firebasePost(const String& path, const String& json) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  secureClient.setInsecure();

  if (!http.begin(secureClient, firebaseUrl(path))) return false;
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(json);
  http.end();

  return (code >= 200 && code < 300);
}

String firebaseGet(const String& path) {
  if (WiFi.status() != WL_CONNECTED) return "";

  HTTPClient http;
  secureClient.setInsecure();

  if (!http.begin(secureClient, firebaseUrl(path))) return "";
  int code = http.GET();
  String body = "";
  if (code >= 200 && code < 300) {
    body = http.getString();
  }
  http.end();
  return body;
}

float extractJsonNumber(const String& json, const String& key, float fallbackValue) {
  String pattern = "\"" + key + "\":";
  int p = json.indexOf(pattern);
  if (p < 0) return fallbackValue;

  p += pattern.length();
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r')) p++;

  int end = p;
  while (end < (int)json.length()) {
    char c = json[end];
    if (c == ',' || c == '}' || c == '\n' || c == '\r') break;
    end++;
  }

  String token = json.substring(p, end);
  token.trim();
  if (token.length() == 0) return fallbackValue;

  return token.toFloat();
}

bool extractJsonBool(const String& json, const String& key, bool fallbackValue) {
  String pattern = "\"" + key + "\":";
  int p = json.indexOf(pattern);
  if (p < 0) return fallbackValue;

  p += pattern.length();
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r')) p++;

  if (json.startsWith("true", p)) return true;
  if (json.startsWith("false", p)) return false;
  return fallbackValue;
}

String extractJsonString(const String& json, const String& key, const String& fallbackValue) {
  String pattern = "\"" + key + "\":";
  int p = json.indexOf(pattern);
  if (p < 0) return fallbackValue;

  p += pattern.length();
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r')) p++;

  if (p >= (int)json.length() || json[p] != '"') return fallbackValue;
  p++;

  int end = p;
  while (end < (int)json.length()) {
    if (json[end] == '"' && json[end - 1] != '\\') break;
    end++;
  }
  if (end >= (int)json.length()) return fallbackValue;

  return json.substring(p, end);
}

// =====================================================
// OUTPUTS
// =====================================================
void applyOutputs() {
  digitalWrite(PIN_HEATER, heaterState ? HIGH : LOW);
  digitalWrite(PIN_VENT,   ventState   ? HIGH : LOW);
  digitalWrite(PIN_PUMP,   pumpState   ? HIGH : LOW);
  digitalWrite(PIN_MIXER,  mixerState  ? HIGH : LOW);
  digitalWrite(PIN_VALVE,  valveState  ? HIGH : LOW);
}

// =====================================================
// ANALYTICS
// =====================================================
void computeAnalytics(Measurement& m) {
  float phScore =
      (m.ph >= cfg.phMin && m.ph <= cfg.phMax)
          ? 100.0f
          : clampf(100.0f - fabs(m.ph - ((cfg.phMin + cfg.phMax) * 0.5f)) * 80.0f, 0.0f, 100.0f);

  float ecScore  = clampf(100.0f - (m.ec / cfg.ecMax) * 100.0f + 20.0f, 0.0f, 100.0f);
  float doScore  = clampf((m.dissolvedOxygen / max(cfg.doMin, 0.1f)) * 70.0f + 20.0f, 0.0f, 100.0f);
  float nh4Score = clampf(100.0f - (m.ammonium / cfg.ammoniumHigh) * 100.0f, 0.0f, 100.0f);
  float no3Score = clampf(100.0f - (m.nitrate / cfg.nitrateHigh) * 100.0f, 0.0f, 100.0f);
  float vfaScore = clampf(100.0f - (m.vfa / cfg.vfaHigh) * 100.0f, 0.0f, 100.0f);

  m.maturityIndex = clampf(
      0.35f * m.nirOrganicIndex +
      0.25f * m.biomassQualityIndex +
      0.20f * m.spectralScore +
      0.20f * phScore,
      0.0f, 100.0f
  );

  m.fertilizerSuitabilityIndex = clampf(
      0.25f * phScore +
      0.20f * nh4Score +
      0.20f * no3Score +
      0.15f * vfaScore +
      0.20f * m.biomassQualityIndex,
      0.0f, 100.0f
  );

  m.toxicityRisk = clampf(
      0.45f * (m.ammonium / cfg.ammoniumHigh) * 100.0f +
      0.25f * (m.vfa / cfg.vfaHigh) * 100.0f +
      0.15f * (m.o2 / cfg.o2High) * 100.0f +
      0.15f * (m.h2sPpm / cfg.h2sHigh) * 100.0f,
      0.0f, 100.0f
  );

  float phImb = 0.0f;
  if (m.ph < cfg.phMin) phImb = (cfg.phMin - m.ph) * 40.0f;
  else if (m.ph > cfg.phMax) phImb = (m.ph - cfg.phMax) * 40.0f;

  m.imbalanceRisk = clampf(
      phImb +
      clampf((m.ec - cfg.ecMax) * 18.0f, 0.0f, 35.0f) +
      clampf((cfg.doMin - m.dissolvedOxygen) * 30.0f, 0.0f, 35.0f) +
      clampf((m.vfa - cfg.vfaHigh) * 20.0f, 0.0f, 30.0f),
      0.0f, 100.0f
  );

  m.sampleQualityScore = clampf(
      0.20f * m.maturityIndex +
      0.25f * m.fertilizerSuitabilityIndex +
      0.20f * m.biomassQualityIndex +
      0.15f * m.spectralScore +
      0.10f * doScore +
      0.10f * phScore -
      0.20f * (m.toxicityRisk * 0.5f) -
      0.15f * (m.imbalanceRisk * 0.5f),
      0.0f, 100.0f
  );

  float worstRisk = max(m.toxicityRisk, m.imbalanceRisk);

  if (worstRisk >= 70.0f || m.sampleQualityScore < 40.0f) {
    m.qualityBand = "CRITICAL";
    m.recommendation =
        "Critical band: stop aggressive loading, inspect ammonium/VFA/O2/pH, stabilize process, and repeat analysis immediately.";
  } else if (worstRisk >= 40.0f || m.sampleQualityScore < 70.0f) {
    m.qualityBand = "WARNING";
    m.recommendation =
        "Warning band: adjust process gradually, monitor pH/EC/DO, reduce imbalance factors, and repeat measurement after stabilization.";
  } else {
    m.qualityBand = "NORMAL";
    m.recommendation =
        "Normal band: sample is stable, continue routine operation and periodic monitoring.";
  }
}

// =====================================================
// SENSOR / DEMO MODEL
// =====================================================
void acquireAllMeasurements() {
  latest.timestampMs = millis();

  // Базовые значения, немного зависят от исполнительных механизмов
  float heaterBoost = heaterState ? 1.2f : -0.1f;
  float ventEffect  = ventState   ? -5.0f : 0.0f;
  float pumpEffect  = pumpState   ? 0.35f : -0.10f;
  float mixerEffect = mixerState  ? 0.25f : -0.05f;

  latest.ch4            = clampf(randomf(64.0f, 74.0f) + mixerEffect + ventEffect * 0.15f, 40.0f, 90.0f);
  latest.co2            = clampf(randomf(32.0f, 39.0f) - ventEffect * 0.04f, 15.0f, 60.0f);
  latest.h2sPpm         = clampf(randomf(70.0f, 110.0f) + (ventState ? -12.0f : 0.0f), 10.0f, 400.0f);
  latest.o2             = clampf(randomf(0.8f, 1.5f) + (ventState ? 0.15f : 0.0f), 0.2f, 5.0f);
  latest.gasTemperature = clampf(randomf(35.8f, 37.6f) + heaterBoost, 20.0f, 60.0f);
  latest.humidity       = clampf(randomf(50.0f, 58.0f), 10.0f, 100.0f);
  latest.pressure       = clampf(randomf(1.00f, 1.10f), 0.80f, 1.30f);

  latest.ph                = clampf(randomf(6.8f, 7.4f) + mixerEffect * 0.05f, 4.0f, 10.0f);
  latest.orp               = clampf(randomf(-110.0f, -70.0f), -400.0f, 400.0f);
  latest.ec                = clampf(randomf(3.4f, 4.7f) - pumpEffect * 0.15f, 0.1f, 10.0f);
  latest.dissolvedOxygen   = clampf(randomf(1.5f, 2.4f) + pumpEffect + mixerEffect, 0.0f, 10.0f);
  latest.liquidTemperature = clampf(randomf(31.5f, 34.0f) + heaterBoost * 0.5f, 10.0f, 50.0f);
  latest.ammonium          = clampf(randomf(135.0f, 165.0f), 0.0f, 400.0f);
  latest.nitrate           = clampf(randomf(95.0f, 118.0f), 0.0f, 300.0f);
  latest.vfa               = clampf(randomf(1.2f, 1.9f), 0.0f, 10.0f);

  latest.nirOrganicIndex   = clampf(randomf(62.0f, 74.0f), 0.0f, 100.0f);
  latest.biomassQualityIndex = clampf(randomf(50.0f, 58.0f), 0.0f, 100.0f);
  latest.spectralScore     = clampf(randomf(70.0f, 79.0f), 0.0f, 100.0f);
  latest.opticalDensity    = clampf(randomf(0.45f, 0.75f), 0.0f, 5.0f);
  latest.colorIndex        = clampf(randomf(48.0f, 62.0f), 0.0f, 100.0f);
  latest.turbidityIndex    = clampf(randomf(34.0f, 48.0f), 0.0f, 100.0f);

  computeAnalytics(latest);
}

// =====================================================
// AUTO CONTROL
// =====================================================
void applyAutoControl(const Measurement& m) {
  // простая демонстрационная логика
  heaterState = (m.gasTemperature < cfg.gasTempMin);
  ventState   = (m.h2sPpm > cfg.h2sHigh * 0.55f || m.o2 > cfg.o2High);
  pumpState   = (m.dissolvedOxygen < cfg.doMin + 0.7f);
  mixerState  = (m.imbalanceRisk > 35.0f);
  valveState  = (m.qualityBand == "CRITICAL");

  applyOutputs();
}

// =====================================================
// JSON BUILDERS
// =====================================================
String buildSettingsJson() {
  String json = "{";
  json += "\"gasTempMin\":"   + String(cfg.gasTempMin, 2)   + ",";
  json += "\"gasTempMax\":"   + String(cfg.gasTempMax, 2)   + ",";
  json += "\"ch4Low\":"       + String(cfg.ch4Low, 2)       + ",";
  json += "\"h2sHigh\":"      + String(cfg.h2sHigh, 2)      + ",";
  json += "\"o2High\":"       + String(cfg.o2High, 2)       + ",";
  json += "\"phMin\":"        + String(cfg.phMin, 2)        + ",";
  json += "\"phMax\":"        + String(cfg.phMax, 2)        + ",";
  json += "\"ecMax\":"        + String(cfg.ecMax, 2)        + ",";
  json += "\"doMin\":"        + String(cfg.doMin, 2)        + ",";
  json += "\"ammoniumHigh\":" + String(cfg.ammoniumHigh, 2) + ",";
  json += "\"nitrateHigh\":"  + String(cfg.nitrateHigh, 2)  + ",";
  json += "\"vfaHigh\":"      + String(cfg.vfaHigh, 2);
  json += "}";
  return json;
}

String buildControlJson() {
  String json = "{";
  json += "\"mode\":\"" + systemMode + "\",";
  json += "\"heater\":";
  json += heaterState ? "true," : "false,";
  json += "\"ventilation\":";
  json += ventState ? "true," : "false,";
  json += "\"pump\":";
  json += pumpState ? "true," : "false,";
  json += "\"mixer\":";
  json += mixerState ? "true," : "false,";
  json += "\"valve\":";
  json += valveState ? "true" : "false";
  json += "}";
  return json;
}

String buildCurrentJson(const Measurement& m) {
  String recommendation = jsonEscape(m.recommendation);
  String qualityBand    = jsonEscape(m.qualityBand);

  String json = "{";

  json += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  json += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";

  json += "\"mode\":\"" + systemMode + "\",";
  json += "\"heater\":";
  json += heaterState ? "true," : "false,";
  json += "\"ventilation\":";
  json += ventState ? "true," : "false,";
  json += "\"pump\":";
  json += pumpState ? "true," : "false,";
  json += "\"mixer\":";
  json += mixerState ? "true," : "false,";
  json += "\"valve\":";
  json += valveState ? "true," : "false,";
  json += "\"wifi\":";
  json += (WiFi.status() == WL_CONNECTED) ? "true," : "false,";
  json += "\"firebase\":";
  json += firebaseOk ? "true," : "false,";

  json += "\"ch4\":" + String(m.ch4, 2) + ",";
  json += "\"co2\":" + String(m.co2, 2) + ",";
  json += "\"h2sPpm\":" + String(m.h2sPpm, 2) + ",";
  json += "\"h2s\":" + String(m.h2sPpm, 2) + ",";
  json += "\"o2\":" + String(m.o2, 2) + ",";
  json += "\"gasTemperature\":" + String(m.gasTemperature, 2) + ",";
  json += "\"temperature\":" + String(m.gasTemperature, 2) + ",";
  json += "\"temp\":" + String(m.gasTemperature, 2) + ",";
  json += "\"humidity\":" + String(m.humidity, 2) + ",";
  json += "\"pressure\":" + String(m.pressure, 2) + ",";

  json += "\"ph\":" + String(m.ph, 2) + ",";
  json += "\"orp\":" + String(m.orp, 2) + ",";
  json += "\"ec\":" + String(m.ec, 2) + ",";
  json += "\"dissolvedOxygen\":" + String(m.dissolvedOxygen, 2) + ",";
  json += "\"liquidTemperature\":" + String(m.liquidTemperature, 2) + ",";
  json += "\"ammonium\":" + String(m.ammonium, 2) + ",";
  json += "\"nitrate\":" + String(m.nitrate, 2) + ",";
  json += "\"vfa\":" + String(m.vfa, 2) + ",";

  json += "\"nirOrganicIndex\":" + String(m.nirOrganicIndex, 2) + ",";
  json += "\"biomassQualityIndex\":" + String(m.biomassQualityIndex, 2) + ",";
  json += "\"spectralScore\":" + String(m.spectralScore, 2) + ",";
  json += "\"opticalDensity\":" + String(m.opticalDensity, 2) + ",";
  json += "\"colorIndex\":" + String(m.colorIndex, 2) + ",";
  json += "\"turbidityIndex\":" + String(m.turbidityIndex, 2) + ",";

  json += "\"maturityIndex\":" + String(m.maturityIndex, 2) + ",";
  json += "\"fertilizerSuitabilityIndex\":" + String(m.fertilizerSuitabilityIndex, 2) + ",";
  json += "\"sampleQualityScore\":" + String(m.sampleQualityScore, 2) + ",";
  json += "\"toxicityRisk\":" + String(m.toxicityRisk, 2) + ",";
  json += "\"imbalanceRisk\":" + String(m.imbalanceRisk, 2) + ",";
  json += "\"qualityBand\":\"" + qualityBand + "\",";
  json += "\"recommendation\":\"" + recommendation + "\",";

  json += "\"uptimeSec\":" + String(millis() / 1000UL) + ",";
  json += "\"timestamp\":\"" + String(m.timestampMs) + "\"";

  json += "}";
  return json;
}

String buildHistoryJson(const Measurement& m) {
  String json = "{";
  json += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  json += "\"timestamp\":\"" + String(m.timestampMs) + "\",";
  json += "\"ch4\":" + String(m.ch4, 2) + ",";
  json += "\"co2\":" + String(m.co2, 2) + ",";
  json += "\"h2sPpm\":" + String(m.h2sPpm, 2) + ",";
  json += "\"ph\":" + String(m.ph, 2) + ",";
  json += "\"ec\":" + String(m.ec, 2) + ",";
  json += "\"dissolvedOxygen\":" + String(m.dissolvedOxygen, 2) + ",";
  json += "\"biomassQualityIndex\":" + String(m.biomassQualityIndex, 2) + ",";
  json += "\"sampleQualityScore\":" + String(m.sampleQualityScore, 2) + ",";
  json += "\"toxicityRisk\":" + String(m.toxicityRisk, 2) + ",";
  json += "\"qualityBand\":\"" + jsonEscape(m.qualityBand) + "\"";
  json += "}";
  return json;
}

String buildHeartbeatJson() {
  String json = "{";
  json += "\"deviceId\":\"" + String(DEVICE_ID) + "\",";
  json += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"uptimeSec\":" + String(millis() / 1000UL) + ",";
  json += "\"wifi\":";
  json += (WiFi.status() == WL_CONNECTED) ? "true," : "false,";
  json += "\"mode\":\"" + systemMode + "\"";
  json += "}";
  return json;
}

// =====================================================
// FIREBASE PUSH
// =====================================================
void pushEventToFirebase(const String& level, const String& name, const String& details) {
  String json = "{";
  json += "\"level\":\"" + jsonEscape(level) + "\",";
  json += "\"eventName\":\"" + jsonEscape(name) + "\",";
  json += "\"details\":\"" + jsonEscape(details) + "\",";
  json += "\"source\":\"ESP32\",";
  json += "\"timestamp\":\"" + String(millis()) + "\"";
  json += "}";
  firebasePost("/devices/" + String(DEVICE_ID) + "/events", json);
}

void pushSettingsToFirebase() {
  firebasePut("/devices/" + String(DEVICE_ID) + "/settings/current", buildSettingsJson());
}

void pushControlToFirebase() {
  firebasePut("/devices/" + String(DEVICE_ID) + "/control/current", buildControlJson());
}

void pushCurrentToFirebase(const Measurement& m) {
  firebaseOk = firebasePut("/devices/" + String(DEVICE_ID) + "/current", buildCurrentJson(m));
}

void pushHistoryToFirebase(const Measurement& m) {
  firebasePost("/devices/" + String(DEVICE_ID) + "/history", buildHistoryJson(m));
}

void pushHeartbeatToFirebase() {
  firebasePut("/devices/" + String(DEVICE_ID) + "/heartbeat", buildHeartbeatJson());
}

// =====================================================
// FIREBASE READ
// =====================================================
void pollSettingsFromFirebase() {
  String json = firebaseGet("/devices/" + String(DEVICE_ID) + "/settings/current");
  if (json.length() == 0 || json == "null") return;

  cfg.gasTempMin   = extractJsonNumber(json, "gasTempMin", cfg.gasTempMin);
  cfg.gasTempMax   = extractJsonNumber(json, "gasTempMax", cfg.gasTempMax);
  cfg.ch4Low       = extractJsonNumber(json, "ch4Low", cfg.ch4Low);
  cfg.h2sHigh      = extractJsonNumber(json, "h2sHigh", cfg.h2sHigh);
  cfg.o2High       = extractJsonNumber(json, "o2High", cfg.o2High);
  cfg.phMin        = extractJsonNumber(json, "phMin", cfg.phMin);
  cfg.phMax        = extractJsonNumber(json, "phMax", cfg.phMax);
  cfg.ecMax        = extractJsonNumber(json, "ecMax", cfg.ecMax);
  cfg.doMin        = extractJsonNumber(json, "doMin", cfg.doMin);
  cfg.ammoniumHigh = extractJsonNumber(json, "ammoniumHigh", cfg.ammoniumHigh);
  cfg.nitrateHigh  = extractJsonNumber(json, "nitrateHigh", cfg.nitrateHigh);
  cfg.vfaHigh      = extractJsonNumber(json, "vfaHigh", cfg.vfaHigh);
}

void pollControlFromFirebase() {
  String json = firebaseGet("/devices/" + String(DEVICE_ID) + "/control/current");
  if (json.length() == 0 || json == "null") return;

  String oldMode = systemMode;
  bool oldHeater = heaterState;
  bool oldVent   = ventState;
  bool oldPump   = pumpState;
  bool oldMixer  = mixerState;
  bool oldValve  = valveState;

  String requestedMode = extractJsonString(json, "mode", systemMode);
  bool requestedHeater = extractJsonBool(json, "heater", heaterState);
  bool requestedVent   = extractJsonBool(json, "ventilation", ventState);
  bool requestedPump   = extractJsonBool(json, "pump", pumpState);
  bool requestedMixer  = extractJsonBool(json, "mixer", mixerState);
  bool requestedValve  = extractJsonBool(json, "valve", valveState);

  bool manualOutputCommand =
      (requestedHeater != heaterState) ||
      (requestedVent   != ventState)   ||
      (requestedPump   != pumpState)   ||
      (requestedMixer  != mixerState)  ||
      (requestedValve  != valveState);

  // ВАЖНО:
  // если пользователь нажал любую ручную кнопку, принудительно уходим в MANUAL,
  // чтобы команда точно выполнилась
  if (requestedMode == "MANUAL" || manualOutputCommand) {
    systemMode  = "MANUAL";
    heaterState = requestedHeater;
    ventState   = requestedVent;
    pumpState   = requestedPump;
    mixerState  = requestedMixer;
    valveState  = requestedValve;
    applyOutputs();
  } else if (requestedMode == "AUTO") {
    systemMode = "AUTO";
    applyAutoControl(latest);
  }

  bool changed =
      (oldMode   != systemMode) ||
      (oldHeater != heaterState) ||
      (oldVent   != ventState) ||
      (oldPump   != pumpState) ||
      (oldMixer  != mixerState) ||
      (oldValve  != valveState);

  if (changed) {
    pushCurrentToFirebase(latest);
    pushControlToFirebase();

    String details =
        "mode=" + systemMode +
        ", heater=" + String(heaterState ? "true" : "false") +
        ", vent=" + String(ventState ? "true" : "false") +
        ", pump=" + String(pumpState ? "true" : "false") +
        ", mixer=" + String(mixerState ? "true" : "false") +
        ", valve=" + String(valveState ? "true" : "false");

    pushEventToFirebase("info", "Control applied", details);

    Serial.println("CONTROL APPLIED");
    Serial.println(details);
  }
}

// =====================================================
// WIFI
// =====================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
}

// =====================================================
// SETUP / LOOP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_HEATER, OUTPUT);
  pinMode(PIN_VENT, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_MIXER, OUTPUT);
  pinMode(PIN_VALVE, OUTPUT);

  heaterState = false;
  ventState   = false;
  pumpState   = false;
  mixerState  = false;
  valveState  = false;
  systemMode  = "AUTO";
  applyOutputs();

  randomSeed(micros());

  connectWiFi();

  acquireAllMeasurements();
  applyAutoControl(latest);

  if (WiFi.status() == WL_CONNECTED) {
    pushSettingsToFirebase();
    pushControlToFirebase();
    pushCurrentToFirebase(latest);
    pushHistoryToFirebase(latest);
    pushHeartbeatToFirebase();
    pushEventToFirebase("info", "System start", "ESP32 lab-on-chip node started");
  }

  lastWiFiRetry     = millis();
  lastReadSensors   = millis();
  lastPushCurrent   = millis();
  lastPushHistory   = millis();
  lastPushHeartbeat = millis();
  lastPollControl   = millis();
  lastPollSettings  = millis();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED && now - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetry = now;
    connectWiFi();
  }

  if (now - lastReadSensors >= READ_INTERVAL) {
    lastReadSensors = now;
    acquireAllMeasurements();

    if (systemMode == "AUTO") {
      applyAutoControl(latest);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastPollControl >= POLL_CONTROL_INTERVAL) {
      lastPollControl = now;
      pollControlFromFirebase();
    }

    if (now - lastPollSettings >= POLL_SETTINGS_INTERVAL) {
      lastPollSettings = now;
      pollSettingsFromFirebase();
    }

    if (now - lastPushCurrent >= PUSH_CURRENT_INTERVAL) {
      lastPushCurrent = now;
      pushCurrentToFirebase(latest);
    }

    if (now - lastPushHistory >= PUSH_HISTORY_INTERVAL) {
      lastPushHistory = now;
      pushHistoryToFirebase(latest);
    }

    if (now - lastPushHeartbeat >= PUSH_HEARTBEAT_INTERVAL) {
      lastPushHeartbeat = now;
      pushHeartbeatToFirebase();
    }
  }
}
