import json
import logging
import threading
import time

import paho.mqtt.client as mqtt

from .alerts import send_alert, start_alert_worker, stop_alert_worker
from .config import config
from .db import add

log = logging.getLogger(__name__)
TOPIC_MAIN_HEARTBEAT = "vivarium/main/heartbeat"
TOPIC_MAIN_TELEMETRY = "vivarium/main/telemetry"
TOPIC_MAIN_ALARM = "vivarium/main/alarm"
TOPIC_SUPERVISOR_ALARM = "vivarium/supervisor/alarm"
TOPIC_COMMAND = "vivarium/main/command"
TOPICS = (TOPIC_MAIN_HEARTBEAT, TOPIC_MAIN_TELEMETRY, TOPIC_MAIN_ALARM, TOPIC_SUPERVISOR_ALARM)
MAX_PAYLOAD_BYTES = 64 * 1024
PUBLISH_TIMEOUT_S = 5


class Broker:
    def __init__(self) -> None:
        self.latest: dict[str, dict] = {}
        self.received_at: dict[str, float] = {}
        self.active_alarms: dict[str, str] = {}
        self.lock = threading.Lock()
        self.started = False
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="frog-pi-supervisor",
        )
        if config.mqtt_user:
            self.client.username_pw_set(config.mqtt_user, config.mqtt_password)
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect

    def on_connect(self, client, _userdata, _flags, reason_code, _properties=None):
        if reason_code != 0:
            log.warning("MQTT connect failed: %s", reason_code)
            return
        for topic in TOPICS:
            client.subscribe(topic, qos=1)
        log.info("MQTT connected")

    def on_disconnect(self, _client, _userdata, _disconnect_flags, reason_code, _properties=None):
        log.warning("MQTT disconnected: %s", reason_code)

    def on_message(self, _client, _userdata, msg):
        if len(msg.payload) > MAX_PAYLOAD_BYTES:
            log.warning("Dropped oversized MQTT message on %s", msg.topic)
            return
        if not msg.payload:
            log.info("Cleared retained MQTT message on %s", msg.topic)
            return
        if getattr(msg, "retain", False) is True:
            log.info("Ignored retained MQTT message on %s", msg.topic)
            return
        raw = msg.payload.decode(errors="replace")
        source = "supervisor" if msg.topic.startswith("vivarium/supervisor/") else "main"

        try:
            data = json.loads(raw)
            if not isinstance(data, dict):
                data = {"value": data}
        except json.JSONDecodeError:
            data = {"raw": raw}

        with self.lock:
            self.latest[msg.topic] = data
            self.received_at[msg.topic] = time.time()

        try:
            if msg.topic == TOPIC_MAIN_TELEMETRY:
                try:
                    add("telemetry", source, raw)
                except Exception:
                    log.exception("Failed to store telemetry from %s", msg.topic)
                self._record_main_alarm_transition(data, raw)
            elif msg.topic in (TOPIC_MAIN_ALARM, TOPIC_SUPERVISOR_ALARM):
                self._record_alarm_transition(source, data, raw)
        except Exception:
            log.exception("Failed to process MQTT message on %s", msg.topic)

    def _record_main_alarm_transition(self, data: dict, payload: str) -> None:
        code = str(data.get("alarm", "NONE"))
        previous = self.active_alarms.get("main")
        if code != "NONE" and code != previous:
            if previous:
                add("alarms", "main", payload, "RECOVERED", "INFO", f"{previous} cleared")
            self.active_alarms["main"] = code
            add("alarms", "main", payload, code, "CRITICAL", "Main controller alarm")
            send_alert(code, "Main controller alarm", payload)
        elif code == "NONE" and previous:
            self.active_alarms.pop("main", None)
            add("alarms", "main", payload, "RECOVERED", "INFO", f"{previous} cleared")

    def _record_alarm_transition(self, source: str, data: dict, payload: str) -> None:
        code = str(data.get("code", "UNKNOWN"))
        severity = str(data.get("severity", "CRITICAL"))
        detail = str(data.get("detail", ""))
        previous = self.active_alarms.get(source)
        if code == "NONE":
            if previous:
                self.active_alarms.pop(source, None)
                add("alarms", source, payload, "RECOVERED", "INFO", f"{previous} cleared")
            return
        if previous and code != previous:
            add("alarms", source, payload, "RECOVERED", "INFO", f"{previous} cleared")
        add("alarms", source, payload, code, severity, detail)
        if code != previous:
            self.active_alarms[source] = code
            send_alert(code, detail, payload)

    def start(self) -> None:
        start_alert_worker()
        self.started = True
        try:
            self.client.connect_async(config.mqtt_host, config.mqtt_port, 60)
            self.client.loop_start()
            log.info("MQTT client started for %s:%s", config.mqtt_host, config.mqtt_port)
        except Exception:
            self.started = False
            log.exception("Failed to start MQTT client")

    def stop(self) -> None:
        self.started = False
        self.client.loop_stop()
        if self.client.is_connected():
            self.client.disconnect()
        stop_alert_worker()

    def connected(self) -> bool:
        return self.client.is_connected()

    def snapshot(self) -> dict[str, dict]:
        with self.lock:
            return dict(self.latest)

    def ages(self) -> dict[str, float]:
        now = time.time()
        with self.lock:
            return {topic: max(0.0, now - received) for topic, received in self.received_at.items()}

    def command(self, command: str, value: str) -> bool:
        if not self.client.is_connected():
            return False
        payload = json.dumps({"command": command, "value": value})
        info = self.client.publish(TOPIC_COMMAND, payload, qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            return False
        try:
            info.wait_for_publish(timeout=PUBLISH_TIMEOUT_S)
        except (RuntimeError, ValueError):
            return False
        return info.is_published()
