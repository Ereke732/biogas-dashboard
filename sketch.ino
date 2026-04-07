
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <math.h>

// =========================
// WIFI + FIREBASE
// =========================
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* FIREBASE_DB_URL = "https://erekearduino-default-rtdb.firebaseio.com";
const char* FIREBASE_AUTH = "";
const char* DEVICE_ID = "esp32-biogas-01";

// =========================
// TFT
// =========================
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// =========================
// Optional outputs (relay pins)
// If your current diagram doesn't have relays, the code still works.
// =========================
#define PIN_HEATER 26
#define PIN_VENT   27
#define PIN_PUMP   25

bool heaterState = false;
bool ventState   = false;
bool pumpState   = false;

// =========================
// Event latches
// =========================
bool prevHeaterState = false;
bool prevVentState   = false;
bool prevPumpState   = false;

bool h2sAlarmSent = false;
bool o2AlarmSent  = false;
bool phAlarmSent  = false;
bool ch4AlarmSent = false;

// =========================
// Tabs
// =========================
enum Tab {
  TAB_CH4 = 0,
  TAB_H2S,
  TAB_CO2,
  TAB_PH,
  TAB_O2,
  TAB_COUNT
};

Tab currentTab = TAB_CH4;

// Make this 800..1500 for readable switching
const unsigned long TAB_INTERVAL = 800;
unsigned long lastTabSwitch = 0;

// =========================
// Plotter
// =========================
int currentIndex = 0;
unsigned long lastPlotUpdate = 0;
const unsigned long PLOT_INTERVAL = 250;

// =========================
// Firebase timers
// =========================
unsigned long lastFirebaseCurrent = 0;
unsigned long lastFirebaseHistory = 0;
unsigned long lastControlPoll     = 0;
unsigned long lastWiFiRetry       = 0;

const unsigned long FIREBASE_CURRENT_INTERVAL = 700;
const unsigned long FIREBASE_HISTORY_INTERVAL = 700;
const unsigned long FIREBASE_CONTROL_INTERVAL = 1000;

const unsigned long WIFI_RETRY_INTERVAL       = 2000;

const int NUM_POINTS = 31;

// Example series for graph tabs
float ch4Data[NUM_POINTS] = {
  0.9, 1.1, 1.2, 1.2, 1.8, 2.4, 2.9, 3.0, 3.8, 3.9,
  4.0, 3.7, 3.9, 4.0, 3.1, 3.0, 3.1, 3.0, 3.0, 3.0,
  2.4, 2.4, 2.5, 2.4, 2.4, 0.9, 0.3, 0.3, 0.3, 0.2, 0.1
};

float h2sData[NUM_POINTS] = {
  0.23, 0.30, 0.27, 0.24, 0.28, 0.26, 0.19, 0.31, 0.24, 0.32,
  0.25, 0.23, 0.27, 0.29, 0.18, 0.26, 0.30, 0.24, 0.22, 0.27,
  0.29, 0.23, 0.31, 0.25, 0.20, 0.28, 0.31, 0.24, 0.27, 0.19, 0.41
};

float co2Data[NUM_POINTS] = {
  5.8, 6.1, 5.7, 4.3, 4.4, 4.3, 2.9, 3.0, 3.0, 3.0,
  3.0, 4.2, 4.3, 4.3, 4.2, 4.2, 6.2, 6.4, 6.2, 6.3,
  6.2, 6.3, 6.4, 6.3, 6.6, 2.2, 1.3, 1.3, 1.3, 1.3, 1.2
};

float phData[NUM_POINTS] = {
  7.40, 7.68, 7.56, 7.32, 7.48, 7.72, 7.33, 7.60, 7.41, 7.54,
  7.61, 7.56, 7.26, 7.49, 7.58, 7.19, 7.53, 7.77, 7.42, 7.27,
  7.51, 7.64, 7.29, 7.80, 7.50, 7.20, 7.76, 7.33, 7.48, 7.65, 6.83
};

float o2Data[NUM_POINTS] = {
  0.82, 1.11, 0.97, 0.95, 0.86, 1.27, 1.05, 1.14, 1.15, 0.89,
  1.24, 1.13, 1.14, 1.04, 0.89, 0.93, 1.06, 0.89, 1.09, 1.11,
  1.08, 1.13, 0.98, 1.02, 1.13, 1.21, 0.93, 1.05, 0.91, 1.17, 0.59
};

// =========================
// Current packet
// =========================
struct SensorPacket {
  float temperature;
  float CH4;
  float CO2;
  float O2;
  float H2S;
  float pH;
  float level;
};

SensorPacket latest;

// =========================
// UI drawing
// =========================
void drawHeader(const char* title, uint16_t color) {
  tft.fillRect(0, 0, 320, 28, color);
  tft.setTextColor(ILI9341_WHITE, color);
  tft.setTextSize(2);
  tft.setCursor(8, 7);
  tft.print(title);
}

