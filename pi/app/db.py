import os
import sqlite3
import threading
import time
from contextlib import contextmanager

from .config import config

_lock = threading.Lock()
_last_purge = 0.0
_last_error: str | None = None
_last_success = 0.0
PURGE_INTERVAL_S = 3600


@contextmanager
def _connect():
    conn = sqlite3.connect(config.db_path, timeout=10)
    try:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute("PRAGMA busy_timeout=10000")
        yield conn
    except Exception:
        conn.rollback()
        raise
    else:
        conn.commit()
    finally:
        conn.close()


def _record_success() -> None:
    global _last_error, _last_success
    with _lock:
        _last_error = None
        _last_success = time.time()


def _record_error(exc: Exception) -> None:
    global _last_error
    with _lock:
        _last_error = f"{type(exc).__name__}: {exc}"


def _purge_if_due(conn: sqlite3.Connection, now: float) -> None:
    global _last_purge
    if now - _last_purge < PURGE_INTERVAL_S:
        return
    telemetry_cutoff = now - max(1, config.telemetry_retention_days) * 86400
    alarm_cutoff = now - max(1, config.alarm_retention_days) * 86400
    conn.execute("DELETE FROM telemetry WHERE ts < ?", (telemetry_cutoff,))
    conn.execute("DELETE FROM alarms WHERE ts < ?", (alarm_cutoff,))
    conn.execute("DELETE FROM command_audit WHERE ts < ?", (alarm_cutoff,))
    _last_purge = now


def init() -> None:
    directory = os.path.dirname(config.db_path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    try:
        with _connect() as conn:
            conn.execute(
                "CREATE TABLE IF NOT EXISTS telemetry (id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL, source TEXT NOT NULL, payload TEXT NOT NULL)"
            )
            conn.execute(
                "CREATE TABLE IF NOT EXISTS alarms (id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL, source TEXT NOT NULL, code TEXT NOT NULL, severity TEXT NOT NULL, detail TEXT, payload TEXT NOT NULL)"
            )
            conn.execute(
                "CREATE TABLE IF NOT EXISTS command_audit (id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL, command TEXT NOT NULL, value TEXT NOT NULL, published INTEGER NOT NULL)"
            )
            conn.execute(
                "CREATE TABLE IF NOT EXISTS alarm_ack (alarm_id INTEGER PRIMARY KEY, acknowledged_at REAL NOT NULL)"
            )
            conn.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry(ts)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_source ON telemetry(source)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_alarms_ts ON alarms(ts)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_alarms_code ON alarms(code)")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_command_audit_ts ON command_audit(ts)")
        _record_success()
    except Exception as exc:
        _record_error(exc)
        raise


def add(table: str, source: str, payload: str, code: str | None = None,
        severity: str | None = None, detail: str | None = None) -> None:
    now = time.time()
    try:
        with _lock, _connect() as conn:
            _purge_if_due(conn, now)
            if table == "telemetry":
                conn.execute(
                    "INSERT INTO telemetry(ts,source,payload) VALUES(?,?,?)",
                    (now, source, payload),
                )
            elif table == "alarms":
                conn.execute(
                    "INSERT INTO alarms(ts,source,code,severity,detail,payload) VALUES(?,?,?,?,?,?)",
                    (now, source, code or "UNKNOWN", severity or "CRITICAL", detail or "", payload),
                )
            else:
                raise ValueError(f"unsupported table: {table}")
        _record_success()
    except Exception as exc:
        _record_error(exc)
        raise


def health() -> dict[str, object]:
    try:
        with _connect() as conn:
            conn.execute("SELECT 1").fetchone()
        _record_success()
    except Exception as exc:
        _record_error(exc)
    with _lock:
        return {
            "ok": _last_error is None,
            "lastError": _last_error,
            "lastSuccessAt": _last_success or None,
        }


def recent(limit: int = 100) -> list[tuple[float, str, str]]:
    limit = max(1, min(limit, 5000))
    with _connect() as conn:
        return conn.execute(
            "SELECT ts,source,payload FROM telemetry ORDER BY id DESC LIMIT ?",
            (limit,),
        ).fetchall()


def recent_alarms(limit: int = 100) -> list[tuple[int, float, str, str, str, str | None, bool]]:
    limit = max(1, min(limit, 5000))
    with _connect() as conn:
        return conn.execute(
            "SELECT alarms.id, alarms.ts, alarms.source, alarms.code, alarms.severity, alarms.detail, alarm_ack.alarm_id IS NOT NULL FROM alarms LEFT JOIN alarm_ack ON alarm_ack.alarm_id = alarms.id ORDER BY alarms.id DESC LIMIT ?",
            (limit,),
        ).fetchall()


def active_alarms() -> list[tuple[int, float, str, str, str, str | None, bool]]:
    with _connect() as conn:
        return conn.execute(
            """
            SELECT alarms.id, alarms.ts, alarms.source, alarms.code, alarms.severity,
                   alarms.detail, alarm_ack.alarm_id IS NOT NULL
            FROM alarms
            LEFT JOIN alarm_ack ON alarm_ack.alarm_id = alarms.id
            WHERE alarms.code != 'RECOVERED'
              AND alarm_ack.alarm_id IS NULL
              AND NOT EXISTS (
                  SELECT 1 FROM alarms recovery
                  WHERE recovery.source = alarms.source
                    AND recovery.code = 'RECOVERED'
                                        AND recovery.id > alarms.id
                    AND recovery.detail LIKE alarms.code || ' cleared%'
              )
                            AND NOT EXISTS (
                                    SELECT 1 FROM alarms newer
                                    WHERE newer.source = alarms.source
                                        AND newer.code = alarms.code
                                        AND newer.id > alarms.id
                            )
            ORDER BY alarms.id DESC
            """
        ).fetchall()


def telemetry_history(since: float | None, until: float | None, limit: int = 2000) -> list[tuple[float, str, str]]:
    limit = max(1, min(limit, 10000))
    clauses = []
    params: list[float | int] = []
    if since is not None:
        clauses.append("ts >= ?")
        params.append(since)
    if until is not None:
        clauses.append("ts <= ?")
        params.append(until)
    where = f"WHERE {' AND '.join(clauses)}" if clauses else ""
    params.append(limit)
    with _connect() as conn:
        return conn.execute(
            f"SELECT ts,source,payload FROM telemetry {where} ORDER BY ts DESC, id DESC LIMIT ?",
            params,
        ).fetchall()[::-1]


def audit_command(command: str, value: str, published: bool) -> None:
    now = time.time()
    try:
        with _lock, _connect() as conn:
            conn.execute(
                "INSERT INTO command_audit(ts,command,value,published) VALUES(?,?,?,?)",
                (now, command, value, int(published)),
            )
        _record_success()
    except Exception as exc:
        _record_error(exc)
        raise


def recent_commands(limit: int = 200) -> list[tuple[float, str, str, bool]]:
    limit = max(1, min(limit, 5000))
    with _connect() as conn:
        return conn.execute(
            "SELECT ts,command,value,published FROM command_audit ORDER BY id DESC LIMIT ?",
            (limit,),
        ).fetchall()


def acknowledge_alarm(alarm_id: int) -> bool:
    try:
        with _lock, _connect() as conn:
            exists = conn.execute("SELECT 1 FROM alarms WHERE id = ?", (alarm_id,)).fetchone()
            if not exists:
                return False
            conn.execute(
                "INSERT OR IGNORE INTO alarm_ack(alarm_id, acknowledged_at) VALUES(?,?)",
                (alarm_id, time.time()),
            )
        _record_success()
        return True
    except Exception as exc:
        _record_error(exc)
        raise


