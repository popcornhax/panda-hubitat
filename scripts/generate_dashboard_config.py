#!/usr/bin/env python3
"""Generate ignored ESPHome Hubitat component configuration from devices.yaml."""
import os
from pathlib import Path
import sys
import json

import yaml

ROOT = Path(__file__).resolve().parents[1]
MAPPING = ROOT / "firmware/devices.yaml"
OUTPUT = ROOT / "firmware/generated-dashboard.yaml"


def quote(value: str) -> str:
    return json.dumps(str(value), ensure_ascii=False)


def main() -> int:
    os.umask(0o077)
    if not MAPPING.is_file():
        raise SystemExit("firmware/devices.yaml is required; run Hubitat discovery and explicitly select devices")
    with MAPPING.open(encoding="utf-8") as stream:
        mapping = yaml.safe_load(stream) or {}
    devices = mapping.get("hubitat_devices")
    if not isinstance(devices, list) or not 1 <= len(devices) <= 32:
        raise SystemExit("hubitat_devices must contain 1 to 32 explicitly selected devices")
    selected = []
    allow = []
    seen = set()
    for entry in devices:
        try:
            device_id = int(entry["id"])
            label = str(entry["label"])
            kind = str(entry["kind"])
            commands = set(entry["commands"])
        except (KeyError, TypeError, ValueError) as err:
            raise SystemExit("invalid selected-device mapping") from err
        if device_id <= 0 or device_id in seen or not label or len(label) > 32:
            raise SystemExit("device IDs must be unique and labels must be 1–32 characters")
        if kind not in {"switch", "dimmer", "motion", "weather", "presence", "water", "contact", "sensor"}:
            raise SystemExit(f"unsupported device kind: {kind!r}")
        allowed = []
        if kind in {"switch", "dimmer"}:
            allowed = [name for name in ("on", "off", "setLevel") if name in commands]
            if not {"on", "off"}.issubset(allowed):
                raise SystemExit("selected light must expose both on and off commands")
            if kind == "dimmer" and "setLevel" not in allowed:
                raise SystemExit("dimmer must expose setLevel")
        seen.add(device_id)
        selected.append((device_id, label, kind))
        allow.append((device_id, allowed))
    favorites = {int(entry["id"]) for entry in devices if entry.get("favorite", False) is True}
    lines = [
        "hubitat_local:",
        "  id: hubitat_transport",
        "  host: !secret hubitat_host",
        "  app_id: !secret app_id",
        "  access_token: !secret access_token",
        "  temperature_scale: F",
        "  device_ids:",
    ]
    lines.extend(f"    - {device_id}" for device_id, _, _ in selected)
    lines.append("  command_allowlist:")
    for device_id, commands in allow:
        if commands:
            lines.append(f"    - device_id: {device_id}")
            lines.append("      commands: [" + ", ".join(quote(command) for command in commands) + "]")
    lines += [
        "",
        "dashboard_config:",
        "  id: dashboard_config_app",
        "  transport_id: hubitat_transport",
        "  devices:",
    ]
    for device_id, label, kind in selected:
        lines += [f"    - id: {device_id}", f"      label: {quote(label)}", f"      kind: {kind}",
                  f"      favorite: {'true' if device_id in favorites else 'false'}"]
    lines += [
        "",
        "hubitat_dashboard:",
        "  id: hubitat_dashboard_app",
        "  config_id: dashboard_config_app",
        "  transport_id: hubitat_transport",
        "  backlight_id: backlight",
        "  wifi_id: wifi_id",
        "  devices:",
    ]
    for device_id, label, kind in selected:
        lines += [f"    - id: {device_id}", f"      label: {quote(label)}", f"      kind: {kind}",
                  f"      favorite: {'true' if device_id in favorites else 'false'}"]
    temp = OUTPUT.with_suffix(".tmp")
    temp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.chmod(temp, 0o600)
    temp.replace(OUTPUT)
    print(f"Generated private dashboard config for {len(selected)} explicitly selected devices.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
