# TERRA v7 Operations and API

## 1. Runtime configuration

### Main controller and supervisor

Network and hardware constants are compiled into each firmware image:

- Main controller: `controller/include/config.h`.
- Independent supervisor: `supervisor/include/config.h`.

Do not place production passwords in source control. The current firmware configuration files contain placeholders and must be changed before flashing. Keep the main and supervisor device IDs unique.

### Raspberry Pi environment

Copy `pi/.env.example` to `.env` and set:

| Variable | Purpose | Default |
| --- | --- | --- |
| `MQTT_HOST` / `MQTT_PORT` | Broker address | `localhost` / `1883` |
| `MQTT_USER` / `MQTT_PASSWORD` | Broker credentials | empty |
| `DB_PATH` | SQLite database path | `/var/lib/frog-vivarium/vivarium.db` |
| `ALERT_EMAIL` | Alert recipient | empty |
| `SMTP_HOST` / `SMTP_PORT` | SMTP server | empty / `587` |
| `SMTP_USER` / `SMTP_PASSWORD` | SMTP credentials | empty |
| `ALERT_COOLDOWN_S` | Minimum repeat interval per alarm code | `900` |
| `TELEMETRY_RETENTION_DAYS` | SQLite retention window | `30` |
| `HEARTBEAT_TIMEOUT_S` | Pi heartbeat freshness threshold | `15` |
| `TELEMETRY_TIMEOUT_S` | Pi telemetry freshness threshold | `15` |
| `API_TOKEN` | Required API key | empty |
| `TARGET_TEMPERATURE_MIN_C` / `TARGET_TEMPERATURE_MAX_C` | Dashboard temperature target band | `20.0` / `26.0` |
| `TARGET_HUMIDITY_MIN_PCT` / `TARGET_HUMIDITY_MAX_PCT` | Dashboard humidity target band | `60.0` / `85.0` |
| `TARGET_SOIL_MIN_PCT` / `TARGET_SOIL_MAX_PCT` | Dashboard soil-moisture target band | `35.0` / `70.0` |
| `CAMERA_ENABLED` | Enable local USB camera capture | `true` |
| `CAMERA_DEVICE_INDEX` | V4L2 device index (e.g. `/dev/video0` -> `0`) | `0` |
| `CAMERA_WIDTH` / `CAMERA_HEIGHT` | USB camera capture resolution | `1280` / `720` |
| `CAMERA_FPS` | USB camera capture / stream frame rate | `15` |
| `CAMERA_SNAPSHOT_URL` | Optional fallback remote JPEG/PNG/WebP snapshot source | empty |
| `CAMERA_STREAM_URL` | Optional credential-free browser-compatible stream source | empty |
| `CAMERA_TIMEOUT_S` | Snapshot fetch timeout for remote camera | `3` |
| `DASHBOARD_POLL_S` | Browser dashboard refresh interval | `5` |

The service refuses API access when `API_TOKEN` is empty. Use a long, unique value and send it as the `X-API-Key` header.

## 2. MQTT contract

| Topic | Direction | Frequency | Retained | Payload role |
| --- | --- | ---: | --- | --- |
| `vivarium/main/heartbeat` | Main to broker | 2 s | No | Liveness, state, alarm, sensor status. |
| `vivarium/main/telemetry` | Main to broker | 5 s | No | Measurements, interlocks, and actuator state. |
| `vivarium/main/alarm` | Reserved main alarm topic | Not currently emitted | No | Reserved for a future dedicated main alarm stream; current main alarms are included in heartbeat and telemetry. |
| `vivarium/supervisor/alarm` | Supervisor to broker | Event/repeat | No | Independent supervision alarms. |
| `vivarium/main/command` | Pi to main | On demand | No | Validated actuator/control command. |

Health payloads include `deviceId`, `bootId`, `sequence`, state/alarm fields, and sensor or actuator fields. Consumers must use receipt time and freshness thresholds, not MQTT retained state. Empty payloads are retained-message cleanup tombstones and are ignored.

## 3. Commands

The Pi API validates commands before publishing this JSON shape:

```json
{"command":"mister","value":"ON"}
```

Allowed commands:

