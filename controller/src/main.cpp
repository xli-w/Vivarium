#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Adafruit_SHT4x.h>
#include <esp_task_wdt.h>
#include <cmath>
#include "config.h"
#include "model.h"

WiFiClient net;
PubSubClient mqtt(net);
DHT dhtExternal(cfg::PIN_EXTERNAL_DHT, DHT11);
Adafruit_SHT4x sht4Upper;
Adafruit_SHT4x sht4Lower;
Sensors sensors;
Outputs outputs;
Runtime runtime;

uint32_t lastSensor = 0;
uint32_t lastControl = 0;
uint32_t lastHeartbeat = 0;
uint32_t lastTelemetry = 0;
uint32_t heartbeatSequence = 0;
uint32_t telemetrySequence = 0;

bool validTemperature(float value) {
  return !isnan(value) && value >= cfg::MIN_VALID_TEMP_C && value <= cfg::MAX_VALID_TEMP_C;
}

bool validHumidity(float value) {
  return !isnan(value) && value >= 0.0f && value <= 100.0f;
}

float moisturePct(int raw) {
  if (cfg::SOIL_DRY == cfg::SOIL_WET) return NAN;
  const float p = 100.0f * (raw - cfg::SOIL_DRY) /
                  static_cast<float>(cfg::SOIL_WET - cfg::SOIL_DRY);
  return constrain(p, 0.0f, 100.0f);
}

bool fresh(uint32_t updatedAt) {
  return updatedAt != 0 && millis() - updatedAt <= cfg::SENSOR_STALE_MS;
}

bool credentialsConfigured() {
  return strcmp(cfg::WIFI_SSID, "CHANGE_ME") != 0 &&
         strcmp(cfg::WIFI_PASSWORD, "CHANGE_ME") != 0 &&
         strcmp(cfg::MQTT_PASSWORD, "CHANGE_ME") != 0;
}

void writeOutput(uint8_t pin, bool on) {
  const bool levelHigh = on == cfg::OUTPUT_ACTIVE_HIGH;
  digitalWrite(pin, levelHigh ? HIGH : LOW);
}

bool tcaSelect(uint8_t channel) {
  if (!cfg::USE_TCA9548A || channel > 7) return !cfg::USE_TCA9548A;
  Wire.beginTransmission(cfg::TCA9548A_ADDR);
  Wire.write(static_cast<uint8_t>(1U << channel));
  return Wire.endTransmission() == 0;
}

void setAlarm(AlarmCode alarm) {
  if (runtime.alarm != alarm) {
    if (alarm != AlarmCode::NONE) runtime.alarmSeq++;
    runtime.alarm = alarm;
  }
  outputs.alarm = alarm != AlarmCode::NONE;
}

void setMister(bool on) {
  const bool wasOn = outputs.misterPump;
  if (on && (sensors.doorOpen || sensors.reservoirLow || sensors.drainageHigh)) on = false;
  outputs.misterPump = on;
  if (on) runtime.misterStarted = millis();
  if (wasOn && !on) runtime.lastMister = millis();
  writeOutput(cfg::PIN_MISTER_PUMP, on);
}

bool foggerWatered() {
  return outputs.misterPump ||
         (runtime.lastMister != 0 &&
          millis() - runtime.lastMister <= cfg::FOGGER_POST_MIST_MS);
}

void setFogger(bool on) {
  const bool wasOn = outputs.fogger;
  if (on && (sensors.doorOpen || sensors.reservoirLow || sensors.drainageHigh ||
             !foggerWatered())) on = false;
  outputs.fogger = on;
  if (on) runtime.foggerStarted = millis();
  if (wasOn && !on) runtime.lastFogger = millis();
  writeOutput(cfg::PIN_FOGGER, on);
}

void setHeater(bool on) {
  const bool wasOn = outputs.heater;
  if (on && (runtime.heaterLockout || sensors.doorOpen ||
             sensors.upperTemp >= cfg::HARD_TEMP_C || sensors.lowerTemp >= cfg::HARD_TEMP_C)) {
    on = false;
  }
  if (on && !wasOn) runtime.heaterStarted = millis();
  if (wasOn && !on) runtime.lastHeaterOff = millis();
  outputs.heater = on;
  writeOutput(cfg::PIN_HEATER, on);
}

void setDrainagePump(bool on) {
  if (on && (!sensors.drainageHigh || runtime.drainageLockout)) on = false;
  outputs.drainagePump = on;
  if (on) runtime.drainageStarted = millis();
  writeOutput(cfg::PIN_DRAINAGE_PUMP, on);
}