void drawTabs(Tab active) {
  const char* names[TAB_COUNT] = {"CH4", "H2S", "CO2", "pH", "O2"};
  int x = 0;
  for (int i = 0; i < TAB_COUNT; i++) {
    uint16_t bg = (i == active) ? ILI9341_YELLOW : ILI9341_DARKCYAN;
    uint16_t fg = (i == active) ? ILI9341_BLACK : ILI9341_WHITE;
    tft.fillRect(x, 30, 64, 18, bg);
    tft.drawRect(x, 30, 64, 18, ILI9341_WHITE);
    tft.setTextColor(fg, bg);
    tft.setTextSize(1);
    tft.setCursor(x + 18, 35);
    tft.print(names[i]);
    x += 64;
  }
}

void drawAxes(int x0, int y0, int w, int h, float yMin, float yMax, const char* unit) {
  tft.drawRect(x0, y0, w, h, ILI9341_WHITE);

  for (int i = 1; i < 4; i++) {
    int x = x0 + (w * i) / 4;
    tft.drawFastVLine(x, y0, h, ILI9341_DARKGREY);
  }

  tft.drawFastHLine(x0, y0 + h / 2, w, ILI9341_DARKGREY);

  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setTextSize(1);

  tft.setCursor(2, y0 - 2);
  tft.print(yMax, 2);

  tft.setCursor(2, y0 + h - 4);
  tft.print(yMin, 2);

  tft.setCursor(x0 + w + 4, y0 + 2);
  tft.print(unit);

  tft.setCursor(x0, y0 + h + 4); tft.print("AUG");
  tft.setCursor(x0 + w / 4 - 6, y0 + h + 4); tft.print("SEP");
  tft.setCursor(x0 + w / 2 - 6, y0 + h + 4); tft.print("OCT");
  tft.setCursor(x0 + 3 * w / 4 - 6, y0 + h + 4); tft.print("NOV");
  tft.setCursor(x0 + w - 20, y0 + h + 4); tft.print("DEC");
}

void drawChart(const char* title, float* data, float yMin, float yMax, uint16_t color, const char* unit) {
  tft.fillScreen(ILI9341_BLACK);
  drawHeader(title, ILI9341_BLUE);
  drawTabs(currentTab);

  const int x0 = 20;
  const int y0 = 60;
  const int w = 270;
  const int h = 130;

  drawAxes(x0, y0, w, h, yMin, yMax, unit);

  int prevX = x0;
  float n0 = (data[0] - yMin) / (yMax - yMin);
  if (n0 < 0) n0 = 0;
  if (n0 > 1) n0 = 1;
  int prevY = y0 + h - (int)(n0 * h);

  for (int i = 1; i < NUM_POINTS; i++) {
    float n = (data[i] - yMin) / (yMax - yMin);
    if (n < 0) n = 0;
    if (n > 1) n = 1;

    int x = x0 + (i * (w - 1)) / (NUM_POINTS - 1);
    int y = y0 + h - (int)(n * h);

    tft.drawLine(prevX, prevY, x, y, color);
    tft.fillCircle(x, y, 1, color);

    prevX = x;
    prevY = y;
  }

  tft.fillRect(0, 222, 320, 18, ILI9341_NAVY);
  tft.setTextColor(ILI9341_WHITE, ILI9341_NAVY);
  tft.setTextSize(1);
  tft.setCursor(6, 227);
  tft.print("Auto tabs + Firebase + Events");
}

void drawCurrentTab() {
  switch (currentTab) {
    case TAB_CH4:
      drawChart("Methane (CH4)", ch4Data, 0.0, 4.2, ILI9341_GREEN, "%");
      break;
    case TAB_H2S:
      drawChart("Hydrogen Sulfide (H2S)", h2sData, 0.17, 0.42, ILI9341_RED, "%");
      break;
    case TAB_CO2:
      drawChart("Carbon Dioxide (CO2)", co2Data, 1.0, 7.0, ILI9341_ORANGE, "%");
      break;
    case TAB_PH:
      drawChart("pH", phData, 6.8, 7.9, ILI9341_CYAN, "pH");
      break;
    case TAB_O2:
      drawChart("Oxygen (O2)", o2Data, 0.55, 1.40, ILI9341_MAGENTA, "%");
      break;
    default:
      break;
  }
}

// =========================
// Firebase helpers
// =========================
String firebaseUrl(const String& path) {
  String url = String(FIREBASE_DB_URL) + path + ".json";
  if (strlen(FIREBASE_AUTH) > 0) {
    url += "?auth=" + String(FIREBASE_AUTH);
  }
  return url;
}

bool firebasePut(const String& path, const String& json) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, firebaseUrl(path))) return false;
  https.addHeader("Content-Type", "application/json");

  int code = https.PUT(json);
  https.getString();
  https.end();
  return code >= 200 && code < 300;
}

