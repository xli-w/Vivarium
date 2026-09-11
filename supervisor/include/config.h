#pragma once
#include <Arduino.h>

namespace supcfg {
constexpr bool DISPLAY_TEST_MODE = false;

constexpr const char* WIFI_SSID = "CHANGE_ME";
constexpr const char* WIFI_PASSWORD = "CHANGE_ME";
constexpr const char* MQTT_HOST = "192.168.1.10";
constexpr uint16_t MQTT_PORT = 1883;
constexpr const char* MQTT_USER = "CHANGE_ME";
constexpr const char* MQTT_PASSWORD = "CHANGE_ME";
constexpr const char* DEVICE_ID = "vivarium-cyd";

// CYD / ESP32-2432S028R — 2.8in ILI9341 320x240 Landscape mode.
// TFT rotation 1 = Landscape (USB on right), 3 = Landscape (USB on left).
constexpr uint8_t PIN_BUZZER = 26;

constexpr uint8_t PIN_TFT_CS = 15;
constexpr uint8_t PIN_TFT_DC = 2;
constexpr int8_t PIN_TFT_RST = -1;
constexpr uint8_t PIN_TFT_MOSI = 13;
constexpr uint8_t PIN_TFT_MISO = 12;
constexpr uint8_t PIN_TFT_SCLK = 14;
constexpr uint8_t PIN_TFT_BACKLIGHT = 21;
constexpr uint8_t TFT_ROTATION = 1; // 1 = 320x240 Landscape (3 = Landscape 180° inverted)

constexpr uint8_t PIN_TOUCH_CS = 33;
constexpr uint8_t PIN_TOUCH_IRQ = 36;
constexpr uint8_t PIN_TOUCH_MOSI = 32;
constexpr uint8_t PIN_TOUCH_MISO = 39;
constexpr uint8_t PIN_TOUCH_SCLK = 25;

// Starting calibration for the common CYD 2.8in resistive panel.
constexpr int TOUCH_X_MIN = 300;
constexpr int TOUCH_X_MAX = 3900;
constexpr int TOUCH_Y_MIN = 3700;
constexpr int TOUCH_Y_MAX = 200;
constexpr bool TOUCH_SWAP_XY = false;
constexpr bool TOUCH_INVERT_X = false;
constexpr bool TOUCH_INVERT_Y = false;

constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;
constexpr uint32_t UI_INTERVAL_MS = 500;
constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 10000;
constexpr uint32_t STALE_TELEMETRY_MS = 15000;
constexpr uint32_t ALARM_REPEAT_MS = 900000;
constexpr uint32_t SENSOR_WARMUP_MS = 5000;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 120;
constexpr uint16_t SWIPE_THRESHOLD_PX = 55;

constexpr float MAX_SENSOR_TEMP_C = 40.0f;
constexpr float MIN_SENSOR_TEMP_C = 5.0f;
constexpr float MAX_SENSOR_DIFFERENCE_C = 4.0f;
constexpr float MAX_MAIN_GRADIENT_C = 8.0f;
}
