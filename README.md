# Libby's Vivarium

A local-first monitoring and climate-control system for a frog vivarium. It combines two ESP32 devices and a Raspberry Pi to sense the enclosure, control its climate, watch for failures, record history, and provide a small LAN dashboard.

The system is designed around one rule: **safety stays local**. The main controller keeps enforcing sensor checks and actuator interlocks even when Wi-Fi, MQTT, the Raspberry Pi, or the independent supervisor is unavailable.

## What it does

- Measures upper and lower temperature and humidity, ambient conditions, soil moisture, water level, drainage level, and door state.
- Controls misting, fogging, heating, ventilation, drainage, feeding, and a local alarm.
- Applies local safeguards for bad or stale sensors, an open door, low water, high drainage, overheating, maximum runtimes, cooldowns, and manual-control expiry.
- Shows an independent supervisory view on a CYD touchscreen with local buzzer alarms when the main controller is offline, stale, or reporting suspicious conditions.
- Stores MQTT telemetry and alarm history in SQLite and optionally sends email alerts.
- Provides a responsive local dashboard with live state, history, notifications, diagnostics, optional camera views, and validated control requests.

## Architecture

```text
 Sensors and interlocks
			 |
			 v
 Main ESP32-S3 ---- MQTT broker ---- Raspberry Pi
	 |       \                         |  |  \
	 |        \---- MQTT ------------ CYD  DB  API/email
	 v
 Pumps, fan, heater, drainage pump, feeder servo, alarm
```

The main ESP32-S3 is the only climate-control authority. The supervisor observes and raises alarms but never commands actuators. The Pi provides storage, alerts, dashboard access, and command forwarding; it cannot bypass the main controller's firmware safety rules.

## Repository layout

| Path | Purpose |
| --- | --- |
| `controller/` | Main ESP32-S3 firmware for sensors, climate logic, interlocks, and actuators. |
| `supervisor/` | Independent ESP32-CYD firmware for cross-checking, alarms, display, and touch. |
| `pi/app/` | Raspberry Pi FastAPI service, MQTT client, SQLite storage, camera handling, and email alerts. |
| `pi/app/static/` | Browser dashboard pages, styles, and client-side behavior. |
| `pi/tests/` | Host-side Python contract and MQTT tests. |
| `docs/` | System architecture, operations, and commissioning documentation. |

## Getting started

1. Install PlatformIO for the controller and supervisor firmware, and install Python 3 with the packages in `pi/requirements.txt`.
2. Configure private Wi-Fi and MQTT values in local copies of `controller/include/config.h` and `supervisor/include/config.h`. The checked-in headers intentionally contain placeholders.
3. Copy `pi/.env.example` to `.env` on the Raspberry Pi and configure the broker, database, alerts, retention, and optional camera settings.
4. Build and flash both firmware projects. Commission the controller with actuators disconnected before connecting live loads.
5. Run the Pi service and open its dashboard at `http://<pi-address>:8080/`.

The API token is optional for a trusted home LAN. If `API_TOKEN` is set, the Pi API and dashboard data requests require `X-API-Key`; leaving it empty keeps the setup simple for local use.

## Documentation

- [Architecture](docs/ARCHITECTURE.md): authority boundaries, hardware, control logic, MQTT topics, supervision, and failure behavior.
- [Operations and API](docs/OPERATIONS.md): configuration, deployment, API endpoints, commands, storage, alerts, and troubleshooting.
- [Commissioning](docs/COMMISSIONING.md): staged wiring, first power-up, safety checks, network integration, and acceptance tests.

## Hardware and safety

Use properly rated drivers, relays or SSRs, flyback protection, separate servo power, and an independent thermal cutoff for the heater. Firmware safeguards reduce risk but do not replace suitable electrical, mechanical, and thermal protection.

## Release state

This repository contains the v7 implementation. Firmware builds have been verified in the development environment; physical installation, flashing, and live-load validation must still be completed using the commissioning guide.
