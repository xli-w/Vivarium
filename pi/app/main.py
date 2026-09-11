from contextlib import asynccontextmanager
import json
import html
import csv
import io
import logging
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from urllib.parse import urlsplit

from fastapi import Cookie, Depends, FastAPI, Header, HTTPException, Query
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, StrictInt, StrictStr

from .config import config
from .alerts import status as alert_status
from .camera import usb_camera
from .db import acknowledge_alarm, active_alarms, audit_command
from .db import health as database_health
from .db import init, recent, recent_alarms, recent_commands, telemetry_history
from .mqtt import TOPIC_MAIN_HEARTBEAT, TOPIC_MAIN_TELEMETRY, TOPIC_SUPERVISOR_ALARM, Broker

log = logging.getLogger(__name__)

ALLOWED_COMMANDS = {"mister", "fogger", "heater", "fan", "feed", "manual", "alloff"}
SWITCH_COMMANDS = {"mister", "fogger", "heater", "manual"}
DOCUMENTATION = {
    "architecture": "ARCHITECTURE.md",
    "commissioning": "COMMISSIONING.md",
    "operations": "OPERATIONS.md",
}

broker = Broker()


@asynccontextmanager
async def lifespan(_app: FastAPI):
    init()
    broker.start()
    try:
        yield
    finally:
        usb_camera.release()
        broker.stop()


app = FastAPI(title="Frog Vivarium Supervisor", version="7.0", lifespan=lifespan)


def _decode_payload(payload: str) -> dict:
    try:
        decoded = json.loads(payload)
    except json.JSONDecodeError:
        return {"raw": payload}
    return decoded if isinstance(decoded, dict) else {"value": decoded}


def _camera_metadata() -> dict:
    stream_parts = urlsplit(config.camera_stream_url) if config.camera_stream_url else None
    public_stream = bool(
        config.camera_stream_url
        and stream_parts
        and not stream_parts.username
        and not stream_parts.password
        and not stream_parts.query
        and stream_parts.scheme in {"http", "https"}
    )
    snapshot_available = config.camera_enabled or bool(config.camera_snapshot_url)
    stream_available = public_stream or config.camera_enabled
    stream_url = (
        config.camera_stream_url
        if public_stream
        else ("/api/camera/stream" if config.camera_enabled else None)
    )
    return {
        "snapshotAvailable": snapshot_available,
        "streamAvailable": stream_available,
        "snapshotUrl": "/api/camera/snapshot" if snapshot_available else None,
        "streamUrl": stream_url,
    }


class Command(BaseModel):
    command: str = Field(min_length=1, max_length=32)
    value: StrictStr | StrictInt = Field(...)


def normalize_command(command: Command) -> str:
    if command.command not in ALLOWED_COMMANDS:
        raise HTTPException(400, "unsupported command")
    if command.command == "fan":
        if not isinstance(command.value, int) or isinstance(command.value, bool):
            raise HTTPException(400, "fan value must be 0-255")
        if command.value < 0 or command.value > 255:
            raise HTTPException(400, "fan value must be 0-255")
        return str(command.value)
    if command.command in SWITCH_COMMANDS:
        if not isinstance(command.value, str) or command.value.upper() not in {"ON", "OFF"}:
            raise HTTPException(400, "command value must be ON or OFF")
        return command.value.upper()
    if command.command == "feed":
        if not isinstance(command.value, str) or command.value.upper() != "ON":
            raise HTTPException(400, "feed value must be ON")
        return "ON"
    return "OFF"


def require_token(
    x_api_key: str | None = Header(default=None),
    cookie_token: str | None = Cookie(default=None, alias="terra_api_key"),
) -> None:
    if not config.api_token:
        raise HTTPException(503, "API token not configured")
    # When called directly in unit tests without FastAPI dependency injection,
    # omitted parameters default to their Header/Cookie marker objects.
    provided_key = next((value for value in (x_api_key, cookie_token) if isinstance(value, str)), None)
    if provided_key != config.api_token:
        raise HTTPException(401, "invalid API token")


def _alarm_dict(row: tuple) -> dict:
    alarm_id, timestamp, source, code, severity, detail, acknowledged = row
    return {
        "id": alarm_id,
        "timestamp": timestamp,
        "source": source,
        "code": code,
        "severity": severity,
        "detail": detail or "",
        "acknowledged": bool(acknowledged),
    }


def _target_status(value: float | int | None, low: float, high: float) -> str:
    if value is None:
        return "unknown"
    value = float(value)
    margin = max((high - low) * 0.15, 1.0)
    if low <= value <= high:
        return "within"
    if low - margin <= value <= high + margin:
        return "approaching"
    return "out"