void setFan(uint8_t pwm) {
  if (sensors.doorOpen) pwm = 0;
  outputs.fan = pwm;
  const uint32_t duty = cfg::OUTPUT_ACTIVE_HIGH ? pwm : 255 - pwm;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(cfg::PIN_FAN_PWM, duty);
#else
  ledcWrite(cfg::PWM_CH_FAN, duty);
#endif
}

uint32_t servoDuty(uint8_t degrees) {
  const uint32_t pulseUs = cfg::SERVO_MIN_US +
      (static_cast<uint32_t>(cfg::SERVO_MAX_US - cfg::SERVO_MIN_US) * degrees) / 180U;
  return (pulseUs * 4095U) / 20000U;
}

void setServo(uint8_t degrees) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(cfg::PIN_FOOD_SERVO, servoDuty(degrees));
#else
  ledcWrite(cfg::PWM_CH_SERVO, servoDuty(degrees));
#endif
}

void startFeed() {
  const uint32_t now = millis();
  if (runtime.feedPhase != 0 ||
      (runtime.lastFeed != 0 && now - runtime.lastFeed < cfg::FEED_COOLDOWN_MS)) return;
  runtime.feedPhase = 1;
  runtime.feedStarted = now;
  outputs.foodServoActive = true;
  setServo(cfg::SERVO_FEED_DEG);
}

void serviceFeed() {
  if (runtime.feedPhase == 1 && millis() - runtime.feedStarted >= cfg::FEED_MOVE_MS) {
    setServo(cfg::SERVO_REST_DEG);
    runtime.feedPhase = 2;
    runtime.feedStarted = millis();
    runtime.lastFeed = millis();
  } else if (runtime.feedPhase == 2 && millis() - runtime.feedStarted >= cfg::FEED_MOVE_MS) {
    runtime.feedPhase = 0;
    outputs.foodServoActive = false;
  }
}

void stopClimateOutputs() {
  setMister(false);
  setFogger(false);
  setHeater(false);
  setFan(0);
}

void markManualActivity() {
  runtime.manual = true;
  runtime.manualLastActivity = millis();
  runtime.manualTimeoutAlarm = false;
}

bool expireManualMode() {
  if (!runtime.manual || runtime.manualLastActivity == 0 ||
      millis() - runtime.manualLastActivity < cfg::MANUAL_TIMEOUT_MS) {
    return false;
  }
  stopClimateOutputs();
  runtime.manual = false;
  runtime.manualLastActivity = 0;
  runtime.manualTimeoutAlarm = true;
  return true;
}

uint32_t manualRemainingSeconds() {
  if (!runtime.manual || runtime.manualLastActivity == 0) return 0;
  const uint32_t elapsed = millis() - runtime.manualLastActivity;
  if (elapsed >= cfg::MANUAL_TIMEOUT_MS) return 0;
  return (cfg::MANUAL_TIMEOUT_MS - elapsed) / 1000U;
}

void readSht(Adafruit_SHT4x& sensor, uint8_t channel, float& temperature,
            float& humidity, bool& ok, uint32_t& updatedAt) {
  sensors_event_t humidityEvent;
  sensors_event_t temperatureEvent;
  if (tcaSelect(channel) && sensor.getEvent(&humidityEvent, &temperatureEvent) &&
      validTemperature(temperatureEvent.temperature) &&
      validHumidity(humidityEvent.relative_humidity)) {
    temperature = temperatureEvent.temperature;
    humidity = humidityEvent.relative_humidity;
    ok = true;
    updatedAt = millis();
  } else {
    temperature = NAN;
    humidity = NAN;
    ok = false;
  }
}

void readDht(float& temperature, float& humidity, bool& ok, uint32_t& updatedAt) {
  const float nextTemperature = dhtExternal.readTemperature();
  const float nextHumidity = dhtExternal.readHumidity();
  if (validTemperature(nextTemperature) && validHumidity(nextHumidity)) {
    temperature = nextTemperature;
    humidity = nextHumidity;
    ok = true;
    updatedAt = millis();
  } else {
    temperature = NAN;
    humidity = NAN;
    ok = false;
  }
}