bool firebasePost(const String& path, const String& json) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, firebaseUrl(path))) return false;
  https.addHeader("Content-Type", "application/json");

  int code = https.POST(json);
  https.getString();
  https.end();
  return code >= 200 && code < 300;
}

String firebaseGet(const String& path) {
  if (WiFi.status() != WL_CONNECTED) return "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, firebaseUrl(path))) return "";

  int code = https.GET();
  String payload = https.getString();
  https.end();

  if (code >= 200 && code < 300) return payload;
  return "";
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
  }
}

void applyOutputs() {
  digitalWrite(PIN_HEATER, heaterState ? HIGH : LOW);
  digitalWrite(PIN_VENT,   ventState   ? HIGH : LOW);
  digitalWrite(PIN_PUMP,   pumpState   ? HIGH : LOW);
}

SensorPacket buildLatestPacket() {
  SensorPacket s;
  int i = currentIndex;

  s.CH4 = ch4Data[i];
  s.H2S = h2sData[i] * 1000.0f;  // turn 0.23 -> 230 ppm style for dashboard/event logic
  s.CO2 = co2Data[i];
  s.pH  = phData[i];
  s.O2  = o2Data[i];
  s.temperature = 45.0f + 10.0f * (0.5f + 0.5f * sinf((float)i * 0.35f));
  s.level = 60.0f + 20.0f * sinf((float)i * 0.22f);

  return s;
}

String buildCurrentJson(const SensorPacket& s) {
  String json = "{";
  json += "\"timestamp\":\"" + String(millis()) + "\",";
  json += "\"temperature\":" + String(s.temperature, 2) + ",";
  json += "\"CH4\":" + String(s.CH4, 2) + ",";
  json += "\"CO2\":" + String(s.CO2, 2) + ",";
  json += "\"O2\":" + String(s.O2, 2) + ",";
  json += "\"H2S\":" + String(s.H2S, 2) + ",";
  json += "\"pH\":" + String(s.pH, 2) + ",";
  json += "\"level\":" + String(s.level, 2) + ",";
  json += "\"heater\":" + String(heaterState ? "true" : "false") + ",";
  json += "\"ventilation\":" + String(ventState ? "true" : "false") + ",";
  json += "\"pump\":" + String(pumpState ? "true" : "false");
  json += "}";
  return json;
}

String buildHistoryJson(const SensorPacket& s) {
  String json = "{";
  json += "\"timestamp\":\"" + String(millis()) + "\",";
  json += "\"temperature\":" + String(s.temperature, 2) + ",";
  json += "\"CH4\":" + String(s.CH4, 2) + ",";
  json += "\"CO2\":" + String(s.CO2, 2) + ",";
  json += "\"O2\":" + String(s.O2, 2) + ",";
  json += "\"H2S\":" + String(s.H2S, 2) + ",";
  json += "\"pH\":" + String(s.pH, 2) + ",";
  json += "\"level\":" + String(s.level, 2);
  json += "}";
  return json;
}

void pushCurrentToFirebase(const SensorPacket& s) {
  String path = "/devices/" + String(DEVICE_ID) + "/current";
  firebasePut(path, buildCurrentJson(s));
}

void pushHistoryToFirebase(const SensorPacket& s) {
  String path = "/devices/" + String(DEVICE_ID) + "/history";
  firebasePost(path, buildHistoryJson(s));
}

void pushEventToFirebase(const String& level, const String& eventName, const String& details) {
  String path = "/devices/" + String(DEVICE_ID) + "/events";

  String json = "{";
  json += "\"timestamp\":\"" + String(millis()) + "\",";
  json += "\"level\":\"" + level + "\",";
  json += "\"event\":\"" + eventName + "\",";
  json += "\"details\":\"" + details + "\"";
  json += "}";

  firebasePost(path, json);
}

void pollControlFromFirebase() {
  String path = "/devices/" + String(DEVICE_ID) + "/control/current";
  String json = firebaseGet(path);

  if (json.length() == 0 || json == "null") return;

  bool oldHeater = heaterState;
  bool oldVent   = ventState;
  bool oldPump   = pumpState;

  if (json.indexOf("\"heater\":true") >= 0) heaterState = true;
  if (json.indexOf("\"heater\":false") >= 0) heaterState = false;

  if (json.indexOf("\"ventilation\":true") >= 0 || json.indexOf("\"vent\":true") >= 0) ventState = true;
  if (json.indexOf("\"ventilation\":false") >= 0 || json.indexOf("\"vent\":false") >= 0) ventState = false;

  if (json.indexOf("\"pump\":true") >= 0) pumpState = true;
  if (json.indexOf("\"pump\":false") >= 0) pumpState = false;

  applyOutputs();

  if (heaterState != oldHeater) {
    pushEventToFirebase("info", heaterState ? "Heater ON" : "Heater OFF",
                        heaterState ? "Heating enabled from web control" : "Heating disabled from web control");
  }
  if (ventState != oldVent) {
    pushEventToFirebase("info", ventState ? "Ventilation ON" : "Ventilation OFF",
                        ventState ? "Ventilation enabled from web control" : "Ventilation disabled from web control");
  }
  if (pumpState != oldPump) {
    pushEventToFirebase("info", pumpState ? "Pump ON" : "Pump OFF",
                        pumpState ? "Pump enabled from web control" : "Pump disabled from web control");
  }
}

