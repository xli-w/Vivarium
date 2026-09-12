# Vivarium v7 Operations and API

## 1. Runtime configuration

### Main controller and supervisor

Network and hardware constants are compiled into each firmware image:

- Main controller: `controller/include/config.h`.
- Independent supervisor: `supervisor/include/config.h`.

The checked-in firmware headers contain `CHANGE_ME` placeholders. Edit those headers in a local, uncommitted working copy before building and flashing, or supply equivalent private build-time configuration. Do not place production passwords in source control. Keep the main and supervisor device IDs unique.

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
| `API_TOKEN` | Optional API key for trusted LAN use | empty |
| `TARGET_TEMPERATURE_MIN_C` / `TARGET_TEMPERATURE_MAX_C` | Dashboard temperature target band | `20.0` / `26.0` |
| `TARGET_HUMIDITY_MIN_PCT` / `TARGET_HUMIDITY_MAX_PCT` | Dashboard humidity target band | `60.0` / `85.0` |
| `TARGET_SOIL_MIN_PCT` / `TARGET_SOIL_MAX_PCT` | Dashboard soil-moisture target band | `35.0` / `70.0` |
| `CAMERA_ENABLED` | Enable local USB camera capture | `true` |
| `CAMERA_DEVICE` / `CAMERA_DEVICE_INDEX` | Camera path or index; `CAMERA_DEVICE` takes precedence | `0` |
| `CAMERA_WIDTH` / `CAMERA_HEIGHT` | USB camera capture resolution | `1280` / `720` |
| `CAMERA_FPS` | USB camera capture and stream frame rate | `15` |
| `CAMERA_SNAPSHOT_URL` | Optional fallback remote JPEG/PNG/WebP snapshot source | empty |
| `CAMERA_STREAM_URL` | Optional credential-free browser-compatible stream source | empty |
| `CAMERA_TIMEOUT_S` | Snapshot fetch timeout for remote camera | `3` |
| `DASHBOARD_POLL_S` | Browser dashboard refresh interval | `5` |

When `API_TOKEN` is set, API clients may send it as the `X-API-Key` header. If the token is left empty, the dashboard and API remain available on the local trusted LAN without extra key checks. This is intentionally a simple home-network setup.

## 2. MQTT contract

| Topic | Direction | Frequency | Retained | Payload role |
| --- | --- | ---: | --- | --- |
| `vivarium/main/heartbeat` | Main to broker | 2 s | No | Liveness, state, alarm, sensor status. |
| `vivarium/main/telemetry` | Main to broker | 5 s | No | Measurements, interlocks, and actuator state. |
| `vivarium/main/alarm` | Reserved main alarm topic | Not currently emitted | No | Future dedicated main alarm stream; current main alarms are included in heartbeat and telemetry. |
| `vivarium/supervisor/alarm` | Supervisor to broker | Event/repeat | No | Independent supervision alarms and `NONE` recovery events. |
| `vivarium/main/command` | Pi to main | On demand | No | Validated actuator/control command. |

Heartbeat payloads include `deviceId`, `bootId`, `sequence`, state/alarm fields, and sensor-status fields. Telemetry payloads include `deviceId`, `sequence`, state/alarm fields, measurements, interlocks, and actuator fields. Consumers must use receipt time and freshness thresholds, not retained MQTT state. Empty payloads are cleanup tombstones and all retained messages are ignored by the Pi.

## 3. Commands

The Pi API validates commands before publishing this JSON shape:

```json
{"command":"mister","value":"ON"}
```

| Command | Accepted value | Effect |
| --- | --- | --- |
| `mister` | `ON` or `OFF` | Request mister state. |
| `fogger` | `ON` or `OFF` | Request fogger state. An `ON` request is accepted only while the mister is running or during its five-second post-mist window. |
| `heater` | `ON` or `OFF` | Request heater state. |
| `fan` | Integer 0 to 255 | Request fan PWM duty. |
| `feed` | `ON` | Start a feed cycle if cooldown permits. |
| `manual` | `ON` or `OFF` | `ON` enters manual mode and disables automatic climate demand; `OFF` returns to automatic mode. Manual mode expires after 10 minutes without a manual command. |
| `alloff` | Any value accepted by API | Stop climate outputs and enter manual mode. Hard interlocks remain active. |

The main controller independently rechecks safety conditions. A successful API response means the message was published to MQTT, not that the actuator turned on.

## 4. HTTP API

The systemd service runs FastAPI through Uvicorn on `0.0.0.0:8080`. The dashboard shell, static assets, and rendered documentation are public. When `API_TOKEN` is configured, `/api/*` requests accept `X-API-Key`; otherwise the local LAN API remains open by design for a trusted home deployment.

The root path `/` serves the dashboard. Available pages are `index.html`, `notifications.html`, `history.html`, `settings.html`, `mobile.html`, `diagnostics.html`, and `events.html`. The service worker caches the static shell for offline display; cached data never enables controls.

- **Overview**: Live controller state, camera, telemetry, alarms, fluids, and actuator controls.
- **History**: Selectable 6-hour, 24-hour, and 7-day climate, fluid, actuator, and alarm history.
- **Mobile**: Touch-optimized telemetry and quick actuator controls.
- **Notifications**: Active and recovered alarms, acknowledgement, and email queue/cooldown status.
- **Settings**: Diagnostics, command audit, local display preferences, and offline-cache status.

The browser polls rather than opening a WebSocket, so stale or unavailable data is shown explicitly.

### Health

```text
GET /api/health
```

