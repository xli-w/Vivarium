# Vivarium

TERRA is a three-device monitoring, supervision, and local-control system for a vivarium. The system is intentionally layered: the main ESP32-S3 keeps the enclosure safe and climate control local, the independent ESP32 supervisor checks the main controller, and the Raspberry Pi provides storage, alerting, and a protected LAN API.

## System at a glance

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

The main controller remains functional when the Pi, broker, or supervisor is unavailable. The Pi and supervisor never replace the main controller as the climate safety authority.

## Documents

- [Architecture](docs/ARCHITECTURE.md): system boundaries, hardware, control logic, MQTT contracts, and failure behavior.
- [Operations and API](docs/OPERATIONS.md): configuration, deployment, endpoints, commands, database, alerts, and troubleshooting.
- [Commissioning](docs/COMMISSIONING.md): staged wiring, first power-up, safety checks, network integration, and acceptance tests.

The Raspberry Pi also hosts the first TERRA web dashboard at `/`. It is an authenticated operational view of the existing MQTT and database state, with camera display, alarm history, chart-ready telemetry, and guarded command requests. It does not replace the main controller or bypass firmware interlocks.

## Repository layout

| Path | Purpose |
| --- | --- |
| `controller/` | Main ESP32-S3 firmware for sensors, climate logic, interlocks, and actuators. |
| `supervisor/` | Independent ESP32-CYD firmware for cross-checking, alarms, display, and touch. |
| `pi/app/` | Raspberry Pi FastAPI service, MQTT client, SQLite persistence, and email alert worker. |
| `pi/tests/` | Host-side Python contract and MQTT tests. |
| `docs/` | System architecture, operations, and commissioning documentation. |

## Prerequisites

- An MQTT broker reachable by all three networked devices.
- Wi-Fi credentials and MQTT credentials configured separately for the controller and supervisor.
- A Raspberry Pi running Python with the packages in `pi/requirements.txt`.
- Properly rated drivers, relays or SSRs, flyback protection, heater thermal protection, and separate servo power.
- PlatformIO and the required board libraries for firmware builds and flashing. Firmware flashing and physical validation have not been performed in this environment.

## Release state

This repository is the v7 release set. Before live use, follow [Commissioning](docs/COMMISSIONING.md) completely. In particular, leave the supervisor in display-test mode only for its isolated bench test; set `DISPLAY_TEST_MODE=false` before connecting it to the live system.