@app.get("/documentation/{document_name}", response_class=HTMLResponse)
def documentation(document_name: str) -> HTMLResponse:
    filename = DOCUMENTATION.get(document_name)
    if filename is None:
        raise HTTPException(404, "documentation page not found")
    document_path = Path(__file__).parents[2] / "docs" / filename
    if not document_path.is_file():
        raise HTTPException(404, "documentation page not installed")
    content = html.escape(document_path.read_text(encoding="utf-8"))
    title = html.escape(document_path.stem.title())
    document_links = " | ".join(
        f'<a href="/documentation/{name}">{label}</a>'
        for name, label in (
            ("architecture", "Architecture"),
            ("operations", "Operations"),
            ("commissioning", "Commissioning"),
        )
    )
    return HTMLResponse(
        f"""<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>TERRA - {title}</title>
<style>body{{margin:0;background:#f4f1e8;color:#20251f;font:16px/1.6 system-ui,sans-serif}}main{{max-width:960px;margin:0 auto;padding:32px 20px}}a{{color:#176b5c}}pre{{white-space:pre-wrap;background:#fffdf7;border:1px solid #d7d1c3;padding:24px;overflow:auto}}</style>
</head><body><main><p><a href="/">Back to dashboard</a></p><nav aria-label="Documentation">{document_links}</nav><h1>{title}</h1><pre>{content}</pre></main></body></html>"""
    )


@app.get("/api/health")
def health(_auth: None = Depends(require_token)) -> dict:
    latest = broker.snapshot()
    ages = broker.ages()
    heartbeat_age = ages.get(TOPIC_MAIN_HEARTBEAT)
    telemetry_age = ages.get(TOPIC_MAIN_TELEMETRY)
    return {
        "mqttConnected": broker.connected(),
        "mainHeartbeat": latest.get(TOPIC_MAIN_HEARTBEAT),
        "mainTelemetry": latest.get(TOPIC_MAIN_TELEMETRY),
        "supervisorAlarm": latest.get(TOPIC_SUPERVISOR_ALARM),
        "mainHeartbeatAgeSeconds": heartbeat_age,
        "mainTelemetryAgeSeconds": telemetry_age,
        "mainHeartbeatFresh": heartbeat_age is not None and heartbeat_age <= config.heartbeat_timeout_s,
        "mainTelemetryFresh": telemetry_age is not None and telemetry_age <= config.telemetry_timeout_s,
        "database": database_health(),
    }


@app.get("/api/dashboard")
def dashboard(_auth: None = Depends(require_token)) -> dict:
    latest = broker.snapshot()
    ages = broker.ages()
    return {
        "health": health(),
        "heartbeat": latest.get(TOPIC_MAIN_HEARTBEAT),
        "telemetry": latest.get(TOPIC_MAIN_TELEMETRY),
        "supervisorAlarm": latest.get(TOPIC_SUPERVISOR_ALARM),
        "camera": _camera_metadata(),
        "pollSeconds": config.dashboard_poll_s,
        "ages": ages,
    }


@app.get("/api/telemetry")
def telemetry(limit: int = Query(default=200, ge=1, le=5000), _auth: None = Depends(require_token)) -> list:
    return recent(limit)


@app.get("/api/history")
def history(
    since: float | None = Query(default=None, ge=0),
    until: float | None = Query(default=None, ge=0),
    limit: int = Query(default=2000, ge=1, le=10000),
    _auth: None = Depends(require_token),
) -> list[dict]:
    if since is not None and until is not None and since > until:
        raise HTTPException(400, "since must not be after until")
    return [
        {"ts": ts, "source": source, "data": _decode_payload(payload)}
        for ts, source, payload in telemetry_history(since, until, limit)
    ]


@app.get("/api/alarms")
def alarms(limit: int = Query(default=200, ge=1, le=5000), _auth: None = Depends(require_token)) -> list:
    return [_alarm_dict(row) for row in recent_alarms(limit)]


@app.get("/api/notifications")
def notifications(limit: int = Query(default=100, ge=1, le=5000), _auth: None = Depends(require_token)) -> dict:
    events = [_alarm_dict(row) for row in recent_alarms(limit)]
    active = [_alarm_dict(row) for row in active_alarms()]
    return {
        "active": active,
        "events": events,
        "email": alert_status(),
    }


@app.post("/api/notifications/{alarm_id}/ack")
def acknowledge(alarm_id: int, _auth: None = Depends(require_token)) -> dict:
    if not acknowledge_alarm(alarm_id):
        raise HTTPException(404, "alarm not found")
    return {"ok": True, "alarmId": alarm_id}


@app.get("/api/audit")
def audit(limit: int = Query(default=200, ge=1, le=5000), _auth: None = Depends(require_token)) -> list[dict]:
    return [
        {"timestamp": timestamp, "command": command_name, "value": value, "published": bool(published)}
        for timestamp, command_name, value, published in recent_commands(limit)
    ]


