import math
import os
from dataclasses import dataclass
from dotenv import load_dotenv

load_dotenv()


def _int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc


def _float(name: str, default: float) -> float:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be a number") from exc


@dataclass(frozen=True)
class Config:
    mqtt_host: str = os.getenv("MQTT_HOST", "localhost")
    mqtt_port: int = _int("MQTT_PORT", 1883)
    mqtt_user: str = os.getenv("MQTT_USER", "")
    mqtt_password: str = os.getenv("MQTT_PASSWORD", "")
    db_path: str = os.getenv("DB_PATH", "/var/lib/frog-terrarium/terrarium.db")
    alert_email: str = os.getenv("ALERT_EMAIL", "")
    smtp_host: str = os.getenv("SMTP_HOST", "")
    smtp_port: int = _int("SMTP_PORT", 587)
    smtp_user: str = os.getenv("SMTP_USER", "")
    smtp_password: str = os.getenv("SMTP_PASSWORD", "")
    camera_snapshot_url: str = os.getenv("CAMERA_SNAPSHOT_URL", "")
    camera_stream_url: str = os.getenv("CAMERA_STREAM_URL", "")
    camera_timeout_s: float = _float("CAMERA_TIMEOUT_S", 3.0)
    dashboard_poll_s: int = _int("DASHBOARD_POLL_S", 5)
    alert_cooldown_s: int = _int("ALERT_COOLDOWN_S", 900)
    telemetry_retention_days: int = _int("TELEMETRY_RETENTION_DAYS", 30)
    heartbeat_timeout_s: int = _int("HEARTBEAT_TIMEOUT_S", 15)
    telemetry_timeout_s: int = _int("TELEMETRY_TIMEOUT_S", 15)
    api_token: str = os.getenv("API_TOKEN", "")
    target_temperature_min_c: float = _float("TARGET_TEMPERATURE_MIN_C", 20.0)
    target_temperature_max_c: float = _float("TARGET_TEMPERATURE_MAX_C", 26.0)
    target_humidity_min_pct: float = _float("TARGET_HUMIDITY_MIN_PCT", 60.0)
    target_humidity_max_pct: float = _float("TARGET_HUMIDITY_MAX_PCT", 85.0)
    target_soil_min_pct: float = _float("TARGET_SOIL_MIN_PCT", 35.0)
    target_soil_max_pct: float = _float("TARGET_SOIL_MAX_PCT", 70.0)

    def __post_init__(self) -> None:
        if not self.mqtt_host.strip():
            raise ValueError("MQTT_HOST must not be empty")
        if not 1 <= self.mqtt_port <= 65535:
            raise ValueError("MQTT_PORT must be between 1 and 65535")
        if not 1 <= self.smtp_port <= 65535:
            raise ValueError("SMTP_PORT must be between 1 and 65535")
        if self.alert_cooldown_s < 0:
            raise ValueError("ALERT_COOLDOWN_S must not be negative")
        if self.telemetry_retention_days < 1:
            raise ValueError("TELEMETRY_RETENTION_DAYS must be at least 1")
        if self.heartbeat_timeout_s < 1:
            raise ValueError("HEARTBEAT_TIMEOUT_S must be at least 1")
        if self.telemetry_timeout_s < 1:
            raise ValueError("TELEMETRY_TIMEOUT_S must be at least 1")
        if not math.isfinite(self.camera_timeout_s) or self.camera_timeout_s <= 0:
            raise ValueError("CAMERA_TIMEOUT_S must be greater than 0")
        if self.dashboard_poll_s < 1:
            raise ValueError("DASHBOARD_POLL_S must be at least 1")


config = Config()