// =========================
// Serial + plotter
// =========================
void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "ch4") currentTab = TAB_CH4;
  else if (cmd == "h2s") currentTab = TAB_H2S;
  else if (cmd == "co2") currentTab = TAB_CO2;
  else if (cmd == "ph") currentTab = TAB_PH;
  else if (cmd == "o2") currentTab = TAB_O2;
  else if (cmd == "next") currentTab = (Tab)((currentTab + 1) % TAB_COUNT);

  drawCurrentTab();
}

void printPlotter(const SensorPacket& s) {
  Serial.print("CH4:");
  Serial.print(s.CH4, 2);
  Serial.print("\t");

  Serial.print("H2S:");
  Serial.print(s.H2S, 2);
  Serial.print("\t");

  Serial.print("CO2:");
  Serial.print(s.CO2, 2);
  Serial.print("\t");

  Serial.print("pH:");
  Serial.print(s.pH, 2);
  Serial.print("\t");

  Serial.print("O2:");
  Serial.println(s.O2, 2);
}

void processEvents(const SensorPacket& s) {
  if (s.H2S > 300.0f && !h2sAlarmSent) {
    pushEventToFirebase("danger", "H2S high", "Hydrogen sulfide exceeded 300 ppm");
    h2sAlarmSent = true;
  }
  if (s.H2S <= 300.0f) h2sAlarmSent = false;

  if (s.O2 > 1.20f && !o2AlarmSent) {
    pushEventToFirebase("warn", "O2 high", "Oxygen exceeded safe anaerobic threshold");
    o2AlarmSent = true;
  }
  if (s.O2 <= 1.20f) o2AlarmSent = false;

  if ((s.pH < 6.8f || s.pH > 7.9f) && !phAlarmSent) {
    pushEventToFirebase("warn", "pH out of range", "pH left the normal operating band");
    phAlarmSent = true;
  }
  if (s.pH >= 6.8f && s.pH <= 7.9f) phAlarmSent = false;

  if (s.CH4 < 1.0f && !ch4AlarmSent) {
    pushEventToFirebase("warn", "CH4 low", "Methane concentration dropped below target");
    ch4AlarmSent = true;
  }
  if (s.CH4 >= 1.0f) ch4AlarmSent = false;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_HEATER, OUTPUT);
  pinMode(PIN_VENT, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  applyOutputs();

  connectWiFi();

  tft.begin();
  tft.setRotation(1);

  latest = buildLatestPacket();
  drawCurrentTab();

  pushEventToFirebase("info", "System start", "ESP32 graph/Firebase dashboard started");
}

void loop() {

if (millis() - lastFirebaseCurrent >= FIREBASE_CURRENT_INTERVAL) {
  lastFirebaseCurrent = millis();
  pushCurrentToFirebase(latest);
}

if (millis() - lastFirebaseHistory >= FIREBASE_HISTORY_INTERVAL) {
  lastFirebaseHistory = millis();
  pushHistoryToFirebase(latest);
}

  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetry = millis();
    connectWiFi();
  }

  handleSerial();

  if (millis() - lastTabSwitch >= TAB_INTERVAL) {
    lastTabSwitch = millis();
    currentTab = (Tab)((currentTab + 1) % TAB_COUNT);
    drawCurrentTab();
  }

  if (millis() - lastPlotUpdate >= PLOT_INTERVAL) {
    lastPlotUpdate = millis();

    latest = buildLatestPacket();
    printPlotter(latest);
    processEvents(latest);

    currentIndex++;
    if (currentIndex >= NUM_POINTS) currentIndex = 0;
  }

  if (millis() - lastControlPoll >= FIREBASE_CONTROL_INTERVAL) {
    lastControlPoll = millis();
    pollControlFromFirebase();
  }

  if (millis() - lastFirebaseCurrent >= FIREBASE_CURRENT_INTERVAL) {
    lastFirebaseCurrent = millis();
    pushCurrentToFirebase(latest);
  }

  if (millis() - lastFirebaseHistory >= FIREBASE_HISTORY_INTERVAL) {
    lastFirebaseHistory = millis();
    pushHistoryToFirebase(latest);
  }

  delay(20);
}
