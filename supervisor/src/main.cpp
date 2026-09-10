
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <cmath>

#include "config.h"

// ============================================================================
// TERRA CYD SUPERVISOR — 320x240 LVGL 9 + TFT_eSPI + XPT2046
// Target: ESP32-2432S028R (Cheap Yellow Display 2.8" ILI9341 + XPT2046)
// ============================================================================

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(supcfg::PIN_TOUCH_CS, supcfg::PIN_TOUCH_IRQ);

WiFiClient net;
PubSubClient mqtt(net);

bool shtOk = false;
uint32_t bootMs = 0;
uint32_t lastSensorRead = 0;
uint32_t lastUiData = 0;
uint32_t lastMainHeartbeatRx = 0;
uint32_t lastTelemetryRx = 0;
uint32_t lastAlarmPublish = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastMqttAttempt = 0;
uint32_t bootCounter = 0;

bool mainOnline = false;
String alarmCode = "NONE";
String alarmDetail = "System Nominal";
String mainState = "UNKNOWN";
String mainAlarm = "NONE";
uint32_t mainAlarmSeq = 0;
bool alarmSilenced = false;
uint32_t alarmSilencedUntil = 0;

float ownTemp = NAN;
float ownHum = NAN;
float mainExternal = NAN;
float mainExternalHum = NAN;
bool mainExternalOk = false;
float mainUpper = NAN;
float mainLower = NAN;
float mainUpperHum = NAN;
float mainLowerHum = NAN;
float mainSoil = NAN;

// Main Controller Extended Telemetry
bool mainWaterLow = false;
bool mainDrainageHigh = false;
bool mainDoorOpen = true;
bool mainMisterPump = false;
bool mainFogger = false;
bool mainHeater = false;
bool mainDrainagePump = false;
uint8_t mainFanPwm = 0;

// ---------- Buzzer ----------
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void buzzerSetup() { ledcAttach(supcfg::PIN_BUZZER, 2200, 8); }
void buzzerTone(uint32_t hz) { ledcWriteTone(supcfg::PIN_BUZZER, hz); }
#else
constexpr uint8_t BUZZER_CHANNEL = 0;
void buzzerSetup() {
  ledcSetup(BUZZER_CHANNEL, 2200, 8);
  ledcAttachPin(supcfg::PIN_BUZZER, BUZZER_CHANNEL);
}
void buzzerTone(uint32_t hz) { ledcWriteTone(BUZZER_CHANNEL, hz); }
#endif
void buzzerOff() { buzzerTone(0); }

// ---------- Basic Helpers ----------
bool credentialsConfigured() {
  return strcmp(supcfg::WIFI_SSID, "CHANGE_ME") != 0 &&
         strcmp(supcfg::WIFI_PASSWORD, "CHANGE_ME") != 0 &&
         strcmp(supcfg::MQTT_PASSWORD, "CHANGE_ME") != 0;
}

bool alarmActive() {
  return alarmCode != "NONE" || (mainAlarm != "NONE" && mainAlarm != "UNKNOWN");
}

String fmt(float value, const char* suffix = "", uint8_t decimals = 1) {
  if (isnan(value)) return "--";
  char b[24];
  dtostrf(value, 0, decimals, b);
  String s(b);
  s.trim();
  s += suffix;
  return s;
}

String age(uint32_t timestamp) {
  if (!timestamp) return "--";
  const uint32_t seconds = (millis() - timestamp) / 1000U;
  if (seconds < 60U) return String(seconds) + "s";
  return String(seconds / 60U) + "m";
}

String uptimeStr() {
  const uint32_t s = millis() / 1000U;
  const uint32_t m = (s / 60U) % 60U;
  const uint32_t h = (s / 3600U) % 24U;
  const uint32_t d = s / 86400U;
  char buf[32];
  if (d > 0) {
    snprintf(buf, sizeof(buf), "%lud %02luh %02lum", d, h, m);
  } else {
    snprintf(buf, sizeof(buf), "%02luh %02lum %02lus", h, m, s % 60U);
  }
  return String(buf);
}

float mainAverage() {
  if (isnan(mainUpper) || isnan(mainLower)) return NAN;
  return (mainUpper + mainLower) * 0.5f;
}

float tempDelta() {
  const float avg = mainAverage();
  if (isnan(mainExternal) || isnan(avg)) return NAN;
  return mainExternal - avg;
}

// ---------- Alarm / Supervision ----------
void publishAlarm(const char* code, const char* detail) {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["source"] = supcfg::DEVICE_ID;
  doc["severity"] = "CRITICAL";
  doc["code"] = code;
  doc["detail"] = detail;
  doc["uptimeMs"] = millis();
  doc["bootId"] = bootCounter;

  String payload;
  serializeJson(doc, payload);
  mqtt.publish("terrarium/supervisor/alarm", payload.c_str(), false);
  lastAlarmPublish = millis();
}

void setAlarm(const char* code, const char* detail) {
  const bool changed = alarmCode != code;
  alarmCode = code;
  alarmDetail = detail;

  if (changed) {
    alarmSilenced = false;
    alarmSilencedUntil = 0;
  }

  if (!supcfg::DISPLAY_TEST_MODE && mqtt.connected() &&
      (changed || lastAlarmPublish == 0 ||
       millis() - lastAlarmPublish >= supcfg::ALARM_REPEAT_MS)) {
    publishAlarm(code, detail);
  }
}

void clearAlarm() {
  alarmCode = "NONE";
  alarmDetail = "System Nominal";
  alarmSilenced = false;
  alarmSilencedUntil = 0;
  buzzerOff();
}

void silenceAlarm(uint32_t durationMs = 900000) {
  alarmSilenced = true;
  alarmSilencedUntil = millis() + durationMs;
  buzzerOff();
}

void supervisionService() {
  if (supcfg::DISPLAY_TEST_MODE) {
    clearAlarm();
    return;
  }

  const uint32_t now = millis();

  if (!lastMainHeartbeatRx ||
      now - lastMainHeartbeatRx > supcfg::HEARTBEAT_TIMEOUT_MS) {
    mainOnline = false;
    setAlarm("MAIN_OFFLINE", "Main controller heartbeat lost");
    return;
  }

  mainOnline = true;

  if (!lastTelemetryRx ||
      now - lastTelemetryRx > supcfg::STALE_TELEMETRY_MS) {
    setAlarm("TELEMETRY_STALE", "Main controller telemetry is stale");
    return;
  }

  if (!isnan(mainUpper) && !isnan(mainLower) &&
      fabsf(mainUpper - mainLower) > supcfg::MAX_MAIN_GRADIENT_C) {
    setAlarm("MAIN_GRADIENT", "Main temperature gradient is implausible");
    return;
  }

  if (mainWaterLow) {
    setAlarm("RESERVOIR_LOW", "Water reservoir level low");
    return;
  }

  if (mainAlarm != "NONE" && mainAlarm != "UNKNOWN") {
    String detail = "Main: " + mainAlarm;
    setAlarm("MAIN_ALARM", detail.c_str());
    return;
  }

  clearAlarm();
}

