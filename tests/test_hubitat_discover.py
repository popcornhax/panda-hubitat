import importlib.util
import contextlib
import io
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "scripts" / "hubitat_discover.py"
SPEC = importlib.util.spec_from_file_location("hubitat_discover", MODULE_PATH)
discoverer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(discoverer)

TOKEN = "private-token-value"


class FakeResponse:
    def __init__(self, status=200, body=b"{}"):
        self.status = status
        self.body = body

    def read(self, limit=-1):
        return self.body[:limit]


class FakeConnection:
    def __init__(self, host, port, timeout, response=None, error=None):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.response = response or FakeResponse()
        self.error = error
        self.request_args = None
        self.closed = False

    def request(self, method, path, headers=None):
        self.request_args = (method, path, headers)
        if self.error:
            raise RuntimeError(f"request failed for {path}: {TOKEN}")

    def getresponse(self):
        return self.response

    def close(self):
        self.closed = True


def factory_for(response=None, error=None, seen=None):
    def factory(host, port, timeout):
        connection = FakeConnection(host, port, timeout, response, error)
        if seen is not None:
            seen.append(connection)
        return connection
    return factory


class HubitatNormalizeTests(unittest.TestCase):
    def test_normalizes_array_and_mapping_shapes(self):
        self.assertEqual(
            discoverer.normalize_attributes([
                {"name": "temperature", "currentValue": 21.5},
                {"name": "switch", "value": "on"},
                {"name": "ignored"},
            ]),
            {"switch": "on", "temperature": 21.5, "ignored": None},
        )
        self.assertEqual(
            discoverer.normalize_attributes({"humidity": {"value": 40}, "mode": "auto"}),
            {"humidity": 40, "mode": "auto"},
        )
        self.assertEqual(
            discoverer.normalize_names(["Switch", {"name": "TemperatureMeasurement"},
                                        {"command": "setLevel"}]),
            ["setLevel", "Switch", "TemperatureMeasurement"],
        )

    def test_safe_classification_ignores_label(self):
        device = {
            "label": "Office Lamp",
            "type": "Generic Component",
            "capabilities": ["Switch"],
            "commands": ["on", "off"],
        }
        self.assertEqual(discoverer.safe_kind(device), "switch")

    def test_sensitive_label_can_only_exclude_not_allow(self):
        device = {
            "label": "Garage Door Switch",
            "type": "Generic Component",
            "capabilities": ["Switch"],
            "commands": ["on", "off"],
        }
        self.assertIsNone(discoverer.safe_kind(device))

    def test_denies_sensitive_capability_command_or_type(self):
        fixtures = [
            {"type": "Lock", "capabilities": ["Switch"], "commands": ["on"]},
            {"type": "Generic", "capabilities": ["Switch", "GarageDoorControl"], "commands": ["open"]},
            {"type": "Generic", "capabilities": ["Switch"], "commands": ["unlock"]},
            {"type": "Generic", "capabilities": ["Valve"], "commands": ["open"]},
            {"type": "Generic", "capabilities": ["Switch"], "commands": [{"command": "unlock"}]},
            {"type": "Generic", "capabilities": ["Switch"], "commands": [{"command": "openDoor"}]},
        ]
        for fixture in fixtures:
            fixture.setdefault("label", "not used for classification")
            self.assertIsNone(discoverer.safe_kind(fixture))

    def test_device_normalization_handles_realistic_mixed_shapes(self):
        device = discoverer.normalize_device(
            {"id": 42, "label": "Office", "type": "Generic Component", "capabilities": ["Switch"]},
            {"attributes": [{"name": "switch", "currentValue": "off"}],
             "commands": [{"name": "off"}, {"name": "on"}]},
        )
        self.assertEqual(device["id"], "42")
        self.assertEqual(device["attributes"], {"switch": "off"})
        self.assertEqual(device["commands"], ["off", "on"])
        self.assertEqual(discoverer.safe_kind(device), "switch")