Reports MQTT connection state, latest main heartbeat and telemetry, supervisor alarm, message ages, freshness booleans, and current database health.

### Telemetry and history

```text
GET /api/telemetry?limit=200
GET /api/history?since=<unix-seconds>&until=<unix-seconds>&limit=2000
```

`/api/telemetry` returns recent SQLite rows, newest first. `/api/history` returns chronological rows with decoded JSON under `data`. The endpoints bound their limits to 5000 and 10000 respectively and reject a range where `since` is after `until`.

### Alarms, notifications, and audit

```text
GET  /api/alarms?limit=200
GET  /api/notifications?limit=100
POST /api/notifications/<alarm-id>/ack
GET  /api/audit?limit=200
GET  /api/targets
```

Alarm history is newest first. Notifications include active alarms, recent events, recovery events, email configuration, queue depth, cooldowns, and persisted acknowledgements. Acknowledgement does not clear an active alarm.

### Data export

```text
GET /api/export?format=csv&since=<unix-seconds>&until=<unix-seconds>
GET /api/export?format=json&since=<unix-seconds>&until=<unix-seconds>
```

Exports decoded telemetry with timestamps and source fields. The maximum is 10,000 records. CSV and JSON exports apply the same time-range validation as `/api/history`.

### Publish a command

```text
POST /api/command
Content-Type: application/json
X-API-Key: <token>

{"command":"fan","value":128}
```

Responses are `200` for a successful MQTT publish, `400` for invalid commands or values, `401` for an incorrect key, `503` for missing API configuration or unavailable MQTT, and `422` for malformed request bodies.

### Camera

```text
GET /api/camera/snapshot
GET /api/camera/stream
```

The snapshot endpoint returns a USB-camera frame or fetches the configured remote snapshot. It accepts JPEG, PNG, and WebP responses, applies the configured timeout and an 8 MiB size limit, and returns `404` when no source is configured or `502` when the camera is unavailable. Remote camera URLs are kept server-side.

The stream endpoint provides an MJPEG stream from the USB camera. The dashboard only exposes an external stream URL when it is an `http` or `https` URL without user info or query parameters. The Pi-side stream uses the same-origin cookie; API keys are not placed in camera URLs.

## 5. Pi storage and alerts

SQLite contains four tables:

- `telemetry(ts, source, payload)`: raw telemetry payloads.
- `alarms(ts, source, code, severity, detail, payload)`: parsed alarm metadata plus raw payload.
- `alarm_ack(alarm_id, acknowledged_at)`: notification acknowledgement state.
- `command_audit(ts, command, value, published)`: command publication attempts. This does not confirm physical actuator state.

Rows older than `TELEMETRY_RETENTION_DAYS` are purged during database writes, at most once per hour. The database uses WAL mode, normal synchronous mode, a 10-second busy timeout, and indexes for time, source, and alarm code.

Repeated alarm events remain in history, but the active-notification query exposes only the newest unrecovered event for each source and code. SQLite connections are closed after each operation; a failed transaction is rolled back.

Email delivery is asynchronous. A bounded queue, three retries, and per-code cooldown protect the service from alert storms. Missing SMTP configuration suppresses delivery and logs a warning; it does not stop telemetry storage.

Main-controller alarm transitions are derived from the `alarm` field in heartbeat/telemetry. Supervisor alarms arrive on `vivarium/supervisor/alarm`; the supervisor sends `code: "NONE"` when its active alarm clears. The Pi stores raised and `RECOVERED` events. Acknowledgement is separate from recovery and is persisted in `alarm_ack`.

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

Create `.env` before starting the service because the unit requires its `EnvironmentFile`. The unit uses a restricted service account, a private temporary directory, a read-only system, and an explicit writable database path. Keep `.env` readable only by the service account or an administrative group.

## 7. Troubleshooting

### No main heartbeat

Check main power, Wi-Fi credentials, broker address, MQTT credentials, broker ACLs, and the controller serial log. The controller continues local control while network troubleshooting is underway.

### Telemetry is present but stale

Check that the controller loop is running, its MQTT command subscription is healthy, and the broker is not supplying retained health data. Health messages must be live and non-retained; the controller clears legacy retained health topics when it connects.

### Main reports `SENSOR_FAULT`

Check upper/lower SHT4x power and channel wiring, TCA9548A selection, soil calibration, analog wiring, and sensor values against the configured validity range. Do not bypass the fault to run a heater or water actuator.

### Main reports `EXTERNAL_SENSOR_FAULT`

Check DHT11 power, pull-up, GPIO 4 wiring, and the sensor data line. Climate control remains available, but the external reference should be repaired before relying on ambient comparisons.

### Pi API returns `503`

For missing API configuration, set `API_TOKEN`. For MQTT failure, check broker reachability and credentials. The main controller does not depend on this API.

### Alerts are not arriving

Check SMTP host, port, credentials, recipient, TLS requirements, and the service journal. Confirm the alarm code has not been suppressed by `ALERT_COOLDOWN_S` and that the alert queue is not full.

### A timeout lockout remains active

The drainage lockout clears when the drainage level returns to normal. A heater timeout lockout clears only after both valid zone temperatures are at or above the heater-off threshold. This protects against immediately restarting a timed-out heater; investigate the cause before relying on automatic recovery.

### Supervisor reports `MAIN_OFFLINE`

Check main power and broker connectivity first. Health topics must be non-retained; remove any legacy retained values at the broker or reconnect the main controller so it can clear them. Restore the main controller and allow a fresh heartbeat before accepting recovery.