| Command | Accepted value | Effect |
| --- | --- | --- |
| `mister` | `ON` or `OFF` | Request mister state. |
| `fogger` | `ON` or `OFF` | Request fogger state. An `ON` request is accepted only while the mister is running or during its five-second post-mist wetting window. |
| `heater` | `ON` or `OFF` | Request heater state. |
| `fan` | Integer 0 to 255 | Request fan PWM duty. |
| `feed` | `ON` | Start a feed cycle if cooldown permits. |
| `manual` | `ON` or `OFF` | Disable or enable automatic climate demand. Manual mode expires after 10 minutes without a manual command. |
| `alloff` | Any value accepted by API | Stop climate outputs and enter manual mode. |

The main controller independently rechecks safety conditions. A successful API response means the message was published to MQTT, not that the actuator turned on.

## 4. HTTP API

The systemd service runs FastAPI through Uvicorn on `0.0.0.0:8080`. Every endpoint requires `X-API-Key`.

The root path `/` serves the operational dashboard static files (`index.html`, `notifications.html`, `history.html`, `settings.html`, and `mobile.html`).
- **Live Overview (`index.html`)**: Features an enclosure camera view, main controller status, and recent alarms/audit log in the hero section, followed by a compact Environment & Fluid Matrix (climate sensors, soil moisture, misting reservoir level, substrate drainage level, and auto drainage pump state) and interactive actuator control widgets (mister, fogger, heater, 3-speed fan control, feeder trigger, manual mode toggle, and emergency stop).
- **Climate History (`history.html`)**: Provides selectable 6-hour, 24-hour, and 7-day interactive chart windows with color-coded legends for temperature, humidity, and soil moisture trends queried from `/api/history`.
- **Mobile Control Centre (`mobile.html`)**: Touch-optimized single-thumb view with 48px touch targets, compact sensor/fluid matrix, camera frame, and quick actuator triggers.
- **Notifications (`notifications.html`)**: Active/recovered notifications, acknowledgement, and email queue/cooldown status.
- **Settings (`settings.html`)**: Combined system diagnostics, command audit, display preferences, and offline-cache status. Target bands live inline with Overview telemetry; telemetry export lives in History.

The browser polls rather than opening a WebSocket, so stale or unavailable data is shown explicitly.

### Health

```text
GET /api/health
```

Reports MQTT connection state, latest main heartbeat and telemetry, supervisor alarm, message ages, freshness booleans, and database health.

### Telemetry history

```text
GET /api/telemetry?limit=200
```

Returns recent SQLite telemetry rows, newest first. `limit` is restricted to 1 through 5000.

### Alarm history

```text
GET /api/alarms?limit=200
```

Returns recent alarm rows, newest first. `limit` is restricted to 1 through 5000.

### Notifications and audit

```text
GET  /api/notifications?limit=100
POST /api/notifications/<alarm-id>/ack
GET  /api/audit?limit=200
GET  /api/targets
```

Notifications include unacknowledged alarms, recovery events, email configuration/queue/cooldown state, and persisted acknowledgements. Target bands are configured with `TARGET_TEMPERATURE_MIN_C`, `TARGET_TEMPERATURE_MAX_C`, `TARGET_HUMIDITY_MIN_PCT`, `TARGET_HUMIDITY_MAX_PCT`, `TARGET_SOIL_MIN_PCT`, and `TARGET_SOIL_MAX_PCT`.

### Data export

```text
GET /api/export?format=csv&since=<unix-seconds>&until=<unix-seconds>
GET /api/export?format=json&since=<unix-seconds>&until=<unix-seconds>
```

Exports decoded telemetry with timestamps and source fields. The default maximum is 10,000 records.

### Publish a command

```text
POST /api/command
Content-Type: application/json
X-API-Key: <token>

{"command":"fan","value":128}
```

Responses are `200` for a successful MQTT publish, `400` for invalid commands or values, `401` for an incorrect key, `503` for missing API configuration or unavailable MQTT, and `422` for malformed request bodies.

### Dashboard snapshot

```text
GET /api/dashboard
```