void readSensors() {
  sensors.soilRaw = analogRead(cfg::PIN_SOIL_MOISTURE);
  sensors.soilMoisture = moisturePct(sensors.soilRaw);
  sensors.soilUpdatedAt = millis();
  sensors.reservoirLow = digitalRead(cfg::PIN_RESERVOIR_LEVEL) == cfg::RESERVOIR_LOW_ACTIVE;
  sensors.drainageHigh = digitalRead(cfg::PIN_DRAINAGE_LEVEL) == cfg::DRAINAGE_HIGH_ACTIVE;
  sensors.doorOpen = digitalRead(cfg::PIN_DOOR_REED) == cfg::DOOR_OPEN_ACTIVE;
  sensors.levelsUpdatedAt = millis();
  readDht(sensors.externalTemp, sensors.externalHumidity, sensors.externalOk,
      sensors.externalUpdatedAt);
  readSht(sht4Upper, cfg::TCA_CH_UPPER, sensors.upperTemp,
          sensors.upperHumidity, sensors.upperOk, sensors.upperUpdatedAt);
  readSht(sht4Lower, cfg::TCA_CH_LOWER, sensors.lowerTemp,
          sensors.lowerHumidity, sensors.lowerOk, sensors.lowerUpdatedAt);
}

void readFastInterlocks() {
  sensors.reservoirLow = digitalRead(cfg::PIN_RESERVOIR_LEVEL) == cfg::RESERVOIR_LOW_ACTIVE;
  sensors.drainageHigh = digitalRead(cfg::PIN_DRAINAGE_LEVEL) == cfg::DRAINAGE_HIGH_ACTIVE;
  sensors.doorOpen = digitalRead(cfg::PIN_DOOR_REED) == cfg::DOOR_OPEN_ACTIVE;
  sensors.levelsUpdatedAt = millis();
}

bool climateValid() {
  return sensors.upperOk && sensors.lowerOk &&
         fresh(sensors.upperUpdatedAt) &&
         fresh(sensors.lowerUpdatedAt) && fresh(sensors.soilUpdatedAt) &&
         fresh(sensors.levelsUpdatedAt);
}

bool tooHot() {
  return sensors.upperTemp >= cfg::HARD_TEMP_C || sensors.lowerTemp >= cfg::HARD_TEMP_C;
}

bool publishJson(const char* topicSuffix, JsonDocument& document) {
  if (!mqtt.connected()) return false;
  char payload[1536];
  const size_t length = serializeJson(document, payload, sizeof(payload));
  if (length == 0 || length >= sizeof(payload)) return false;
  char topic[64];
  snprintf(topic, sizeof(topic), "vivarium/%s", topicSuffix);
  return mqtt.publish(topic, payload, false);
}

void clearRetainedStatus() {
  mqtt.publish("vivarium/main/heartbeat", "", true);
  mqtt.publish("vivarium/main/telemetry", "", true);
}

void publishHeartbeat() {
  StaticJsonDocument<768> d;
  d["deviceId"] = cfg::DEVICE_ID;
  d["bootId"] = runtime.bootId;
  d["sequence"] = ++heartbeatSequence;
  d["uptimeMs"] = millis();
  d["state"] = stateName(runtime.state);
  d["alarm"] = alarmName(runtime.alarm);
  d["alarmSequence"] = runtime.alarmSeq;
  d["wifi"] = runtime.wifi;
  d["mqtt"] = runtime.mqtt;
  d["externalSensorOk"] = sensors.externalOk;
  d["upperSensorOk"] = sensors.upperOk;
  d["lowerSensorOk"] = sensors.lowerOk;
  publishJson("main/heartbeat", d);
}

void publishTelemetry() {
  StaticJsonDocument<1536> d;
  d["deviceId"] = cfg::DEVICE_ID;
  d["sequence"] = ++telemetrySequence;
  d["state"] = stateName(runtime.state);
  d["alarm"] = alarmName(runtime.alarm);
  d["alarmSequence"] = runtime.alarmSeq;
  d["externalTemperatureC"] = sensors.externalTemp;
  d["externalHumidityPct"] = sensors.externalHumidity;
  d["upperTemperatureC"] = sensors.upperTemp;
  d["upperHumidityPct"] = sensors.upperHumidity;
  d["lowerTemperatureC"] = sensors.lowerTemp;
  d["lowerHumidityPct"] = sensors.lowerHumidity;
  d["soilMoisturePct"] = sensors.soilMoisture;
  d["soilRaw"] = sensors.soilRaw;
  d["externalSensorOk"] = sensors.externalOk;
  d["upperSensorOk"] = sensors.upperOk;
  d["lowerSensorOk"] = sensors.lowerOk;
  d["reservoirLow"] = sensors.reservoirLow;
  d["drainageHigh"] = sensors.drainageHigh;
  d["doorOpen"] = sensors.doorOpen;
  d["manual"] = runtime.manual;
  d["manualRemainingSeconds"] = manualRemainingSeconds();
  d["manualTimedOut"] = runtime.manualTimeoutAlarm;
  d["misterPump"] = outputs.misterPump;
  d["fogger"] = outputs.fogger;
  d["heater"] = outputs.heater;
  d["fanPwm"] = outputs.fan;
  d["drainagePump"] = outputs.drainagePump;
  d["foodServoActive"] = outputs.foodServoActive;
  publishJson("main/telemetry", d);
}

