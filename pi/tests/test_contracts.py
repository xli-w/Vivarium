import unittest
from tempfile import TemporaryDirectory
from dataclasses import replace
from unittest.mock import Mock, patch

from fastapi import HTTPException

from app.main import Command, command, config, dashboard, documentation, export_data, history, normalize_command, targets
from app import db


class CommandContractTests(unittest.TestCase):
    def test_switch_values_are_normalized(self):
        self.assertEqual(normalize_command(Command(command="mister", value="on")), "ON")

    def test_fan_requires_integer_in_range(self):
        self.assertEqual(normalize_command(Command(command="fan", value=255)), "255")
        with self.assertRaises(HTTPException):
            normalize_command(Command(command="fan", value=256))

    def test_switch_rejects_arbitrary_values(self):
        with self.assertRaises(HTTPException):
            normalize_command(Command(command="fogger", value="maybe"))

    def test_alloff_does_not_forward_an_arbitrary_value(self):
        self.assertEqual(normalize_command(Command(command="alloff", value="ignored")), "OFF")

    def test_command_requires_successful_broker_publish(self):
        broker = Mock()
        broker.command.return_value = True
        with patch("app.main.broker", broker), patch("app.main.audit_command"):
            result = command(Command(command="mister", value="ON"), None)
        self.assertEqual(result, {"ok": True, "command": "mister", "value": "ON"})
        broker.command.assert_called_once_with("mister", "ON")

    def test_command_reports_broker_failure(self):
        broker = Mock()
        broker.command.return_value = False
        with patch("app.main.broker", broker), patch("app.main.audit_command"), self.assertRaises(HTTPException) as raised:
            command(Command(command="mister", value="ON"), None)
        self.assertEqual(raised.exception.status_code, 503)

    def test_dashboard_combines_live_state_and_camera_metadata(self):
        broker = Mock()
        broker.snapshot.return_value = {
            "vivarium/main/heartbeat": {"state": "NORMAL"},
            "vivarium/main/telemetry": {"soilMoisturePct": 44},
        }
        broker.ages.return_value = {}
        broker.connected.return_value = True
        with patch("app.main.broker", broker):
            result = dashboard(None)
        self.assertEqual(result["heartbeat"], {"state": "NORMAL"})
        self.assertEqual(result["telemetry"], {"soilMoisturePct": 44})
        self.assertIn("camera", result)

    def test_history_rejects_reversed_time_range(self):
        with self.assertRaises(HTTPException) as raised:
            history(since=20, until=10, limit=10, _auth=None)
        self.assertEqual(raised.exception.status_code, 400)

    def test_dashboard_does_not_expose_authenticated_stream_url(self):
        broker = Mock()
        broker.snapshot.return_value = {}
        broker.ages.return_value = {}
        broker.connected.return_value = False
        test_config = replace(
            config,
            camera_enabled=False,
            camera_stream_url="https://camera.local/live?token=secret",
            camera_snapshot_url="",
        )
        with patch("app.main.broker", broker), patch("app.main.config", test_config):
            result = dashboard(None)
        self.assertFalse(result["camera"]["streamAvailable"])
        self.assertIsNone(result["camera"]["streamUrl"])

    def test_usb_camera_snapshot_success(self):
        fake_jpeg = b"\xff\xd8\xff\xe0\x00\x10JFIF"
        test_config = replace(config, camera_enabled=True)
        with patch("app.main.config", test_config), patch("app.main.usb_camera.get_snapshot", return_value=fake_jpeg):
            from app.main import camera_snapshot
            response = camera_snapshot(None)
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.media_type, "image/jpeg")
            self.assertEqual(response.body, fake_jpeg)

    def test_usb_camera_snapshot_failure(self):
        test_config = replace(config, camera_enabled=True, camera_snapshot_url="")
        with patch("app.main.config", test_config), patch("app.main.usb_camera.get_snapshot", return_value=None):
            from app.main import camera_snapshot
            with self.assertRaises(HTTPException) as raised:
                camera_snapshot(None)
            self.assertEqual(raised.exception.status_code, 502)

    def test_require_token_accepts_header_and_cookie(self):
        from app.main import require_token
        test_config = replace(config, api_token="secret-123")
        with patch("app.main.config", test_config):
            # Valid header
            require_token(x_api_key="secret-123")
            # Valid cookie
            require_token(cookie_token="secret-123")
            # Invalid
            with self.assertRaises(HTTPException) as raised:
                require_token(cookie_token="wrong-token")
            self.assertEqual(raised.exception.status_code, 401)

    def test_documentation_renders_known_document(self):
        result = documentation("operations")
        self.assertEqual(result.status_code, 200)
        self.assertIn("TERRA v7 Operations and API", result.body.decode())

    def test_documentation_rejects_unknown_document(self):
        with self.assertRaises(HTTPException) as raised:
            documentation("secrets")
        self.assertEqual(raised.exception.status_code, 404)

    def test_targets_classify_current_telemetry(self):
        broker = Mock()
        broker.snapshot.return_value = {
            "vivarium/main/telemetry": {
                "upperTemperatureC": 23,
                "lowerTemperatureC": 23,
                "externalTemperatureC": 40,
                "upperHumidityPct": 70,
                "lowerHumidityPct": 70,
                "soilMoisturePct": 50,
            }
        }
        with patch("app.main.broker", broker):
            result = targets(None)
        self.assertEqual(result["temperature"]["values"]["upper"]["status"], "within")
        self.assertEqual(result["temperature"]["values"]["external"]["status"], "out")

    def test_export_returns_json_attachment(self):
        with patch("app.main.telemetry_history", return_value=[(10.0, "main", '{"soilMoisturePct":44}')]):
            result = export_data(format="json", since=None, until=None, limit=10, _auth=None)
        self.assertEqual(result.media_type, "application/json")
        self.assertIn("soilMoisturePct", result.body.decode())

    def test_export_rejects_reversed_time_range(self):
        with self.assertRaises(HTTPException) as raised:
            export_data(format="json", since=20, until=10, limit=10, _auth=None)
        self.assertEqual(raised.exception.status_code, 400)

    def test_active_alarms_collapse_repeated_events(self):
        with TemporaryDirectory() as directory, patch.object(db, "config", replace(config, db_path=f"{directory}/test.db")):
            db.init()
            db.add("alarms", "supervisor", '{"code":"MAIN_OFFLINE"}', "MAIN_OFFLINE", "CRITICAL", "Main controller heartbeat lost")
            db.add("alarms", "supervisor", '{"code":"MAIN_OFFLINE"}', "MAIN_OFFLINE", "CRITICAL", "Main controller heartbeat lost")
            active = db.active_alarms()
        self.assertEqual(len(active), 1)
        self.assertEqual(active[0][3], "MAIN_OFFLINE")

    def test_database_health_recovers_after_successful_probe(self):
        with TemporaryDirectory() as directory, patch.object(db, "config", replace(config, db_path=f"{directory}/test.db")):
            db.init()
            self.assertTrue(db.health()["ok"])


if __name__ == "__main__":
    unittest.main()
