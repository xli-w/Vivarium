#pragma once
#include <Arduino.h>

namespace cfg {
// ===== Network =====
constexpr const char* WIFI_SSID = "CHANGE_ME";
constexpr const char* WIFI_PASSWORD = "CHANGE_ME";
constexpr const char* MQTT_HOST = "192.168.1.10";
constexpr uint16_t MQTT_PORT = 1883;
constexpr const char* MQTT_USER = "terrarium";
constexpr const char* MQTT_PASSWORD = "CHANGE_ME";
constexpr const char* DEVICE_ID = "terrarium-main";

// ===== Main controller I/O =====
constexpr uint8_t PIN_SOIL_MOISTURE = 1;
constexpr uint8_t PIN_RESERVOIR_LEVEL = 6;
constexpr uint8_t PIN_DRAINAGE_LEVEL = 7;
constexpr uint8_t PIN_I2C_SDA = 8;
constexpr uint8_t PIN_I2C_SCL = 9;
constexpr uint8_t PIN_DOOR_REED = 10;

constexpr bool USE_TCA9548A = true;
constexpr uint8_t TCA9548A_ADDR = 0x70;
constexpr uint8_t TCA_CH_EXTERNAL = 2;
constexpr uint8_t TCA_CH_UPPER = 0;
constexpr uint8_t TCA_CH_LOWER = 1;

constexpr uint8_t PIN_MISTER_PUMP = 11;
constexpr uint8_t PIN_FOGGER = 12;
constexpr uint8_t PIN_HEATER = 13;
constexpr uint8_t PIN_FAN_PWM = 14;
constexpr uint8_t PIN_DRAINAGE_PUMP = 15;
constexpr uint8_t PIN_FOOD_SERVO = 16;
constexpr uint8_t PIN_ALARM = 17;
constexpr uint8_t PIN_STATUS_LED = 18;

// ===== Signal polarity =====
constexpr bool RESERVOIR_LOW_ACTIVE = true;
constexpr bool DRAINAGE_HIGH_ACTIVE = true;
// With INPUT_PULLUP, wire the reed so an open door reads HIGH.
constexpr bool DOOR_OPEN_ACTIVE = true;
constexpr bool OUTPUT_ACTIVE_HIGH = true;

// ===== Timing =====
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;
constexpr uint32_t CONTROL_INTERVAL_MS = 250;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 2000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 5000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;

// ===== Sensor validity =====
constexpr uint32_t SENSOR_STALE_MS = 10000;
constexpr float MIN_VALID_TEMP_C = -20.0f;
constexpr float MAX_VALID_TEMP_C = 60.0f;

// ===== Actuator protection =====
constexpr uint32_t MISTER_MAX_ON_MS = 5000;
constexpr uint32_t MISTER_COOLDOWN_MS = 30000;
constexpr uint32_t FOGGER_START_DELAY_MS = 1000;
constexpr uint32_t FOGGER_MAX_ON_MS = 5000;
constexpr uint32_t FOGGER_POST_MIST_MS = 5000;
constexpr uint32_t DRAINAGE_MAX_RUNTIME_MS = 120000;
constexpr uint32_t HEATER_MAX_ON_MS = 900000;
constexpr uint32_t HEATER_MIN_OFF_MS = 30000;
constexpr uint32_t FEED_COOLDOWN_MS = 3600000;
constexpr uint32_t FEED_MOVE_MS = 800;
constexpr uint32_t MANUAL_TIMEOUT_MS = 600000;

// ===== Servo positions =====
constexpr uint8_t SERVO_REST_DEG = 10;
constexpr uint8_t SERVO_FEED_DEG = 100;
constexpr uint16_t SERVO_MIN_US = 500;
constexpr uint16_t SERVO_MAX_US = 2500;

// ===== Climate targets =====
constexpr float HARD_TEMP_C = 30.0f;
constexpr float HEAT_ON_BELOW_C = 22.0f;
constexpr float HEAT_OFF_ABOVE_C = 24.0f;
constexpr float HIGH_TEMP_C = 27.0f;
constexpr float HUMIDITY_START_PCT = 70.0f;
constexpr float HUMIDITY_STOP_PCT = 78.0f;
constexpr float HUMIDITY_HIGH_PCT = 90.0f;
constexpr float SOIL_START_PCT = 35.0f;
constexpr float SOIL_STOP_PCT = 48.0f;

constexpr uint8_t BASE_FAN_PWM = 50;
constexpr uint8_t HIGH_HUMIDITY_FAN_PWM = 130;
constexpr uint8_t HIGH_TEMP_FAN_PWM = 220;
constexpr uint8_t EMERGENCY_FAN_PWM = 255;

// ===== Moisture calibration =====
constexpr int SOIL_DRY = 3200;
constexpr int SOIL_WET = 1200;
}