void applyCommand(JsonDocument& d) {
  const char* command = d["command"] | "";
  const char* value = d["value"] | "";
  const bool on = strcmp(value, "ON") == 0;
  if (strcmp(command, "manual") == 0) {
    runtime.manual = on;
    if (on) {
      markManualActivity();
    } else {
      runtime.manualLastActivity = 0;
      runtime.manualTimeoutAlarm = false;
    }
  } else if (strcmp(command, "alloff") == 0) {
    stopClimateOutputs();
    markManualActivity();
  } else if (strcmp(command, "mister") == 0) {
    markManualActivity();
    setMister(on && !sensors.doorOpen && !sensors.reservoirLow && climateValid() && !tooHot());
  } else if (strcmp(command, "fogger") == 0) {
    markManualActivity();
    setFogger(on && climateValid() && !tooHot());
  } else if (strcmp(command, "heater") == 0) {
    markManualActivity();
    setHeater(on && climateValid() && !tooHot());
  } else if (strcmp(command, "fan") == 0) {
    markManualActivity();
    const int requested = constrain(d["value"] | 0, 0, 255);
    setFan(static_cast<uint8_t>(requested));
  } else if (strcmp(command, "feed") == 0 && on) {
    startFeed();
  }
}

void mqttCallback(char* /*topic*/, byte* payload, unsigned int length) {
  StaticJsonDocument<512> d;
  if (deserializeJson(d, payload, length) == DeserializationError::Ok) applyCommand(d);
}

void wifiService() {
  if (!credentialsConfigured()) {
    runtime.wifi = false;
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    runtime.wifi = true;
    return;
  }
  runtime.wifi = false;
  if (millis() - runtime.lastWifiAttempt < cfg::WIFI_RECONNECT_INTERVAL_MS) return;
  runtime.lastWifiAttempt = millis();
  WiFi.begin(cfg::WIFI_SSID, cfg::WIFI_PASSWORD);
}

void mqttService() {
  if (!runtime.wifi) {
    runtime.mqtt = false;
    runtime.commandSubscribed = false;
    return;
  }
  if (mqtt.connected()) {
    const uint32_t now = millis();
    if (!runtime.commandSubscribed &&
        now - runtime.lastMqttSubscribeAttempt >= cfg::MQTT_RECONNECT_INTERVAL_MS) {
      runtime.lastMqttSubscribeAttempt = now;
      runtime.commandSubscribed = mqtt.subscribe("vivarium/main/command", 1);
    }
    runtime.mqtt = runtime.commandSubscribed;
    return;
  }
  runtime.mqtt = false;
  runtime.commandSubscribed = false;
  if (millis() - runtime.lastMqttAttempt < cfg::MQTT_RECONNECT_INTERVAL_MS) return;
  runtime.lastMqttAttempt = millis();
  String clientId = String(cfg::DEVICE_ID) + "-" + String(runtime.bootId, HEX);
  if (mqtt.connect(clientId.c_str(), cfg::MQTT_USER, cfg::MQTT_PASSWORD)) {
    clearRetainedStatus();
    runtime.lastMqttSubscribeAttempt = millis();
    runtime.commandSubscribed = mqtt.subscribe("vivarium/main/command", 1);
    runtime.mqtt = runtime.commandSubscribed;
  }
}

