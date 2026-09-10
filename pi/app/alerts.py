import logging
import queue
import smtplib
import ssl
import threading
from time import sleep
from email.message import EmailMessage
from time import time

from .config import config

log = logging.getLogger(__name__)
_last_sent: dict[str, float] = {}
_queue: queue.Queue[tuple[str, str, str, int] | None] = queue.Queue(maxsize=256)
_worker_started = False
_worker_lock = threading.Lock()
_worker_thread: threading.Thread | None = None
MAX_RETRIES = 3


def status() -> dict:
    now = time()
    return {
        "configured": all([config.alert_email, config.smtp_host, config.smtp_user, config.smtp_password]),
        "cooldownSeconds": config.alert_cooldown_s,
        "activeCooldowns": {
            code: max(0, int(config.alert_cooldown_s - (now - sent)))
            for code, sent in _last_sent.items()
            if now - sent < config.alert_cooldown_s
        },
        "queueDepth": _queue.qsize(),
    }


def _within_cooldown(code: str, now: float) -> bool:
    return now - _last_sent.get(code, 0.0) < config.alert_cooldown_s


def _send_alert_now(code: str, detail: str, payload: str) -> bool:
    now = time()
    if _within_cooldown(code, now):
        return False

    required = [config.alert_email, config.smtp_host, config.smtp_user, config.smtp_password]
    if not all(required):
        log.warning("Alert email not configured; suppressing delivery of %s", code)
        return False

    msg = EmailMessage()
    msg["Subject"] = f"Frog Terrarium ALERT: {code}"
    msg["From"] = config.smtp_user
    msg["To"] = config.alert_email
    msg.set_content(f"{code}\n\n{detail}\n\nPayload:\n{payload}")

    with smtplib.SMTP(config.smtp_host, config.smtp_port, timeout=20) as smtp:
        smtp.starttls(context=ssl.create_default_context())
        smtp.login(config.smtp_user, config.smtp_password)
        smtp.send_message(msg)

    _last_sent[code] = now
    return True


def _worker() -> None:
    while True:
        item = _queue.get()
        if item is None:
            _queue.task_done()
            return
        code, detail, payload, attempt = item
        try:
            _send_alert_now(code, detail, payload)
        except Exception:
            log.exception("Failed to send alert email for %s", code)
            if attempt < MAX_RETRIES:
                sleep(min(2 ** attempt, 30))
                try:
                    _queue.put_nowait((code, detail, payload, attempt + 1))
                except queue.Full:
                    log.error("Alert retry queue full; dropped alert %s", code)
        finally:
            _queue.task_done()


def start_alert_worker() -> None:
    global _worker_started, _worker_thread
    with _worker_lock:
        if _worker_started:
            return
        _worker_thread = threading.Thread(target=_worker, name="alert-worker", daemon=True)
        _worker_thread.start()
        _worker_started = True


def stop_alert_worker() -> None:
    global _worker_started, _worker_thread
    with _worker_lock:
        if not _worker_started:
            return
        try:
            _queue.put_nowait(None)
        except queue.Full:
            log.warning("Alert queue full; worker will stop after queued alerts")
            return
        worker = _worker_thread
    if worker is not None:
        worker.join(timeout=5)
    with _worker_lock:
        if worker is None or not worker.is_alive():
            _worker_started = False
            _worker_thread = None


def send_alert(code: str, detail: str, payload: str) -> bool:
    try:
        _queue.put_nowait((code, detail, payload, 0))
        return True
    except queue.Full:
        log.error("Alert queue full; dropped alert %s", code)
        return False
