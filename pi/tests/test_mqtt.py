import threading
import unittest
from unittest.mock import Mock, patch

from app.mqtt import MAX_PAYLOAD_BYTES, Broker, TOPIC_MAIN_TELEMETRY, TOPIC_SUPERVISOR_ALARM


class MqttTests(unittest.TestCase):
    def test_command_waits_for_publish_completion(self):
        broker = Broker.__new__(Broker)
        info = Mock()
        info.rc = 0
        info.is_published.return_value = True
        broker.client = Mock()
        broker.client.is_connected.return_value = True
        broker.client.publish.return_value = info

        self.assertTrue(broker.command("light", "ON"))
        info.wait_for_publish.assert_called_once()

    def test_oversized_message_is_dropped(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.lock = threading.Lock()
        message = Mock(topic=TOPIC_MAIN_TELEMETRY, payload=b"x" * (MAX_PAYLOAD_BYTES + 1))

        with patch("app.mqtt.add") as add:
            broker.on_message(None, None, message)
        add.assert_not_called()
        self.assertEqual(broker.latest, {})

    def test_empty_retained_tombstone_is_ignored(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.lock = threading.Lock()
        message = Mock(topic=TOPIC_MAIN_TELEMETRY, payload=b"")

        with patch("app.mqtt.add") as add:
            broker.on_message(None, None, message)
        add.assert_not_called()
        self.assertEqual(broker.latest, {})

    def test_retained_health_message_is_ignored(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.lock = threading.Lock()
        message = Mock(topic=TOPIC_MAIN_TELEMETRY, payload=b"{}", retain=True)

        with patch("app.mqtt.add") as add:
            broker.on_message(None, None, message)
        add.assert_not_called()
        self.assertEqual(broker.latest, {})

    def test_retained_alarm_message_is_ignored(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.active_alarms = {}
        broker.lock = threading.Lock()
        message = Mock(topic=TOPIC_SUPERVISOR_ALARM, payload=b'{"code":"MAIN_OFFLINE"}', retain=True)

        with patch("app.mqtt.add") as add, patch("app.mqtt.send_alert") as send_alert:
            broker.on_message(None, None, message)
        add.assert_not_called()
        send_alert.assert_not_called()
        self.assertEqual(broker.latest, {})

    def test_supervisor_alarm_recovery_clears_active_alarm(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.active_alarms = {}
        broker.lock = threading.Lock()
        alarm = Mock(topic=TOPIC_SUPERVISOR_ALARM, payload=b'{"code":"MAIN_OFFLINE"}', retain=False)
        recovery = Mock(topic=TOPIC_SUPERVISOR_ALARM, payload=b'{"code":"NONE","severity":"INFO"}', retain=False)

        with patch("app.mqtt.add") as add, patch("app.mqtt.send_alert") as send_alert:
            broker.on_message(None, None, alarm)
            broker.on_message(None, None, recovery)

        self.assertNotIn("supervisor", broker.active_alarms)
        self.assertEqual(add.call_count, 2)
        send_alert.assert_called_once()

    def test_supervisor_alarm_replacement_records_recovery(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.active_alarms = {}
        broker.lock = threading.Lock()
        first = Mock(topic=TOPIC_SUPERVISOR_ALARM, payload=b'{"code":"MAIN_OFFLINE"}', retain=False)
        replacement = Mock(topic=TOPIC_SUPERVISOR_ALARM, payload=b'{"code":"TELEMETRY_STALE"}', retain=False)

        with patch("app.mqtt.add") as add, patch("app.mqtt.send_alert"):
            broker.on_message(None, None, first)
            broker.on_message(None, None, replacement)

        self.assertEqual(broker.active_alarms["supervisor"], "TELEMETRY_STALE")
        self.assertEqual(
            [call.args[3] for call in add.call_args_list],
            ["MAIN_OFFLINE", "RECOVERED", "TELEMETRY_STALE"],
        )

    def test_main_alarm_replacement_records_recovery(self):
        broker = Broker.__new__(Broker)
        broker.latest = {}
        broker.received_at = {}
        broker.active_alarms = {}
        broker.lock = threading.Lock()
        first = Mock(topic=TOPIC_MAIN_TELEMETRY, payload=b'{"alarm":"OVER_TEMP"}', retain=False)
        replacement = Mock(topic=TOPIC_MAIN_TELEMETRY, payload=b'{"alarm":"SENSOR_FAULT"}', retain=False)

        with patch("app.mqtt.add") as add, patch("app.mqtt.send_alert"):
            broker.on_message(None, None, first)
            broker.on_message(None, None, replacement)

        self.assertEqual(broker.active_alarms["main"], "SENSOR_FAULT")
        self.assertEqual(add.call_count, 5)
        self.assertEqual(add.call_args_list[3].args[3], "RECOVERED")


if __name__ == "__main__":
    unittest.main()