void enforceOutputTimeouts() {
  const uint32_t now = millis();
  if (outputs.misterPump && now - runtime.misterStarted >= cfg::MISTER_MAX_ON_MS) {
    setMister(false);
  }
  if (outputs.fogger && now - runtime.foggerStarted >= cfg::FOGGER_MAX_ON_MS) {
    setFogger(false);
  }
  if (outputs.drainagePump && now - runtime.drainageStarted >= cfg::DRAINAGE_MAX_RUNTIME_MS) {
    setDrainagePump(false);
    runtime.drainageLockout = true;
    setAlarm(AlarmCode::DRAINAGE_TIMEOUT);
  }
  if (outputs.heater && now - runtime.heaterStarted >= cfg::HEATER_MAX_ON_MS) {
    setHeater(false);
    runtime.heaterLockout = true;
    setAlarm(AlarmCode::HEATER_TIMEOUT);
  }
}

void controlLoop() {
  const uint32_t now = millis();
  AlarmCode nextAlarm = AlarmCode::NONE;
  readFastInterlocks();
  enforceOutputTimeouts();
  const bool manualTimedOut = expireManualMode();
  if (manualTimedOut) nextAlarm = AlarmCode::MANUAL_TIMEOUT;
  if (!sensors.externalOk || !fresh(sensors.externalUpdatedAt)) {
    nextAlarm = AlarmCode::EXTERNAL_SENSOR_FAULT;
  }

  if (!sensors.drainageHigh) {
    runtime.drainageLockout = false;
    if (outputs.drainagePump) setDrainagePump(false);
  }
  if (sensors.drainageHigh && !outputs.drainagePump && !runtime.drainageLockout) {
    setDrainagePump(true);
  }

  if (!climateValid()) {
    stopClimateOutputs();
    runtime.state = SystemState::SENSOR_FAULT;
    nextAlarm = AlarmCode::SENSOR_FAULT;
  } else if (tooHot()) {
    setMister(false);
    setFogger(false);
    setHeater(false);
    setFan(cfg::EMERGENCY_FAN_PWM);
    runtime.state = SystemState::OVER_TEMP;
    nextAlarm = AlarmCode::OVER_TEMP;
  } else if (sensors.doorOpen) {
    setMister(false);
    setFogger(false);
    setFan(0);
    setHeater(false);
    runtime.state = SystemState::MANUAL;
    nextAlarm = AlarmCode::DOOR_OPEN;
  } else {
    const float maxHumidity = max(sensors.upperHumidity, sensors.lowerHumidity);
    const bool moistureDemand = sensors.soilMoisture < cfg::SOIL_START_PCT;
    const bool recovered = sensors.soilMoisture >= cfg::SOIL_STOP_PCT &&
                           maxHumidity >= cfg::HUMIDITY_STOP_PCT;

    if (sensors.reservoirLow) {
      setMister(false);
      setFogger(false);
      nextAlarm = AlarmCode::WATER_LOW;
      runtime.state = SystemState::LOW_RESERVOIR;
    } else if (!runtime.manual) {
      if (moistureDemand && !outputs.misterPump &&
          now - runtime.lastMister >= cfg::MISTER_COOLDOWN_MS) {
        setMister(true);
      }
      if (outputs.misterPump && !outputs.fogger &&
          now - runtime.misterStarted >= cfg::FOGGER_START_DELAY_MS) {
        setFogger(true);
      }
      if (recovered) {
        setMister(false);
        setFogger(false);
      }
    }

    if (!runtime.manual) {
      if (!outputs.heater && !runtime.heaterLockout &&
          sensors.upperTemp <= cfg::HEAT_ON_BELOW_C &&
          sensors.lowerTemp <= cfg::HEAT_ON_BELOW_C &&
          now - runtime.lastHeaterOff >= cfg::HEATER_MIN_OFF_MS) {
        setHeater(true);
      } else if (outputs.heater && sensors.upperTemp >= cfg::HEAT_OFF_ABOVE_C &&
                 sensors.lowerTemp >= cfg::HEAT_OFF_ABOVE_C) {
        setHeater(false);
        runtime.heaterLockout = false;
      }
    }

    uint8_t fan = outputs.heater ? cfg::HIGH_TEMP_FAN_PWM : cfg::BASE_FAN_PWM;
    if (maxHumidity > cfg::HUMIDITY_HIGH_PCT) fan = max(fan, cfg::HIGH_HUMIDITY_FAN_PWM);
    if (max(sensors.upperTemp, sensors.lowerTemp) > cfg::HIGH_TEMP_C) {
      fan = max(fan, cfg::HIGH_TEMP_FAN_PWM);
    }
    if (!runtime.manual) setFan(fan);
    runtime.state = outputs.heater ? SystemState::HEATING :
                    outputs.misterPump ? SystemState::MISTING :
                    outputs.fogger ? SystemState::FOGGING :
                    runtime.manual ? SystemState::MANUAL : SystemState::NORMAL;
  }

  if (sensors.drainageHigh && outputs.drainagePump) runtime.state = SystemState::DRAINING;
  if (runtime.alarm == AlarmCode::DRAINAGE_TIMEOUT && nextAlarm == AlarmCode::NONE) {
    nextAlarm = AlarmCode::DRAINAGE_TIMEOUT;
  }
  if (runtime.alarm == AlarmCode::HEATER_TIMEOUT && nextAlarm == AlarmCode::NONE) {
    nextAlarm = AlarmCode::HEATER_TIMEOUT;
  }
  if (runtime.manualTimeoutAlarm && nextAlarm == AlarmCode::NONE) {
    nextAlarm = AlarmCode::MANUAL_TIMEOUT;
  }
  setAlarm(nextAlarm);
  writeOutput(cfg::PIN_ALARM, outputs.alarm);
  writeOutput(cfg::PIN_STATUS_LED, ((millis() / 500U) % 2U) != 0);
}

