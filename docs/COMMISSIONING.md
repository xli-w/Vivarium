# TERRA v7 Commissioning

Commission in stages. Do not connect pumps, heater, or other high-current loads until the controller has passed the low-risk input and output checks.

## 1. Prepare the build and configuration

- [ ] Confirm the repository version is v7.
- [ ] Install the required PlatformIO environments for `controller` and `supervisor`.
- [ ] Configure controller Wi-Fi, MQTT host, MQTT port, user, password, and `DEVICE_ID` in `controller/include/config.h`.
- [ ] Configure supervisor Wi-Fi, MQTT host, MQTT port, user, password, and `DEVICE_ID` in `supervisor/include/config.h`.
- [ ] Set `DISPLAY_TEST_MODE=true` for the isolated supervisor bench test only.
- [ ] Copy `pi/.env.example` to the Pi service directory as `.env`.
- [ ] Set a unique non-placeholder `API_TOKEN` and the production MQTT/database/email settings.
- [ ] Verify the MQTT broker allows the three expected client identities and topics.
- [ ] Record the final GPIO polarity and soil calibration values before installation.

## 2. Inspect wiring and power

- [ ] Keep all actuator loads disconnected for the first controller power-up.
- [ ] Verify common ground, regulated logic power, and separate suitable power for pumps, heater driver, and servo.
- [ ] Verify flyback protection for inductive loads and a correctly rated relay/SSR for the heater.
- [ ] Verify an independent hardware thermal cutoff for the heater.
- [ ] Verify the TCA9548A at address `0x70` and channels 2 external, 0 upper, 1 lower.
- [ ] Verify SHT4x wiring on the I2C bus: SDA GPIO 8, SCL GPIO 9.
- [ ] Verify soil input GPIO 1, reservoir GPIO 6, drainage GPIO 7, and door GPIO 10.
- [ ] Verify actuator outputs GPIO 11 through 18 against the wiring table in [Architecture](ARCHITECTURE.md).
- [ ] Confirm the configured `OUTPUT_ACTIVE_HIGH`, `DOOR_OPEN_ACTIVE`, `RESERVOIR_LOW_ACTIVE`, and `DRAINAGE_HIGH_ACTIVE` values match the installed hardware.

## 3. Bring up the main controller

- [ ] Power the controller with actuators disconnected.
- [ ] Confirm safe boot leaves pumps, heater, fan, drainage pump, alarm, and servo in their safe states.
- [ ] Confirm all three SHT4x devices initialise independently.
- [ ] Confirm the soil reading changes across dry and wet reference samples; update `SOIL_DRY` and `SOIL_WET` if needed.
- [ ] Confirm reservoir-low, drainage-high, and door-open readings have the expected polarity.
- [ ] Connect one actuator at a time through its driver and verify the reported state matches the physical state.
- [ ] Verify fan PWM with the driver connected, including zero duty and maximum duty.
- [ ] Verify the servo returns to `SERVO_REST_DEG` and moves to `SERVO_FEED_DEG` only during a feed command.

## 4. Verify local safety behavior

- [ ] Force reservoir low; mister and fogger must stop and `WATER_LOW` must be reported.
- [ ] Force drainage high; the drainage pump must start and stop when the level clears.
- [ ] Hold drainage high through the timeout; the pump must stop and remain locked out until the level clears.
- [ ] Open the door; mister, fogger, fan, and heater must stop and `DOOR_OPEN` must be reported.
- [ ] Force a temperature at or above 30 C; climate outputs must stop and the emergency fan must run at 255.
- [ ] Disconnect each climate sensor in turn; climate outputs must remain off and `SENSOR_FAULT` must be reported.
- [ ] Verify mister maximum runtime and cooldown, then verify the fogger begins one second into a mist cycle, runs no longer than five seconds, and cannot start unless the mister is active or stopped within the five-second post-mist wetting window.
- [ ] Verify heater maximum runtime, minimum off time, and lockout behavior.
- [ ] Verify feeding movement, rest position, and one-hour cooldown.
- [ ] Send manual ON commands under each interlock condition; no command may bypass a hard safety rule.
- [ ] Disconnect Wi-Fi while active; local control and protection must continue.

## 5. Bring up the MQTT and Pi path

- [ ] Start the MQTT broker and verify the controller reconnects.
- [ ] Confirm live `vivarium/main/heartbeat` messages arrive approximately every 2 seconds.
- [ ] Confirm live `vivarium/main/telemetry` messages arrive approximately every 5 seconds.
- [ ] Confirm health messages are not retained and that old retained values are cleared during controller connection.
- [ ] Start the Pi service and verify it subscribes without errors.
- [ ] Confirm telemetry is stored in SQLite and alarm records include code, severity, detail, source, and payload.
- [ ] Verify API requests without the correct `X-API-Key` are rejected.
- [ ] Verify valid commands publish to `vivarium/main/command` and invalid command values are rejected.
- [ ] Confirm `/api/health` reports MQTT, database, and freshness state accurately.
- [ ] Reboot the Pi; the main controller must continue local operation while the Pi is unavailable.

## 6. Bring up and validate the independent supervisor

- [ ] With `DISPLAY_TEST_MODE=true`, power the supervisor from USB with live loads disconnected.
- [ ] Confirm the display, touch input, and buzzer operate locally without MQTT.
- [ ] Set `DISPLAY_TEST_MODE=false` and configure production credentials before network commissioning.
- [ ] Confirm main heartbeat and telemetry are received.
- [ ] Power down the main controller and confirm `MAIN_OFFLINE` after the configured timeout.
- [ ] Restore the main controller and confirm automatic recovery.
- [ ] Create an excessive upper/lower gradient and confirm `MAIN_GRADIENT`.
- [ ] Propagate a main-controller alarm and confirm `MAIN_ALARM`.
- [ ] Confirm the supervisor never publishes actuator commands.

## 7. Acceptance and handover

- [ ] Reconnect all loads and repeat the critical interlock tests.
- [ ] Confirm the heater hardware cutoff independently of firmware.
- [ ] Confirm database retention and available disk space on the Pi.
- [ ] Confirm email delivery and alert cooldown behavior.
- [ ] Save the final configuration values, broker address, device IDs, calibration values, and wiring changes with the installation record.
- [ ] Leave `DISPLAY_TEST_MODE=false` in the production supervisor firmware.
- [ ] Open the Pi dashboard at `/` and verify the overview, stale-state indicators, fluid levels, alarm list, camera fallback, and command feedback.
- [ ] Verify navigation across Overview, Notifications, History, and Settings; confirm the Mobile version is reachable from the footer link.
- [ ] Verify the dashboard requires the configured API key and does not expose it in the URL.
- [ ] Verify dashboard controls reflect later telemetry rather than assuming a command changed the actuator.
