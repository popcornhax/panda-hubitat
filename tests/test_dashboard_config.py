import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class DashboardLayoutCodecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("c++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++ compiler is unavailable")
        candidates = []
        configured = os.environ.get("ARDUINOJSON_INCLUDE")
        if configured:
            candidates.append(pathlib.Path(configured))
        candidates.extend([
            ROOT / "firmware/.esphome/build/panda-touch/managed_components/bblanchon__arduinojson",
            ROOT / "firmware/.esphome/build/panda-touch/managed_components/bblanchon__arduinojson/src",
        ])
        include = next((p for p in candidates if (p / "ArduinoJson.h").is_file()), None)
        if include is None:
            raise unittest.SkipTest("ArduinoJson is not present; compile the firmware once first")
        cls.temp = tempfile.TemporaryDirectory(prefix="dashboard-layout-test-")
        temp = pathlib.Path(cls.temp.name)
        (temp / "stubs/esphome/core").mkdir(parents=True)
        (temp / "stubs").mkdir(exist_ok=True)
        (temp / "stubs/esphome/core/component.h").write_text(
            "#pragma once\nnamespace esphome { namespace setup_priority { constexpr float DATA=600; } "
            "class Component { public: virtual ~Component()=default; virtual void setup(){}; "
            "virtual void loop(){}; virtual void on_safe_shutdown(){}; "
            "virtual float get_setup_priority() const {return 0;} }; }\n"
        )
        (temp / "stubs/esp_http_server.h").write_text(
            "#pragma once\ntypedef int esp_err_t; struct httpd_req_t {};\n"
        )
        (temp / "codec_test.cpp").write_text(r'''#include <iostream>
#include <iterator>
#include <string>
#include "layout_codec.h"
#include "device_catalog.h"
int main(int argc, char **argv) {
  std::string input((std::istreambuf_iterator<char>(std::cin)), {});
  if (argc > 1) {
    JsonDocument doc;
    if (deserializeJson(doc, input)) return 2;
    if (std::string(argv[1]) == "--registry") {
      std::vector<esphome::dashboard_config::CompiledDevice> devices;
      return esphome::dashboard_config::parse_registry(doc.as<JsonArrayConst>(), &devices) ? 0 : 1;
    }
    esphome::dashboard_config::CompiledDevice device;
    std::string error;
    if (!esphome::dashboard_config::classify_device(doc.as<JsonObjectConst>(), &device, &error)) return 1;
    JsonDocument result;
    result["kind"] = device.kind; result["mask"] = device.command_mask;
    result["motion"] = device.motion_capable;
    serializeJson(result, std::cout);
    return 0;
  }
  std::vector<esphome::dashboard_config::CompiledDevice> devices = {
    {1,"Lamp","dimmer",true}, {2,"Motion","motion",false,0,true}
  };
  esphome::dashboard_config::DashboardLayout out;
  std::string error;
  if (!esphome::dashboard_config::parse_layout_json(
      reinterpret_cast<const uint8_t *>(input.data()), input.size(), devices, &out, &error)) {
    std::cerr << error; return 1;
  }
  std::cout << esphome::dashboard_config::serialize_layout_json(out, devices);
  return 0;
}
''')
        cls.binary = temp / "codec_test"
        build = subprocess.run([
            compiler, "-std=c++17", "-I", str(temp / "stubs"), "-I", str(include), "-I", str(include / "src"),
            "-I", str(ROOT / "components/dashboard_config"),
            str(ROOT / "components/dashboard_config/layout_codec.cpp"),
            str(ROOT / "components/dashboard_config/device_catalog.cpp"),
            str(temp / "codec_test.cpp"), "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def run_codec(self, value):
        payload = value if isinstance(value, str) else json.dumps(value, separators=(",", ":"))
        return subprocess.run([str(self.binary)], input=payload, capture_output=True, text=True)

    def valid(self):
        return {
            "version": 1,
            "theme": {"background": 1, "panel": 2, "text": 3, "muted": 4, "accent": 5},
            "font": "normal",
            "pages": [
                {"label": "Home", "title": "Hello", "devices": [1]},
                {"label": "Other", "title": "More", "devices": [1, 2]},
            ],
            "labels": [{"id": 1, "label": "New lamp"}],
        }

    def test_accepts_repeated_device_on_different_pages(self):
        result = self.run_codec(self.valid())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"font":"normal"', result.stdout)

    def test_rejects_unknown_fields_and_ids(self):
        value = self.valid()
        value["admin"] = True
        self.assertNotEqual(self.run_codec(value).returncode, 0)
        value = self.valid()
        value["pages"][0]["devices"] = [999]
        self.assertNotEqual(self.run_codec(value).returncode, 0)

    def test_rejects_duplicates_and_invalid_limits(self):
        value = self.valid()
        value["pages"][0]["devices"] = [1, 1]
        self.assertNotEqual(self.run_codec(value).returncode, 0)
        value = self.valid()
        value["pages"][0]["label"] = "Nav label is too long"
        self.assertNotEqual(self.run_codec(value).returncode, 0)

    def test_rejects_bad_color_font_version_and_size(self):
        for key, value in (("font", "custom"), ("version", 2)):
            payload = self.valid()
            payload[key] = value
            self.assertNotEqual(self.run_codec(payload).returncode, 0)
        payload = self.valid()
        payload["theme"]["background"] = 0x1000000
        self.assertNotEqual(self.run_codec(payload).returncode, 0)
        self.assertNotEqual(self.run_codec(" " * 16385).returncode, 0)

    def test_rejects_trailing_data_and_embedded_nul(self):
        payload = json.dumps(self.valid(), separators=(",", ":"))
        self.assertNotEqual(self.run_codec(payload + " garbage").returncode, 0)
        payload = self.valid()
        payload["pages"][0]["title"] = "Hi\u0000junk"
        self.assertNotEqual(self.run_codec(payload).returncode, 0)
        payload = self.valid()
        payload["font"] = "normal\u0000junk"
        self.assertNotEqual(self.run_codec(payload).returncode, 0)

    def test_eight_pages_and_sleep_validation(self):
        value = self.valid()
        value["pages"] = [{"label": f"Page {i}", "title": "Example", "devices": [1]} for i in range(8)]
        value["sleep"] = {"enabled": True, "motion_device_id": 2, "timeout_seconds": 30, "fade_ms": 800}
        result = self.run_codec(value)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["sleep"]["enabled"])
        value["pages"].append({"label": "Page 9", "title": "Example", "devices": []})
        self.assertNotEqual(self.run_codec(value).returncode, 0)
        for field, bad in (("motion_device_id", 1), ("timeout_seconds", 29), ("fade_ms", 3001)):
            payload = self.valid()
            payload["sleep"] = {"enabled": True, "motion_device_id": 2, "timeout_seconds": 30, "fade_ms": 800}
            payload["sleep"][field] = bad
            self.assertNotEqual(self.run_codec(payload).returncode, 0)

    def catalog(self, capabilities, commands=(), **extra):
        payload = {"id": "10", "label": "Example device", "type": "Generic", "capabilities": capabilities,
                   "commands": list(commands), "attributes": [], **extra}
        return subprocess.run([str(self.binary), "--catalog"], input=json.dumps(payload), capture_output=True, text=True)

    def test_catalog_narrow_permissions_and_standard_sensors(self):
        result = self.catalog(["SwitchLevel", "Switch"], ["on", "off", "setLevel", "setColor", "refresh"])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["mask"], 7)
        result = self.catalog(["Switch"], ["on", "off", "flash"])
        self.assertEqual(json.loads(result.stdout)["mask"], 3)
        result = self.catalog(["ContactSensor"], ["open", "close"])
        self.assertEqual(json.loads(result.stdout), {"kind": "contact", "mask": 0, "motion": False})
        result = self.catalog(["PresenceSensor", "MotionSensor"])
        self.assertTrue(json.loads(result.stdout)["motion"])
        result = self.catalog(["PresenceSensor"])
        self.assertFalse(json.loads(result.stdout)["motion"])
        result = self.catalog(["TemperatureMeasurement"], attributes=[{"name": "temperature", "currentValue": 0}])
        self.assertEqual(json.loads(result.stdout)["kind"], "sensor")

    def test_catalog_rejects_risky_or_malformed_control_devices(self):
        for cap in ("Lock", "GarageDoorControl", "Valve", "Thermostat", "WindowShade"):
            self.assertNotEqual(self.catalog([cap, "Switch"], ["on", "off"]).returncode, 0)
        self.assertNotEqual(self.catalog(["Switch"], ["on", "off", "unlock"]).returncode, 0)
        self.assertNotEqual(self.catalog(["SwitchLevel"], ["on", "off"]).returncode, 0)
        self.assertNotEqual(self.catalog(["Switch"], ["on", "off"], id=True).returncode, 0)
        self.assertNotEqual(self.catalog(["Switch"], ["on", "off"], label="Garage heater").returncode, 0)

    def test_registry_bounds_and_command_mask_integrity(self):
        registry = [{"id": i + 1, "label": "Example", "kind": "switch", "favorite": False, "command_mask": 3}
                    for i in range(32)]
        def run(value):
            return subprocess.run([str(self.binary), "--registry"], input=json.dumps(value), capture_output=True, text=True)
        self.assertEqual(run(registry).returncode, 0)
        self.assertNotEqual(run(registry + [registry[0]]).returncode, 0)
        registry[0]["command_mask"] = 31
        self.assertNotEqual(run(registry).returncode, 0)


if __name__ == "__main__":
    unittest.main()