@app.get("/api/targets")
def targets(_auth: None = Depends(require_token)) -> dict:
    telemetry_data = broker.snapshot().get(TOPIC_MAIN_TELEMETRY, {})
    bands = {
        "temperature": {"min": config.target_temperature_min_c, "max": config.target_temperature_max_c, "unit": "C", "values": {}},
        "humidity": {"min": config.target_humidity_min_pct, "max": config.target_humidity_max_pct, "unit": "%", "values": {}},
        "soil": {"min": config.target_soil_min_pct, "max": config.target_soil_max_pct, "unit": "%", "values": {}},
    }
    for zone in ("external", "upper", "lower"):
        value = telemetry_data.get(f"{zone}TemperatureC")
        bands["temperature"]["values"][zone] = {"value": value, "status": _target_status(value, bands["temperature"]["min"], bands["temperature"]["max"])}
    for zone in ("upper", "lower"):
        value = telemetry_data.get(f"{zone}HumidityPct")
        bands["humidity"]["values"][zone] = {"value": value, "status": _target_status(value, bands["humidity"]["min"], bands["humidity"]["max"])}
    value = telemetry_data.get("soilMoisturePct")
    bands["soil"]["values"]["soil"] = {"value": value, "status": _target_status(value, bands["soil"]["min"], bands["soil"]["max"])}
    return bands


@app.get("/api/export")
def export_data(
    format: str = Query(default="json", pattern="^(json|csv)$"),
    since: float | None = Query(default=None, ge=0),
    until: float | None = Query(default=None, ge=0),
    limit: int = Query(default=10000, ge=1, le=10000),
    _auth: None = Depends(require_token),
) -> Response:
    if since is not None and until is not None and since > until:
        raise HTTPException(400, "since must not be after until")
    rows = [{"ts": timestamp, "source": source, "data": _decode_payload(payload)} for timestamp, source, payload in telemetry_history(since, until, limit)]
    if format == "json":
        return Response(content=json.dumps(rows), media_type="application/json", headers={"Content-Disposition": "attachment; filename=terra-telemetry.json"})
    output = io.StringIO()
    fieldnames = ["ts", "source", "data"]
    writer = csv.DictWriter(output, fieldnames=fieldnames)
    writer.writeheader()
    for row in rows:
        writer.writerow({**row, "data": json.dumps(row["data"], separators=(",", ":"))})
    return Response(content=output.getvalue(), media_type="text/csv", headers={"Content-Disposition": "attachment; filename=terra-telemetry.csv"})


@app.post("/api/command")
def command(c: Command, _auth: None = Depends(require_token)) -> dict:
    value = normalize_command(c)
    published = broker.command(c.command, value)
    try:
        audit_command(c.command, value, published)
    except Exception:
        log.exception("Failed to audit command %s", c.command)
    if not published:
        raise HTTPException(503, "MQTT broker unavailable")
    return {"ok": True, "command": c.command, "value": value}


@app.get("/api/camera/snapshot")
def camera_snapshot(_auth: None = Depends(require_token)) -> Response:
    if config.camera_enabled:
        frame_bytes = usb_camera.get_snapshot()
        if frame_bytes is not None:
            return Response(content=frame_bytes, media_type="image/jpeg", headers={"Cache-Control": "no-store"})
        if not config.camera_snapshot_url:
            raise HTTPException(502, "USB camera snapshot unavailable")

    if not config.camera_snapshot_url:
        raise HTTPException(404, "camera snapshot is not configured")
    try:
        request = Request(config.camera_snapshot_url, headers={"User-Agent": "TERRA-dashboard/7"})
        with urlopen(request, timeout=config.camera_timeout_s) as upstream:
            content_type = upstream.headers.get_content_type()
            if content_type not in {"image/jpeg", "image/png", "image/webp"}:
                raise HTTPException(502, "camera returned an unsupported image type")
            body = upstream.read(8 * 1024 * 1024 + 1)
    except HTTPError as exc:
        raise HTTPException(502, f"camera returned HTTP {exc.code}") from exc
    except (URLError, TimeoutError, OSError) as exc:
        raise HTTPException(502, "camera snapshot unavailable") from exc
    if len(body) > 8 * 1024 * 1024:
        raise HTTPException(502, "camera snapshot is too large")
    return Response(content=body, media_type=content_type, headers={"Cache-Control": "no-store"})


@app.get("/api/camera/stream")
def camera_stream(_auth: None = Depends(require_token)):
    if not config.camera_enabled:
        raise HTTPException(404, "USB camera stream is not enabled")

    def _mjpeg_generator():
        interval = 1.0 / max(1, config.camera_fps)
        consecutive_failures = 0
        while True:
            start_time = time.time()
            frame = usb_camera.get_snapshot()
            if frame:
                consecutive_failures = 0
                yield (
                    b"--frame\r\n"
                    b"Content-Type: image/jpeg\r\n\r\n" + frame + b"\r\n"
                )
            else:
                consecutive_failures += 1
                if consecutive_failures > 60:
                    break
            elapsed = time.time() - start_time
            sleep_time = max(0.01, interval - elapsed)
            time.sleep(sleep_time)

    return StreamingResponse(
        _mjpeg_generator(),
        media_type="multipart/x-mixed-replace; boundary=frame",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "Expires": "0",
        },
    )


app.mount("/", StaticFiles(directory=Path(__file__).parent / "static", html=True), name="static")