void buzzerService() {
  const bool muted = alarmSilenced && millis() < alarmSilencedUntil;

  if (millis() - bootMs < supcfg::SENSOR_WARMUP_MS ||
      supcfg::DISPLAY_TEST_MODE ||
      !alarmActive() ||
      muted) {
    buzzerOff();
    return;
  }

  buzzerTone((millis() % 3000U) < 250U ? 2200 : 0);
}

// ---------- MQTT ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (len == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
    return;
  }

  const String topicName = topic;

  if (topicName == "terrarium/main/heartbeat") {
    lastMainHeartbeatRx = millis();
    mainOnline = true;
    mainState = String((const char*)(doc["state"] | "UNKNOWN"));
    mainAlarm = String((const char*)(doc["alarm"] | "NONE"));
    mainAlarmSeq = doc["alarmSequence"] | 0U;
    return;
  }

  if (topicName == "terrarium/main/telemetry") {
    lastTelemetryRx = millis();

    mainExternal = doc["externalTemperatureC"].is<float>()
            ? doc["externalTemperatureC"].as<float>()
            : NAN;
    mainExternalHum = doc["externalHumidityPct"].is<float>()
               ? doc["externalHumidityPct"].as<float>()
               : NAN;
    mainExternalOk = doc["externalSensorOk"] | false;
    mainUpper = doc["upperTemperatureC"].is<float>()
                    ? doc["upperTemperatureC"].as<float>()
                    : NAN;
    mainLower = doc["lowerTemperatureC"].is<float>()
                    ? doc["lowerTemperatureC"].as<float>()
                    : NAN;
    mainUpperHum = doc["upperHumidityPct"].is<float>()
                       ? doc["upperHumidityPct"].as<float>()
                       : NAN;
    mainLowerHum = doc["lowerHumidityPct"].is<float>()
                       ? doc["lowerHumidityPct"].as<float>()
                       : NAN;
    mainSoil = doc["soilMoisturePct"].is<float>()
           ? doc["soilMoisturePct"].as<float>()
                 : NAN;

    mainWaterLow = doc["reservoirLow"] | false;
    mainDrainageHigh = doc["drainageHigh"] | false;
    mainDoorOpen = doc["doorOpen"] | true;
    mainMisterPump = doc["misterPump"] | false;
    mainFogger = doc["fogger"] | false;
    mainHeater = doc["heater"] | false;
    mainDrainagePump = doc["drainagePump"] | false;
    mainFanPwm = doc["fanPwm"] | 0;

    mainState = String((const char*)(doc["state"] | "UNKNOWN"));
    mainAlarm = String((const char*)(doc["alarm"] | "NONE"));
    mainAlarmSeq = doc["alarmSequence"] | 0U;
  }
}