void safeOutputsAtBoot() {
  pinMode(cfg::PIN_MISTER_PUMP, OUTPUT);
  pinMode(cfg::PIN_FOGGER, OUTPUT);
  pinMode(cfg::PIN_HEATER, OUTPUT);
  pinMode(cfg::PIN_DRAINAGE_PUMP, OUTPUT);
  pinMode(cfg::PIN_ALARM, OUTPUT);
  pinMode(cfg::PIN_STATUS_LED, OUTPUT);
  pinMode(cfg::PIN_RESERVOIR_LEVEL, INPUT_PULLUP);
  pinMode(cfg::PIN_DRAINAGE_LEVEL, INPUT_PULLUP);
  pinMode(cfg::PIN_DOOR_REED, INPUT_PULLUP);
  writeOutput(cfg::PIN_MISTER_PUMP, false);
  writeOutput(cfg::PIN_FOGGER, false);
  writeOutput(cfg::PIN_HEATER, false);
  writeOutput(cfg::PIN_DRAINAGE_PUMP, false);
  writeOutput(cfg::PIN_ALARM, false);
  writeOutput(cfg::PIN_STATUS_LED, false);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(cfg::PIN_FAN_PWM, 25000, 8);
  ledcAttach(cfg::PIN_FOOD_SERVO, 50, 12);
#else
  ledcSetup(cfg::PWM_CH_FAN, 25000, 8);
  ledcAttachPin(cfg::PIN_FAN_PWM, cfg::PWM_CH_FAN);
  ledcSetup(cfg::PWM_CH_SERVO, 50, 12);
  ledcAttachPin(cfg::PIN_FOOD_SERVO, cfg::PWM_CH_SERVO);
#endif
  setFan(0);
  setServo(cfg::SERVO_REST_DEG);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  runtime.bootId = esp_random();
  safeOutputsAtBoot();
  Wire.begin(cfg::PIN_I2C_SDA, cfg::PIN_I2C_SCL);

  bool upperInit = false;
  bool lowerInit = false;
  dhtExternal.begin();
  if (tcaSelect(cfg::TCA_CH_UPPER)) upperInit = sht4Upper.begin(&Wire);
  if (tcaSelect(cfg::TCA_CH_LOWER)) lowerInit = sht4Lower.begin(&Wire);
  Serial.printf("DHT11 external=READY upper SHT4x=%s lower SHT4x=%s\n",
                upperInit ? "OK" : "FAIL", lowerInit ? "OK" : "FAIL");

  mqtt.setServer(cfg::MQTT_HOST, cfg::MQTT_PORT);
  mqtt.setBufferSize(1536);
  mqtt.setCallback(mqttCallback);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t watchdog = {
      .timeout_ms = 10 * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&watchdog);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
#endif

  readSensors();
  controlLoop();
}

void loop() {
  esp_task_wdt_reset();
  const uint32_t now = millis();
  wifiService();
  mqttService();
  mqtt.loop();
  serviceFeed();
  if (now - lastSensor >= cfg::SENSOR_INTERVAL_MS) {
    lastSensor = now;
    readSensors();
  }
  if (now - lastControl >= cfg::CONTROL_INTERVAL_MS) {
    lastControl = now;
    controlLoop();
  }
  if (now - lastHeartbeat >= cfg::HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    publishHeartbeat();
  }
  if (now - lastTelemetry >= cfg::TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    publishTelemetry();
  }
  delay(5);
}