Returns the current health response, decoded latest heartbeat and telemetry, supervisor alarm, message ages, camera availability, and the recommended browser polling interval.

### Chart history

```text
GET /api/history?since=<unix-seconds>&until=<unix-seconds>&limit=2000
```

Returns chronological telemetry records with decoded JSON under `data`. The endpoint is intended for chart consumers and bounds responses to 10,000 rows.

### Camera snapshot

```text
GET /api/camera/snapshot
```

Fetches the configured camera snapshot through the Pi and returns it without caching. The endpoint accepts JPEG, PNG, and WebP responses, applies the configured timeout and size limit, and returns `404` when no snapshot URL is configured or `502` when the camera is unavailable. Camera credentials remain server-side when the snapshot URL contains authentication handled by the Pi-side source.

The dashboard only exposes a stream URL when it is an `http` or `https` URL without user info or query parameters. Configure tokenized or credentialed streams only behind a separate trusted proxy; otherwise use the Pi-side snapshot proxy.

## 5. Pi storage and alerts

SQLite contains two tables:

- `telemetry(ts, source, payload)`: raw main telemetry payloads.
- `alarms(ts, source, code, severity, detail, payload)`: parsed alarm metadata plus raw payload.
- `alarm_ack(alarm_id, acknowledged_at)`: notification acknowledgement state.
- `command_audit(ts, command, value, published)`: command publication attempts. This does not confirm physical actuator state.

Rows older than `TELEMETRY_RETENTION_DAYS` are purged during database writes, at most once per hour. The database uses WAL mode, normal synchronous mode, a 10-second busy timeout, and indexes for time, source, and alarm code.

Email delivery is asynchronous so MQTT processing and API requests are not blocked by SMTP. A bounded queue, three retries, and per-code cooldown protect the service from an alert storm. Missing SMTP configuration suppresses delivery and logs a warning; it does not stop telemetry storage.

## 6. Deployment

The supplied [systemd unit](../pi/systemd-frog-vivarium.service) expects:

- application at `/opt/frog-vivarium/pi`;
- virtual environment at `/opt/frog-vivarium/venv`;
- environment file at `/opt/frog-vivarium/pi/.env`;
- database directory `/var/lib/frog-vivarium`;
- service user `vivarium`;
- MQTT service `mosquitto.service`.

Typical host-side setup is:

```text
python -m venv /opt/frog-vivarium/venv
/opt/frog-vivarium/venv/bin/pip install -r pi/requirements.txt
sudo systemctl enable --now frog-vivarium.service
sudo journalctl -u frog-vivarium.service -f
```

The unit uses a restricted service account, a private temporary directory, a read-only system, and an explicit writable database path. Keep the `.env` file readable only by the service account or an administrative group.

## 7. Troubleshooting

### No main heartbeat

Check main power, Wi-Fi credentials, broker address, MQTT credentials, broker ACLs, and the controller serial log. The controller continues local control while network troubleshooting is underway.

### Telemetry is present but stale

Check that the controller is running its loop, that its MQTT command subscription status is not masking a reconnect problem, and that the broker is not applying retained health data. Health topics must be live and non-retained.

### Main reports `SENSOR_FAULT`

Check upper/lower SHT4x power and channel wiring, TCA9548A selection, soil calibration, analog wiring, and sensor values against the configured validity range. Do not bypass the fault to run a heater or water actuator.

### Main reports `EXTERNAL_SENSOR_FAULT`

Check DHT11 power, pull-up, GPIO 2 wiring, and the sensor data line. Climate control remains available, but the external reference should be repaired before relying on ambient comparisons.

### Pi API returns `503`

For missing API configuration, set `API_TOKEN`. For MQTT failure, check broker reachability and credentials. The main controller does not depend on this API.

### Alerts are not arriving

Check SMTP host, port, credentials, recipient, TLS requirements, and the service journal. Confirm the alarm code has not been suppressed by `ALERT_COOLDOWN_S` and that the alert queue is not full.

### Supervisor reports `MAIN_OFFLINE`

Check main power and broker connectivity first. The supervisor intentionally waits for a live heartbeat rather than trusting retained messages. Restore the main controller and allow a fresh heartbeat sequence before accepting recovery.