void wifiService() {
  if (supcfg::DISPLAY_TEST_MODE || !credentialsConfigured() ||
      WiFi.status() == WL_CONNECTED ||
      millis() - lastWifiAttempt < supcfg::WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastWifiAttempt = millis();
  WiFi.begin(supcfg::WIFI_SSID, supcfg::WIFI_PASSWORD);
}

void mqttService() {
  if (supcfg::DISPLAY_TEST_MODE || WiFi.status() != WL_CONNECTED ||
      mqtt.connected() ||
      millis() - lastMqttAttempt < supcfg::MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastMqttAttempt = millis();
  const String clientId =
      String(supcfg::DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  if (mqtt.connect(clientId.c_str(), supcfg::MQTT_USER, supcfg::MQTT_PASSWORD)) {
    mqtt.subscribe("terrarium/main/heartbeat", 1);
    mqtt.subscribe("terrarium/main/telemetry", 1);
  }
}

// ---------- Sensors ----------
void sensorService() {
  if (millis() - bootMs < supcfg::SENSOR_WARMUP_MS) return;
  if (millis() - lastSensorRead < supcfg::SENSOR_INTERVAL_MS) return;

  lastSensorRead = millis();

  if (supcfg::DISPLAY_TEST_MODE) {
    const float wobble = 0.4f * sinf((float)millis() / 5000.0f);
    ownTemp = 23.8f;
    ownHum = 79.0f;
    mainUpper = 23.6f + wobble;
    mainLower = 22.9f + wobble * 0.7f;
    mainUpperHum = 78.0f;
    mainLowerHum = 81.0f;
    mainExternal = 23.8f;
    mainExternalHum = 79.0f;
    mainExternalOk = true;
    mainSoil = 66.0f;
    mainWaterLow = false;
    mainDrainageHigh = false;
    mainDoorOpen = false;
    mainMisterPump = false;
    mainFogger = false;
    mainHeater = false;
    mainDrainagePump = false;
    mainFanPwm = 128;
    mainState = "RUNNING";
    mainAlarm = "NONE";
    shtOk = true;
  } else {
    ownTemp = mainExternal;
    ownHum = mainExternalHum;
    shtOk = mainExternalOk;
  }
}

// ============================================================================
// LVGL 9.5 320x240 LANDSCAPE UI
// ============================================================================

namespace UI {
constexpr int W = 320;
constexpr int H = 240;

// High-Contrast Near-Black Green Color Palette for CYD LCD
constexpr uint32_t BG        = 0x020603; // Deepest background
constexpr uint32_t HEADER_BG = 0x06140A; // Dark header/footer green
constexpr uint32_t CARD_BG   = 0x040B06; // Near-black card background
constexpr uint32_t CARD_ALT  = 0x0A1C0E; // Active card / tab bg
constexpr uint32_t BORDER    = 0x102816; // Dark green border
constexpr uint32_t TEXT      = 0xF8FAFC; // Primary white
constexpr uint32_t MUTED     = 0x94A3B8; // Secondary muted text
constexpr uint32_t CYAN      = 0x06B6D4; // Independent sensor accent
constexpr uint32_t SKY       = 0x38BDF8; // Main telemetry accent
constexpr uint32_t OK        = 0x10B981; // Emerald green
constexpr uint32_t WARN      = 0xF59E0B; // Amber warning
constexpr uint32_t CRIT      = 0xEF4444; // Crimson critical

enum Page : uint8_t { OVERVIEW, CLIMATE, SAFETY, SYSTEM, PAGE_COUNT };
Page page = OVERVIEW;

lv_display_t* displayHandle = nullptr;
lv_indev_t* inputHandle = nullptr;

lv_obj_t* headerTitle = nullptr;
lv_obj_t* headerBadge = nullptr;
lv_obj_t* headerBadgeLabel = nullptr;
lv_obj_t* content = nullptr;
lv_obj_t* navBar = nullptr;
lv_obj_t* navButtons[PAGE_COUNT] = {};
lv_obj_t* pageContainers[PAGE_COUNT] = {};

struct DynamicRefs {
  // Overview
  lv_obj_t* ownTemp = nullptr;
  lv_obj_t* ownHum = nullptr;
  lv_obj_t* ownStatus = nullptr;
  lv_obj_t* mainAvg = nullptr;
  lv_obj_t* mainDelta = nullptr;
  lv_obj_t* mainState = nullptr;
  lv_obj_t* dashMister = nullptr;
  lv_obj_t* dashFan = nullptr;
  lv_obj_t* dashPump = nullptr;
  lv_obj_t* dashWater = nullptr;
  lv_obj_t* upperTemp = nullptr;
  lv_obj_t* upperHum = nullptr;
  lv_obj_t* lowerTemp = nullptr;
  lv_obj_t* lowerHum = nullptr;
  lv_obj_t* hbAge = nullptr;
  lv_obj_t* gradVal = nullptr;

  // Climate
  lv_obj_t* climOwnTemp = nullptr;
  lv_obj_t* climOwnHum = nullptr;
  lv_obj_t* climOwnStatus = nullptr;
  lv_obj_t* climDelta = nullptr;
  lv_obj_t* climUpper = nullptr;
  lv_obj_t* climLower = nullptr;
  lv_obj_t* climGrad = nullptr;
  lv_obj_t* climSoil = nullptr;
  lv_obj_t* climWater = nullptr;
  lv_obj_t* climActuators = nullptr;

  // Safety
  lv_obj_t* alarmBanner = nullptr;
  lv_obj_t* alarmBannerTitle = nullptr;
  lv_obj_t* alarmBannerDetail = nullptr;
  lv_obj_t* checkHb = nullptr;
  lv_obj_t* checkTelem = nullptr;
  lv_obj_t* checkSht = nullptr;
  lv_obj_t* checkRange = nullptr;
  lv_obj_t* checkDelta = nullptr;
  lv_obj_t* checkGrad = nullptr;
  lv_obj_t* checkWater = nullptr;
  lv_obj_t* silenceBtn = nullptr;
  lv_obj_t* silenceBtnLabel = nullptr;

  // System
  lv_obj_t* sysWifiSsid = nullptr;
  lv_obj_t* sysWifiIp = nullptr;
  lv_obj_t* sysMqttStatus = nullptr;
  lv_obj_t* sysMqttBroker = nullptr;
  lv_obj_t* sysHeap = nullptr;
  lv_obj_t* sysUptime = nullptr;
  lv_obj_t* sysMode = nullptr;
} refs;

inline lv_color_t c(uint32_t val) {
  return lv_color_hex(val);
}

void noScroll(lv_obj_t* obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t* card(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg = CARD_BG, uint32_t border = BORDER) {
  lv_obj_t* obj = lv_obj_create(parent);
  noScroll(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_color(obj, c(bg), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, c(border), 0);
  lv_obj_set_style_radius(obj, 6, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  return obj;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int x, int y) {
  lv_obj_t* obj = lv_label_create(parent);
  lv_label_set_text(obj, text);
  lv_obj_set_style_text_font(obj, font, 0);
  lv_obj_set_style_text_color(obj, c(color), 0);
  lv_obj_set_pos(obj, x, y);
  return obj;
}

const char* pageName(Page p) {
  switch (p) {
    case OVERVIEW: return "DASHBOARD";
    case CLIMATE:  return "CLIMATE ARRAY";
    case SAFETY:   return "SAFETY & ALERTS";
    default:       return "SYSTEM DIAGNOSTICS";
  }
}

void switchPage(Page newPage);
void updateDynamic();

void navCallback(lv_event_t* event) {
  const auto selected = static_cast<Page>((uintptr_t)lv_event_get_user_data(event));
  if (selected == page) return;
  switchPage(selected);
}

void silenceBtnCallback(lv_event_t*) {
  if (supcfg::DISPLAY_TEST_MODE && !alarmActive()) {
    buzzerTone(2200);
    delay(150);
    buzzerOff();
    return;
  }
  silenceAlarm(900000); // Silence for 15 minutes
}

void screenGestureCallback(lv_event_t*) {
  lv_indev_t* indev = lv_indev_active();
  if (!indev) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT) {
    switchPage(static_cast<Page>((page + 1) % PAGE_COUNT));
  } else if (dir == LV_DIR_RIGHT) {
    switchPage(static_cast<Page>((page + PAGE_COUNT - 1) % PAGE_COUNT));
  }
}

void buildHeader(lv_obj_t* screen) {
  lv_obj_t* bar = card(screen, 0, 0, W, 30, HEADER_BG, HEADER_BG);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, c(BORDER), 0);

  label(bar, "Libbys Frogs", &lv_font_montserrat_14, CYAN, 8, 7);
  headerTitle = label(bar, "DASHBOARD", &lv_font_montserrat_12, TEXT, 105, 8);

  // Status Badge in top right
  headerBadge = card(bar, 238, 4, 76, 22, OK, OK);
  lv_obj_set_style_radius(headerBadge, 4, 0);
  headerBadgeLabel = label(headerBadge, "NOMINAL", &lv_font_montserrat_10, 0x000000, 0, 0);
  lv_obj_center(headerBadgeLabel);
}

void buildNav(lv_obj_t* screen) {
  navBar = card(screen, 0, 204, W, 36, HEADER_BG, HEADER_BG);
  lv_obj_set_style_radius(navBar, 0, 0);
  lv_obj_set_style_border_side(navBar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(navBar, 1, 0);
  lv_obj_set_style_border_color(navBar, c(BORDER), 0);

  constexpr const char* titles[PAGE_COUNT] = {"DASH", "CLIMATE", "SAFETY", "SYSTEM"};

  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    navButtons[i] = lv_button_create(navBar);
    lv_obj_set_size(navButtons[i], 74, 28);
    lv_obj_set_pos(navButtons[i], 4 + (i * 78), 4);
    lv_obj_set_style_bg_opa(navButtons[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(navButtons[i], c(i == page ? CARD_ALT : HEADER_BG), 0);
    lv_obj_set_style_border_width(navButtons[i], i == page ? 1 : 0, 0);
    lv_obj_set_style_border_color(navButtons[i], c(CYAN), 0);
    lv_obj_set_style_radius(navButtons[i], 5, 0);
    lv_obj_set_style_shadow_width(navButtons[i], 0, 0);

    lv_obj_add_event_cb(navButtons[i], navCallback, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));

    lv_obj_t* txt = lv_label_create(navButtons[i]);
    lv_label_set_text(txt, titles[i]);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(txt, c(i == page ? CYAN : MUTED), 0);
    lv_obj_center(txt);
  }
}

void refreshNav() {
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    if (!navButtons[i]) continue;
    const bool active = (i == page);
    lv_obj_set_style_bg_color(navButtons[i], c(active ? CARD_ALT : HEADER_BG), 0);
    lv_obj_set_style_border_width(navButtons[i], active ? 1 : 0, 0);
    lv_obj_t* txt = lv_obj_get_child(navButtons[i], 0);
    if (txt) {
      lv_obj_set_style_text_color(txt, c(active ? CYAN : MUTED), 0);
    }
  }
}

void buildPageOverview(lv_obj_t* parent) {
  // Top Row: 2 Hero Cards (Y=2, H=78)
  lv_obj_t* cydCard = card(parent, 4, 2, 153, 78);
  label(cydCard, "EXTERNAL AMBIENT", &lv_font_montserrat_10, CYAN, 8, 6);
  refs.ownTemp = label(cydCard, "-- °C", &lv_font_montserrat_24, TEXT, 8, 22);
  refs.ownHum = label(cydCard, "-- % RH", &lv_font_montserrat_12, MUTED, 8, 54);
  refs.ownStatus = label(cydCard, "SHT4x OK", &lv_font_montserrat_10, OK, 88, 56);

  lv_obj_t* mainCard = card(parent, 163, 2, 153, 78);
  label(mainCard, "MAIN CHAMBER AVG", &lv_font_montserrat_10, SKY, 8, 6);
  refs.mainAvg = label(mainCard, "-- °C", &lv_font_montserrat_24, TEXT, 8, 22);
  refs.mainDelta = label(mainCard, "Δ -- °C", &lv_font_montserrat_12, OK, 8, 54);
  refs.mainState = label(mainCard, "STATE: --", &lv_font_montserrat_10, MUTED, 84, 56);

  // Middle Quick Bar: Actuators & Water Reservoir (Y=82, H=32)
  lv_obj_t* actBar = card(parent, 4, 82, 312, 32);
  refs.dashMister = label(actBar, "Mist: OFF", &lv_font_montserrat_10, MUTED, 8, 9);
  refs.dashFan = label(actBar, "Fan: 0%", &lv_font_montserrat_10, MUTED, 85, 9);
  refs.dashPump = label(actBar, "Pump: OFF", &lv_font_montserrat_10, MUTED, 155, 9);
  refs.dashWater = label(actBar, "Water: OK", &lv_font_montserrat_10, OK, 235, 9);

  // Bottom Row: 3 Status Cards (Y=116, H=54)
  lv_obj_t* upCard = card(parent, 4, 116, 100, 54);
  label(upCard, "UPPER ZONE", &lv_font_montserrat_10, MUTED, 6, 4);
  refs.upperTemp = label(upCard, "-- °C", &lv_font_montserrat_14, TEXT, 6, 18);
  refs.upperHum = label(upCard, "-- %", &lv_font_montserrat_12, SKY, 6, 35);

  lv_obj_t* lowCard = card(parent, 110, 116, 100, 54);
  label(lowCard, "LOWER ZONE", &lv_font_montserrat_10, MUTED, 6, 4);
  refs.lowerTemp = label(lowCard, "-- °C", &lv_font_montserrat_14, TEXT, 6, 18);
  refs.lowerHum = label(lowCard, "-- %", &lv_font_montserrat_12, SKY, 6, 35);

  lv_obj_t* linkCard = card(parent, 216, 116, 100, 54);
  label(linkCard, "SUPERVISION", &lv_font_montserrat_10, MUTED, 6, 4);
  refs.hbAge = label(linkCard, "HB: --", &lv_font_montserrat_12, TEXT, 6, 18);
  refs.gradVal = label(linkCard, "Grad: --", &lv_font_montserrat_12, OK, 6, 35);
}

void buildPageClimate(lv_obj_t* parent) {
  lv_obj_t* left = card(parent, 4, 2, 153, 168);
  label(left, "EXTERNAL AMBIENT SHT4x", &lv_font_montserrat_10, CYAN, 8, 6);
  refs.climOwnTemp = label(left, "-- °C", &lv_font_montserrat_24, TEXT, 8, 22);
  refs.climOwnHum = label(left, "-- % RH", &lv_font_montserrat_14, MUTED, 8, 52);
  refs.climOwnStatus = label(left, "Range: VALID", &lv_font_montserrat_10, OK, 8, 80);
  refs.climDelta = label(left, "Array Δ: -- °C", &lv_font_montserrat_12, OK, 8, 104);
  label(left, "Main Controller GPIO", &lv_font_montserrat_10, MUTED, 8, 138);

  lv_obj_t* right = card(parent, 163, 2, 153, 168);
  label(right, "MAIN ARRAY & ACTUATORS", &lv_font_montserrat_10, SKY, 8, 6);
  refs.climUpper = label(right, "Upper: -- °C | --%", &lv_font_montserrat_12, TEXT, 8, 24);
  refs.climLower = label(right, "Lower: -- °C | --%", &lv_font_montserrat_12, TEXT, 8, 44);
  refs.climGrad = label(right, "Gradient: -- °C", &lv_font_montserrat_12, OK, 8, 64);
  refs.climSoil = label(right, "Soil: M1: --% | M2: --%", &lv_font_montserrat_12, MUTED, 8, 84);
  refs.climWater = label(right, "Water: Reservoir OK", &lv_font_montserrat_12, OK, 8, 104);
  refs.climActuators = label(right, "Actuators: All OFF", &lv_font_montserrat_12, TEXT, 8, 124);
  label(right, "Limit: Gr<8.0°C", &lv_font_montserrat_10, MUTED, 8, 148);
}

void buildPageSafety(lv_obj_t* parent) {
  refs.alarmBanner = card(parent, 4, 2, 312, 42, 0x05160A, OK);
  refs.alarmBannerTitle = label(refs.alarmBanner, "SYSTEM NOMINAL", &lv_font_montserrat_14, OK, 10, 4);
  refs.alarmBannerDetail = label(refs.alarmBanner, "All supervisory safety checks passing.", &lv_font_montserrat_10, MUTED, 10, 22);

  lv_obj_t* matrix = card(parent, 4, 46, 312, 78);
  label(matrix, "REAL-TIME SAFETY MATRIX", &lv_font_montserrat_10, SKY, 8, 4);

  refs.checkHb = label(matrix, "Heartbeat: OK", &lv_font_montserrat_10, OK, 8, 20);
  refs.checkTelem = label(matrix, "Telemetry: OK", &lv_font_montserrat_10, OK, 160, 20);
  refs.checkSht = label(matrix, "Ext SHT4x: OK", &lv_font_montserrat_10, OK, 8, 36);
  refs.checkRange = label(matrix, "Plausibility: PASS", &lv_font_montserrat_10, OK, 160, 36);
  refs.checkDelta = label(matrix, "Ref Only: OK", &lv_font_montserrat_10, OK, 8, 52);
  refs.checkGrad = label(matrix, "Gradient: PASS", &lv_font_montserrat_10, OK, 160, 52);
  refs.checkWater = label(matrix, "Reservoir: PASS", &lv_font_montserrat_10, OK, 8, 64);

  refs.silenceBtn = lv_button_create(parent);
  lv_obj_set_size(refs.silenceBtn, 312, 36);
  lv_obj_set_pos(refs.silenceBtn, 4, 126);
  lv_obj_set_style_bg_opa(refs.silenceBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(refs.silenceBtn, c(CARD_ALT), 0);
  lv_obj_set_style_border_width(refs.silenceBtn, 1, 0);
  lv_obj_set_style_border_color(refs.silenceBtn, c(BORDER), 0);
  lv_obj_set_style_radius(refs.silenceBtn, 6, 0);
  lv_obj_add_event_cb(refs.silenceBtn, silenceBtnCallback, LV_EVENT_CLICKED, nullptr);

  refs.silenceBtnLabel = lv_label_create(refs.silenceBtn);
  lv_label_set_text(refs.silenceBtnLabel, "BUZZER: ARMED & READY (TAP TO MUTE)");
  lv_obj_set_style_text_font(refs.silenceBtnLabel, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(refs.silenceBtnLabel, c(MUTED), 0);
  lv_obj_center(refs.silenceBtnLabel);
}

void buildPageSystem(lv_obj_t* parent) {
  lv_obj_t* c1 = card(parent, 4, 2, 153, 78);
  label(c1, "WI-FI LINK", &lv_font_montserrat_10, SKY, 8, 6);
  refs.sysWifiSsid = label(c1, "SSID: --", &lv_font_montserrat_10, TEXT, 8, 24);
  refs.sysWifiIp = label(c1, "IP: --", &lv_font_montserrat_10, OK, 8, 44);

  lv_obj_t* c2 = card(parent, 163, 2, 153, 78);
  label(c2, "MQTT BROKER", &lv_font_montserrat_10, SKY, 8, 6);
  refs.sysMqttBroker = label(c2, supcfg::MQTT_HOST, &lv_font_montserrat_10, TEXT, 8, 24);
  refs.sysMqttStatus = label(c2, "DISCONNECTED", &lv_font_montserrat_10, WARN, 8, 44);

  lv_obj_t* c3 = card(parent, 4, 82, 153, 84);
  label(c3, "CYD HARDWARE", &lv_font_montserrat_10, SKY, 8, 6);
  refs.sysHeap = label(c3, "RAM: -- KB", &lv_font_montserrat_10, TEXT, 8, 24);
  refs.sysUptime = label(c3, "Up: --", &lv_font_montserrat_10, MUTED, 8, 42);
  label(c3, "ESP32-2432S028R", &lv_font_montserrat_10, MUTED, 8, 60);

  lv_obj_t* c4 = card(parent, 163, 82, 153, 84);
  label(c4, "FIRMWARE CONFIG", &lv_font_montserrat_10, SKY, 8, 6);
  label(c4, "TERRA v7 Supervisor", &lv_font_montserrat_10, TEXT, 8, 24);
  refs.sysMode = label(c4, "MODE: --", &lv_font_montserrat_10, WARN, 8, 42);
  label(c4, "320x240 Landscape", &lv_font_montserrat_10, MUTED, 8, 60);
}

void switchPage(Page newPage) {
  page = newPage;
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    if (pageContainers[i]) {
      if (i == page) {
        lv_obj_remove_flag(pageContainers[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(pageContainers[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  refreshNav();
  if (headerTitle) {
    lv_label_set_text(headerTitle, pageName(page));
  }
  updateDynamic();
}

void updateDynamic() {
  const bool active = alarmActive();
  const uint32_t now = millis();
  const bool hbFresh = lastMainHeartbeatRx && (now - lastMainHeartbeatRx <= supcfg::HEARTBEAT_TIMEOUT_MS);
  const bool telemFresh = lastTelemetryRx && (now - lastTelemetryRx <= supcfg::STALE_TELEMETRY_MS);

  // --- Header Badge ---
  if (headerBadge && headerBadgeLabel) {
    if (active) {
      lv_obj_set_style_bg_color(headerBadge, c(CRIT), 0);
      lv_obj_set_style_border_color(headerBadge, c(CRIT), 0);
      lv_label_set_text(headerBadgeLabel, "ALARM");
      lv_obj_set_style_text_color(headerBadgeLabel, c(TEXT), 0);
    } else if (supcfg::DISPLAY_TEST_MODE) {
      lv_obj_set_style_bg_color(headerBadge, c(WARN), 0);
      lv_obj_set_style_border_color(headerBadge, c(WARN), 0);
      lv_label_set_text(headerBadgeLabel, "TEST");
      lv_obj_set_style_text_color(headerBadgeLabel, c(0x000000), 0);
    } else {
      lv_obj_set_style_bg_color(headerBadge, c(OK), 0);
      lv_obj_set_style_border_color(headerBadge, c(OK), 0);
      lv_label_set_text(headerBadgeLabel, "NOMINAL");
      lv_obj_set_style_text_color(headerBadgeLabel, c(0x000000), 0);
    }
  }

  const float delta = tempDelta();
  const bool deltaOk = !isnan(delta) && (fabsf(delta) <= supcfg::MAX_SENSOR_DIFFERENCE_C);
  const float grad = (!isnan(mainUpper) && !isnan(mainLower)) ? fabsf(mainUpper - mainLower) : NAN;
  const bool gradOk = !isnan(grad) && (grad <= supcfg::MAX_MAIN_GRADIENT_C);

  // --- Page Updates ---
  if (page == OVERVIEW) {
    if (refs.ownTemp) lv_label_set_text(refs.ownTemp, fmt(ownTemp, " °C").c_str());
    if (refs.ownHum)  lv_label_set_text(refs.ownHum, (fmt(ownHum, "%") + " RH").c_str());
    if (refs.ownStatus) {
      lv_label_set_text(refs.ownStatus, shtOk ? "SHT4x OK" : "SHT4x ERR");
      lv_obj_set_style_text_color(refs.ownStatus, c(shtOk ? OK : CRIT), 0);
    }

    if (refs.mainAvg) lv_label_set_text(refs.mainAvg, fmt(mainAverage(), " °C").c_str());
    if (refs.mainDelta) {
      String dStr = "Δ " + fmt(delta, " °C");
      lv_label_set_text(refs.mainDelta, dStr.c_str());
      lv_obj_set_style_text_color(refs.mainDelta, c(deltaOk ? OK : WARN), 0);
    }
    if (refs.mainState) {
      String st = "STATE: " + mainState;
      lv_label_set_text(refs.mainState, st.c_str());
    }

    if (refs.dashMister) {
      bool misting = mainMisterPump;
      lv_label_set_text(refs.dashMister, misting ? "Mist: ON" : "Mist: OFF");
      lv_obj_set_style_text_color(refs.dashMister, c(misting ? SKY : MUTED), 0);
    }
    if (refs.dashFan) {
      String f = "Fan: " + String((mainFanPwm * 100) / 255) + "%";
      lv_label_set_text(refs.dashFan, f.c_str());
      lv_obj_set_style_text_color(refs.dashFan, c(mainFanPwm > 0 ? SKY : MUTED), 0);
    }
    if (refs.dashPump) {
      lv_label_set_text(refs.dashPump, mainDrainagePump ? "Drain: ON" : "Drain: OFF");
      lv_obj_set_style_text_color(refs.dashPump, c(mainDrainagePump ? WARN : MUTED), 0);
    }
    if (refs.dashWater) {
      if (mainDoorOpen) {
        lv_label_set_text(refs.dashWater, "Door: OPEN");
        lv_obj_set_style_text_color(refs.dashWater, c(WARN), 0);
      } else if (mainWaterLow) {
        lv_label_set_text(refs.dashWater, "Water: LOW");
        lv_obj_set_style_text_color(refs.dashWater, c(WARN), 0);
      } else if (mainDrainageHigh) {
        lv_label_set_text(refs.dashWater, "Drain: HIGH");
        lv_obj_set_style_text_color(refs.dashWater, c(WARN), 0);
      } else {
        lv_label_set_text(refs.dashWater, "Water: OK");
        lv_obj_set_style_text_color(refs.dashWater, c(OK), 0);
      }
    }

    if (refs.upperTemp) lv_label_set_text(refs.upperTemp, fmt(mainUpper, " °C").c_str());
    if (refs.upperHum)  lv_label_set_text(refs.upperHum, (fmt(mainUpperHum, "%") + " RH").c_str());
    if (refs.lowerTemp) lv_label_set_text(refs.lowerTemp, fmt(mainLower, " °C").c_str());
    if (refs.lowerHum)  lv_label_set_text(refs.lowerHum, (fmt(mainLowerHum, "%") + " RH").c_str());

    if (refs.hbAge) {
      String hb = "HB: " + age(lastMainHeartbeatRx);
      lv_label_set_text(refs.hbAge, hb.c_str());
      lv_obj_set_style_text_color(refs.hbAge, c(hbFresh || supcfg::DISPLAY_TEST_MODE ? TEXT : WARN), 0);
    }
    if (refs.gradVal) {
      String gr = "Grad: " + fmt(grad, " °C");
      lv_label_set_text(refs.gradVal, gr.c_str());
      lv_obj_set_style_text_color(refs.gradVal, c(gradOk ? OK : WARN), 0);
    }
  }
  else if (page == CLIMATE) {
    if (refs.climOwnTemp) lv_label_set_text(refs.climOwnTemp, fmt(ownTemp, " °C").c_str());
    if (refs.climOwnHum)  lv_label_set_text(refs.climOwnHum, (fmt(ownHum, "%") + " RH").c_str());
    if (refs.climOwnStatus) {
      const bool rangeOk = !isnan(ownTemp) && ownTemp >= supcfg::MIN_SENSOR_TEMP_C && ownTemp <= supcfg::MAX_SENSOR_TEMP_C;
      lv_label_set_text(refs.climOwnStatus, rangeOk ? "Range [5-40°C]: VALID" : "Range [5-40°C]: FAULT");
      lv_obj_set_style_text_color(refs.climOwnStatus, c(rangeOk ? OK : CRIT), 0);
    }
    if (refs.climDelta) {
      String dStr = "Array Δ: " + fmt(delta, " °C") + (deltaOk ? " (PASS)" : " (HIGH)");
      lv_label_set_text(refs.climDelta, dStr.c_str());
      lv_obj_set_style_text_color(refs.climDelta, c(deltaOk ? OK : WARN), 0);
    }

    if (refs.climUpper) {
      String s = "Upper: " + fmt(mainUpper, " °C") + " | " + fmt(mainUpperHum, "%");
      lv_label_set_text(refs.climUpper, s.c_str());
    }
    if (refs.climLower) {
      String s = "Lower: " + fmt(mainLower, " °C") + " | " + fmt(mainLowerHum, "%");
      lv_label_set_text(refs.climLower, s.c_str());
    }
    if (refs.climGrad) {
      String s = "Gradient: " + fmt(grad, " °C") + (gradOk ? " (PASS)" : " (EXCESSIVE)");
      lv_label_set_text(refs.climGrad, s.c_str());
      lv_obj_set_style_text_color(refs.climGrad, c(gradOk ? OK : CRIT), 0);
    }
    if (refs.climSoil) {
      String s = "External: " + fmt(mainExternal, " °C") + " | Soil: " + fmt(mainSoil, "%");
      lv_label_set_text(refs.climSoil, s.c_str());
    }
    if (refs.climWater) {
      if (mainDoorOpen) {
        lv_label_set_text(refs.climWater, "Door: OPEN");
        lv_obj_set_style_text_color(refs.climWater, c(WARN), 0);
      } else if (mainWaterLow) {
        lv_label_set_text(refs.climWater, "Water: RESERVOIR LOW");
        lv_obj_set_style_text_color(refs.climWater, c(WARN), 0);
      } else if (mainDrainageHigh) {
        lv_label_set_text(refs.climWater, "Drainage: HIGH");
        lv_obj_set_style_text_color(refs.climWater, c(WARN), 0);
      } else {
        lv_label_set_text(refs.climWater, "Water: Reservoir OK");
        lv_obj_set_style_text_color(refs.climWater, c(OK), 0);
      }
    }
    if (refs.climActuators) {
      String act = "Mist:" + String(mainMisterPump ? "ON" : "OFF") +
           " Fog:" + String(mainFogger ? "ON" : "OFF") +
           " Heat:" + String(mainHeater ? "ON" : "OFF") +
                   " Fan:" + String((mainFanPwm * 100) / 255) + "%" +
           " Drain:" + String(mainDrainagePump ? "ON" : "OFF");
      lv_label_set_text(refs.climActuators, act.c_str());
    }
  }
  else if (page == SAFETY) {
    if (refs.alarmBanner && refs.alarmBannerTitle && refs.alarmBannerDetail) {
      if (active) {
        lv_obj_set_style_bg_color(refs.alarmBanner, c(0x3B1218), 0);
        lv_obj_set_style_border_color(refs.alarmBanner, c(CRIT), 0);
        String title = "CRITICAL ALARM: " + alarmCode;
        lv_label_set_text(refs.alarmBannerTitle, title.c_str());
        lv_obj_set_style_text_color(refs.alarmBannerTitle, c(CRIT), 0);
        lv_label_set_text(refs.alarmBannerDetail, alarmDetail.c_str());
      } else {
        lv_obj_set_style_bg_color(refs.alarmBanner, c(0x05160A), 0);
        lv_obj_set_style_border_color(refs.alarmBanner, c(OK), 0);
        lv_label_set_text(refs.alarmBannerTitle, "SYSTEM NOMINAL");
        lv_obj_set_style_text_color(refs.alarmBannerTitle, c(OK), 0);
        lv_label_set_text(refs.alarmBannerDetail, "All supervisory safety checks passing.");
      }
    }

    if (refs.checkHb) {
      String s = "Heartbeat: " + age(lastMainHeartbeatRx) + (hbFresh || supcfg::DISPLAY_TEST_MODE ? " [OK]" : " [LOST]");
      lv_label_set_text(refs.checkHb, s.c_str());
      lv_obj_set_style_text_color(refs.checkHb, c(hbFresh || supcfg::DISPLAY_TEST_MODE ? OK : CRIT), 0);
    }
    if (refs.checkTelem) {
      String s = "Telemetry: " + age(lastTelemetryRx) + (telemFresh || supcfg::DISPLAY_TEST_MODE ? " [OK]" : " [STALE]");
      lv_label_set_text(refs.checkTelem, s.c_str());
      lv_obj_set_style_text_color(refs.checkTelem, c(telemFresh || supcfg::DISPLAY_TEST_MODE ? OK : WARN), 0);
    }
    if (refs.checkSht) {
      lv_label_set_text(refs.checkSht, shtOk ? "Ext SHT4x: OK" : "Ext SHT4x: FAULT");
      lv_obj_set_style_text_color(refs.checkSht, c(shtOk ? OK : CRIT), 0);
    }
    if (refs.checkRange) {
      const bool rangeOk = !isnan(ownTemp) && ownTemp >= supcfg::MIN_SENSOR_TEMP_C && ownTemp <= supcfg::MAX_SENSOR_TEMP_C;
      lv_label_set_text(refs.checkRange, rangeOk ? "Ext Plausibility: PASS" : "Ext Plausibility: FAIL");
      lv_obj_set_style_text_color(refs.checkRange, c(rangeOk ? OK : CRIT), 0);
    }
    if (refs.checkDelta) {
      lv_label_set_text(refs.checkDelta, "Ref Only: OK");
      lv_obj_set_style_text_color(refs.checkDelta, c(OK), 0);
    }
    if (refs.checkGrad) {
      lv_label_set_text(refs.checkGrad, gradOk ? "Gradient: PASS" : "Gradient: EXCESSIVE");
      lv_obj_set_style_text_color(refs.checkGrad, c(gradOk ? OK : CRIT), 0);
    }
    if (refs.checkWater) {
      if (mainDoorOpen) {
        lv_label_set_text(refs.checkWater, "Door: OPEN");
        lv_obj_set_style_text_color(refs.checkWater, c(WARN), 0);
      } else if (mainWaterLow) {
        lv_label_set_text(refs.checkWater, "Reservoir: LOW WATER");
        lv_obj_set_style_text_color(refs.checkWater, c(WARN), 0);
      } else if (mainDrainageHigh) {
        lv_label_set_text(refs.checkWater, "Drainage: HIGH");
        lv_obj_set_style_text_color(refs.checkWater, c(WARN), 0);
      } else {
        lv_label_set_text(refs.checkWater, "Reservoir: PASS");
        lv_obj_set_style_text_color(refs.checkWater, c(OK), 0);
      }
    }

    if (refs.silenceBtn && refs.silenceBtnLabel) {
      const bool isSilenced = alarmSilenced && millis() < alarmSilencedUntil;
      if (isSilenced) {
        const uint32_t remSec = (alarmSilencedUntil - millis()) / 1000U;
        String s = "BUZZER SILENCED (" + String(remSec / 60U) + "m " + String(remSec % 60U) + "s REMAINING)";
        lv_label_set_text(refs.silenceBtnLabel, s.c_str());
        lv_obj_set_style_text_color(refs.silenceBtnLabel, c(WARN), 0);
        lv_obj_set_style_border_color(refs.silenceBtn, c(WARN), 0);
      } else if (active) {
        lv_label_set_text(refs.silenceBtnLabel, "TAP TO SILENCE BUZZER (15 MIN)");
        lv_obj_set_style_text_color(refs.silenceBtnLabel, c(CRIT), 0);
        lv_obj_set_style_border_color(refs.silenceBtn, c(CRIT), 0);
      } else if (supcfg::DISPLAY_TEST_MODE) {
        lv_label_set_text(refs.silenceBtnLabel, "BUZZER: TAP TO TEST TONE");
        lv_obj_set_style_text_color(refs.silenceBtnLabel, c(CYAN), 0);
        lv_obj_set_style_border_color(refs.silenceBtn, c(CYAN), 0);
      } else {
        lv_label_set_text(refs.silenceBtnLabel, "BUZZER: ARMED & READY (TAP TO MUTE)");
        lv_obj_set_style_text_color(refs.silenceBtnLabel, c(MUTED), 0);
        lv_obj_set_style_border_color(refs.silenceBtn, c(BORDER), 0);
      }
    }
  }
  else if (page == SYSTEM) {
    if (refs.sysWifiSsid) {
      String s = "SSID: " + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : (supcfg::DISPLAY_TEST_MODE ? "TEST_MODE" : "OFFLINE"));
      lv_label_set_text(refs.sysWifiSsid, s.c_str());
    }
    if (refs.sysWifiIp) {
      String ip = (WiFi.status() == WL_CONNECTED) ? ("IP: " + WiFi.localIP().toString()) : (supcfg::DISPLAY_TEST_MODE ? "IP: 192.168.1.99" : "IP: --");
      lv_label_set_text(refs.sysWifiIp, ip.c_str());
      lv_obj_set_style_text_color(refs.sysWifiIp, c(WiFi.status() == WL_CONNECTED || supcfg::DISPLAY_TEST_MODE ? OK : WARN), 0);
    }
    if (refs.sysMqttStatus) {
      const bool mqOk = mqtt.connected() || supcfg::DISPLAY_TEST_MODE;
      lv_label_set_text(refs.sysMqttStatus, mqOk ? "ONLINE (CONNECTED)" : "DISCONNECTED");
      lv_obj_set_style_text_color(refs.sysMqttStatus, c(mqOk ? OK : WARN), 0);
    }
    if (refs.sysHeap) {
      String h = "RAM: " + String(ESP.getFreeHeap() / 1024U) + " KB Free";
      lv_label_set_text(refs.sysHeap, h.c_str());
    }
    if (refs.sysUptime) {
      String u = "Up: " + uptimeStr();
      lv_label_set_text(refs.sysUptime, u.c_str());
    }
    if (refs.sysMode) {
      lv_label_set_text(refs.sysMode, supcfg::DISPLAY_TEST_MODE ? "MODE: DISPLAY TEST" : "MODE: PRODUCTION LIVE");
      lv_obj_set_style_text_color(refs.sysMode, c(supcfg::DISPLAY_TEST_MODE ? WARN : OK), 0);
    }
  }
}

// ---------- LVGL Display & Touch Drivers ----------
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

void touchscreen_read(lv_indev_t*, lv_indev_data_t* data) {
  if (touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    data->state = LV_INDEV_STATE_PRESSED;

    int x = map(p.x, supcfg::TOUCH_X_MIN, supcfg::TOUCH_X_MAX, 0, W - 1);
    int y = map(p.y, supcfg::TOUCH_Y_MIN, supcfg::TOUCH_Y_MAX, 0, H - 1);

    if (supcfg::TOUCH_SWAP_XY) {
      int tmp = x; x = y; y = tmp;
    }
    if (supcfg::TOUCH_INVERT_X) {
      x = (W - 1) - x;
    }
    if (supcfg::TOUCH_INVERT_Y) {
      y = (H - 1) - y;
    }

    data->point.x = constrain(x, 0, W - 1);
    data->point.y = constrain(y, 0, H - 1);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void initUI() {
  lv_init();

  touchscreenSPI.begin(supcfg::PIN_TOUCH_SCLK, supcfg::PIN_TOUCH_MISO, supcfg::PIN_TOUCH_MOSI, supcfg::PIN_TOUCH_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2); // Aligns XPT2046 with 240x320 base before LVGL rotation

  displayHandle = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(displayHandle, LV_DISPLAY_ROTATION_270);

  // Explicitly initialize and apply LVGL Dark Theme
  lv_theme_t* th = lv_theme_default_init(
      displayHandle,
      lv_palette_main(LV_PALETTE_CYAN),
      lv_palette_main(LV_PALETTE_AMBER),
      true, // dark mode enabled
      &lv_font_montserrat_14);
  lv_display_set_theme(displayHandle, th);

  inputHandle = lv_indev_create();
  lv_indev_set_type(inputHandle, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(inputHandle, touchscreen_read);
  lv_indev_set_display(inputHandle, displayHandle);

  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, c(BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  noScroll(screen);
  lv_obj_add_event_cb(screen, screenGestureCallback, LV_EVENT_GESTURE, nullptr);

  buildHeader(screen);

  content = lv_obj_create(screen);
  noScroll(content);
  lv_obj_set_pos(content, 0, 30);
  lv_obj_set_size(content, W, 174);
  lv_obj_set_style_bg_color(content, c(BG), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_add_event_cb(content, screenGestureCallback, LV_EVENT_GESTURE, nullptr);

  buildNav(screen);

  // Pre-create all page containers once to eliminate object recreation churn on tab switch
  for (uint8_t p = 0; p < PAGE_COUNT; ++p) {
    pageContainers[p] = lv_obj_create(content);
    noScroll(pageContainers[p]);
    lv_obj_set_pos(pageContainers[p], 0, 0);
    lv_obj_set_size(pageContainers[p], W, 174);
    lv_obj_set_style_bg_color(pageContainers[p], c(BG), 0);
    lv_obj_set_style_bg_opa(pageContainers[p], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pageContainers[p], 0, 0);
    lv_obj_set_style_pad_all(pageContainers[p], 0, 0);
    lv_obj_add_event_cb(pageContainers[p], screenGestureCallback, LV_EVENT_GESTURE, nullptr);

    switch (p) {
      case OVERVIEW: buildPageOverview(pageContainers[p]); break;
      case CLIMATE:  buildPageClimate(pageContainers[p]); break;
      case SAFETY:   buildPageSafety(pageContainers[p]); break;
      case SYSTEM:   buildPageSystem(pageContainers[p]); break;
    }
  }

  switchPage(OVERVIEW);
}

void uiService() {
  if (millis() - lastUiData >= supcfg::UI_INTERVAL_MS) {
    lastUiData = millis();
    updateDynamic();
  }
  lv_task_handler();
  static uint32_t lastTick = millis();
  const uint32_t now = millis();
  if (now > lastTick) {
    lv_tick_inc(now - lastTick);
    lastTick = now;
  }
}

} // namespace UI

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(100);

  bootMs = millis();
  bootCounter = esp_random();

  buzzerSetup();
  buzzerOff();

  pinMode(supcfg::PIN_TFT_BACKLIGHT, OUTPUT);
  digitalWrite(supcfg::PIN_TFT_BACKLIGHT, HIGH);

  mqtt.setServer(supcfg::MQTT_HOST, supcfg::MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  UI::initUI();
}

void loop() {
  wifiService();
  mqttService();
  mqtt.loop();

  sensorService();
  supervisionService();
  buzzerService();

  UI::uiService();

  delay(2);
}
