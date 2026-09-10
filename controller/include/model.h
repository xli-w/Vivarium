#pragma once
#include <Arduino.h>

enum class SystemState : uint8_t {
  BOOTING,
  NORMAL,
  MANUAL,
  LOW_RESERVOIR,
  DRAINING,
  MISTING,
  FOGGING,
  HEATING,
  SENSOR_FAULT,
  OVER_TEMP,
  CONTROLLER_FAULT
};

inline const char* stateName(SystemState s) {
  switch (s) {
    case SystemState::BOOTING: return "BOOTING";
    case SystemState::NORMAL: return "NORMAL";
    case SystemState::MANUAL: return "MANUAL";
    case SystemState::LOW_RESERVOIR: return "LOW_RESERVOIR";
    case SystemState::DRAINING: return "DRAINING";
    case SystemState::MISTING: return "MISTING";
    case SystemState::FOGGING: return "FOGGING";
    case SystemState::HEATING: return "HEATING";
    case SystemState::SENSOR_FAULT: return "SENSOR_FAULT";
    case SystemState::OVER_TEMP: return "OVER_TEMP";
    default: return "CONTROLLER_FAULT";
  }
}

enum class AlarmCode : uint8_t {
  NONE,
  SENSOR_FAULT,
  EXTERNAL_SENSOR_FAULT,
  WATER_LOW,
  DRAINAGE_TIMEOUT,
  DOOR_OPEN,
  OVER_TEMP,
  HEATER_TIMEOUT,
  FEED_COOLDOWN,
  MANUAL_TIMEOUT,
  OUTPUT_FAULT
};

inline const char* alarmName(AlarmCode a) {
  switch (a) {
    case AlarmCode::SENSOR_FAULT: return "SENSOR_FAULT";
    case AlarmCode::EXTERNAL_SENSOR_FAULT: return "EXTERNAL_SENSOR_FAULT";
    case AlarmCode::WATER_LOW: return "WATER_LOW";
    case AlarmCode::DRAINAGE_TIMEOUT: return "DRAINAGE_TIMEOUT";
    case AlarmCode::DOOR_OPEN: return "DOOR_OPEN";
    case AlarmCode::OVER_TEMP: return "OVER_TEMP";
    case AlarmCode::HEATER_TIMEOUT: return "HEATER_TIMEOUT";
    case AlarmCode::FEED_COOLDOWN: return "FEED_COOLDOWN";
    case AlarmCode::MANUAL_TIMEOUT: return "MANUAL_TIMEOUT";
    case AlarmCode::OUTPUT_FAULT: return "OUTPUT_FAULT";
    default: return "NONE";
  }
}

struct Sensors {
  float externalTemp = NAN;
  float externalHumidity = NAN;
  float upperTemp = NAN;
  float lowerTemp = NAN;
  float upperHumidity = NAN;
  float lowerHumidity = NAN;
  float soilMoisture = NAN;
  int soilRaw = 0;
  bool externalOk = false;
  bool upperOk = false;
  bool lowerOk = false;
  bool reservoirLow = false;
  bool drainageHigh = false;
  bool doorOpen = true;
  uint32_t externalUpdatedAt = 0;
  uint32_t upperUpdatedAt = 0;
  uint32_t lowerUpdatedAt = 0;
  uint32_t soilUpdatedAt = 0;
  uint32_t levelsUpdatedAt = 0;
};

struct Outputs {
  bool misterPump = false;
  bool fogger = false;
  bool heater = false;
  bool drainagePump = false;
  bool foodServoActive = false;
  bool alarm = false;
  uint8_t fan = 0;
};

struct Runtime {
  SystemState state = SystemState::BOOTING;
  AlarmCode alarm = AlarmCode::NONE;
  uint32_t alarmSeq = 0;
  bool manual = false;
  bool manualTimeoutAlarm = false;
  uint32_t manualLastActivity = 0;
  bool wifi = false;
  bool mqtt = false;
  uint32_t bootId = 0;
  uint32_t lastMister = 0;
  uint32_t misterStarted = 0;
  uint32_t lastFogger = 0;
  uint32_t foggerStarted = 0;
  uint32_t drainageStarted = 0;
  bool drainageLockout = false;
  uint32_t heaterStarted = 0;
  uint32_t lastHeaterOff = 0;
  bool heaterLockout = false;
  uint32_t lastFeed = 0;
  uint32_t feedStarted = 0;
  uint8_t feedPhase = 0;
  uint32_t lastWifiAttempt = 0;
  uint32_t lastMqttAttempt = 0;
  uint32_t lastMqttSubscribeAttempt = 0;
  bool commandSubscribed = false;
};
