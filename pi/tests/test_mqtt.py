import threading
import unittest
from unittest.mock import Mock, patch

from app.mqtt import MAX_PAYLOAD_BYTES, Broker, TOPIC_MAIN_TELEMETRY


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


if __name__ == "__main__":
    unittest.main()