class HubitatNetworkSafetyTests(unittest.TestCase):
    def test_config_requires_rfc1918_literal_ipv4(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "secrets.yaml"
            path.write_text("hubitat_host: 192.168.1.10\napp_id: '12'\naccess_token: abc\n")
            self.assertEqual(discoverer.validate_config(path)["host"], "192.168.1.10")
            path.write_text("hubitat_host: http://192.168.1.10\napp_id: '12'\naccess_token: abc\n")
            with self.assertRaises(discoverer.DiscoveryError):
                discoverer.validate_config(path)
            path.write_text("hubitat_host: 8.8.8.8\napp_id: '12'\naccess_token: abc\n")
            with self.assertRaises(discoverer.DiscoveryError):
                discoverer.validate_config(path)
            path.write_text("hubitat_host: 192.168.1.10\napp_id: '12'\naccess_token: '" + ("x" * 1025) + "'\n")
            with self.assertRaisesRegex(discoverer.DiscoveryError, "supported limit"):
                discoverer.validate_config(path)

    def test_direct_get_encodes_token_and_uses_bounded_timeout(self):
        seen = []
        response = FakeResponse(body=b'{"ok":true}')
        data = discoverer.fetch_json(
            "192.168.1.10", "123", "secret&value", "/devices", factory_for(response, seen=seen)
        )
        self.assertEqual(data, {"ok": True})
        connection = seen[0]
        self.assertEqual((connection.host, connection.port), ("192.168.1.10", 80))
        self.assertLessEqual(connection.timeout, discoverer.TIMEOUT_SECONDS)
        method, target, headers = connection.request_args
        self.assertEqual(method, "GET")
        self.assertEqual(target, "/apps/api/123/devices?access_token=secret%26value")
        self.assertEqual(headers["Accept"], "application/json")
        self.assertTrue(connection.closed)

    def test_redirect_is_rejected_without_following(self):
        seen = []
        with self.assertRaisesRegex(discoverer.DiscoveryError, "redirect"):
            discoverer.fetch_json("192.168.1.10", "1", TOKEN, "/devices",
                                  factory_for(FakeResponse(status=302), seen=seen))
        self.assertEqual(len(seen), 1)
        self.assertTrue(seen[0].closed)

    def test_http_errors_and_transport_errors_do_not_leak_token(self):
        with self.assertRaises(discoverer.DiscoveryError) as caught:
            discoverer.fetch_json("192.168.1.10", "1", TOKEN, "/devices",
                                  factory_for(FakeResponse(status=403)))
        self.assertNotIn(TOKEN, str(caught.exception))
        with self.assertRaises(discoverer.DiscoveryError) as caught:
            discoverer.fetch_json("192.168.1.10", "1", TOKEN, "/devices",
                                  factory_for(error=True))
        self.assertNotIn(TOKEN, str(caught.exception))

    def test_response_body_limit_is_enforced(self):
        body = b" " * (discoverer.MAX_BODY + 1)
        with self.assertRaisesRegex(discoverer.DiscoveryError, "2 MiB"):
            discoverer.fetch_json("192.168.1.10", "1", TOKEN, "/devices",
                                  factory_for(FakeResponse(body=body)))

    def test_cli_has_hard_overall_deadline(self):
        output = io.StringIO()
        with mock.patch.object(discoverer, "TOTAL_TIMEOUT_SECONDS", 0.02), \
             mock.patch.object(discoverer, "run", side_effect=lambda _args: time.sleep(1)), \
             contextlib.redirect_stderr(output):
            code = discoverer.main([])
        self.assertEqual(code, 1)
        self.assertIn("two-minute time limit", output.getvalue())

    def test_discovery_uses_only_list_and_detail_gets(self):
        responses = [
            FakeResponse(body=b'[{"id":7,"label":"Lamp","type":"Generic","capabilities":["Switch"]}]'),
            FakeResponse(body=b'{"attributes":[{"name":"switch","currentValue":"on"}],"commands":["on","off"]}'),
        ]
        calls = []

        def factory(host, port, timeout):
            connection = FakeConnection(host, port, timeout, responses[len(calls)])
            calls.append(connection)
            return connection

        result = discoverer.discover({"host": "192.168.1.10", "app_id": "5", "token": TOKEN}, factory)
        self.assertEqual(result[0]["id"], "7")
        self.assertEqual([call.request_args[0] for call in calls], ["GET", "GET"])
        self.assertEqual([call.request_args[1].split("?")[0] for call in calls],
                         ["/apps/api/5/devices", "/apps/api/5/devices/7"])


class HubitatOutputTests(unittest.TestCase):
    def test_private_json_permissions_and_mapping_selection(self):
        device = {
            "id": "7", "label": "Lamp", "type": "Generic", "kind": "unused",
            "attributes": {"switch": "on"}, "capabilities": ["Switch"],
            "commands": ["off", "on"],
        }
        with tempfile.TemporaryDirectory() as temp:
            private_dir = Path(temp) / "private"
            output = private_dir / "discovery.json"
            discoverer.write_private_json([device], output)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertEqual(private_dir.stat().st_mode & 0o777, 0o700)
            mapping = discoverer.write_mapping([device], ["7"], Path(temp) / "devices.yaml")
            text = mapping.read_text()
            self.assertIn('kind: "switch"', text)
            self.assertIn('"switch": "on"', text)
            self.assertNotIn(TOKEN, text)
            self.assertEqual(discoverer.yaml.safe_load(text)["hubitat_devices"][0]["kind"], "switch")

    def test_mapping_refuses_unsafe_or_unknown_selection(self):
        unsafe = {"id": "1", "label": "Front", "type": "Lock", "attributes": {},
                  "capabilities": ["Lock"], "commands": ["lock", "unlock"]}
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(discoverer.DiscoveryError):
                discoverer.write_mapping([unsafe], ["1"], Path(temp) / "devices.yaml")
            with self.assertRaises(discoverer.DiscoveryError):
                discoverer.write_mapping([], ["2"], Path(temp) / "devices.yaml")

    def test_token_text_is_redacted_from_discovery_outputs(self):
        result = discoverer.redact_token(
            [{"label": f"sensor {TOKEN}", "attributes": {"value": TOKEN}}], TOKEN
        )
        self.assertNotIn(TOKEN, json.dumps(result))


if __name__ == "__main__":
    unittest.main()
