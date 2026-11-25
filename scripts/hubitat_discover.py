#!/usr/bin/env python3
"""Read-only Hubitat Maker API discovery; never invokes device commands."""

from __future__ import annotations

import argparse
import datetime as dt
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import sys
import time
import urllib.parse

import yaml

MAX_BODY = 2 * 1024 * 1024
MAX_DEVICES = 500
TIMEOUT_SECONDS = 8
TOTAL_TIMEOUT_SECONDS = 120
DEFAULT_SECRETS = Path(__file__).resolve().parents[1] / "firmware" / "secrets.yaml"
DEFAULT_PRIVATE_DIR = Path.home() / "Library/Application Support/panda-hubitat/discovery"
DEFAULT_MAPPING = Path(__file__).resolve().parents[1] / "firmware" / "devices.yaml"

RFC1918 = tuple(map(ipaddress.ip_network, ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")))
DENIED_MARKERS = {
    "lock", "door", "garage", "alarm", "security", "valve", "gate",
    "barrier", "shutter", "blind", "shade", "accesscontrol", "access",
    "keypad", "siren", "panic", "emergency",
}
DENIED_COMMANDS = {
    "lock", "unlock", "open", "close", "arm", "disarm", "setcode",
    "setpin", "setusercode", "setlockcode", "setposition",
    "setlevelpercentage", "setvalveposition", "panic", "sos", "hush",
}
SENSOR_CAPABILITIES = {
    "motionsensor", "contactsensor", "presencesensor", "temperaturesensor",
    "temperaturemeasurement", "humiditysensor", "relativehumiditymeasurement",
    "illuminancesensor", "illuminancemeasurement", "watersensor",
    "leaksensor", "smokesensor", "carbonmonoxidesensor", "batterysensor",
    "voltagemeasurement", "powermeter", "energymeter", "currentsensor",
}


class DiscoveryError(Exception):
    """Safe-to-display error that contains no request URL or secret material."""


def _normalize_name(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.lower())


def validate_config(path: Path = DEFAULT_SECRETS) -> dict[str, str]:
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError):
        raise DiscoveryError("Could not read the Hubitat settings file.") from None
    if not isinstance(data, dict):
        raise DiscoveryError("Hubitat settings must be a YAML mapping.")

    host = data.get("hubitat_host")
    app_id = data.get("app_id")
    token = data.get("access_token")
    if not all(isinstance(x, str) and x.strip() for x in (host, app_id, token)):
        raise DiscoveryError("Add hubitat_host, app_id, and access_token to firmware/secrets.yaml.")
    host, app_id, token = host.strip(), app_id.strip(), token.strip()
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        raise DiscoveryError("Hubitat host must be a private IPv4 address with no scheme or path.") from None
    if address.version != 4 or not any(address in network for network in RFC1918):
        raise DiscoveryError("Hubitat host must be an RFC1918 private IPv4 address.")
    if not re.fullmatch(r"[0-9]+", app_id):
        raise DiscoveryError("Hubitat Maker API app ID must contain digits only.")
    if len(token) > 1024:
        raise DiscoveryError("Hubitat access token is longer than the supported limit.")
    if any(ord(ch) < 0x21 or ord(ch) == 0x7F for ch in token):
        raise DiscoveryError("Hubitat access token contains unsupported characters.")
    return {"host": host, "app_id": app_id, "token": token}


def _read_limited(response, limit: int = MAX_BODY) -> bytes:
    body = response.read(limit + 1)
    if len(body) > limit:
        raise DiscoveryError("Hubitat response exceeded the 2 MiB limit.")
    return body


def fetch_json(host: str, app_id: str, token: str, suffix: str,
               connection_factory=http.client.HTTPConnection) -> object:
    """GET one Maker API resource directly; redirects and proxy env are ignored."""
    if not re.fullmatch(r"/(?:devices(?:/[0-9]+)?)", suffix):
        raise DiscoveryError("Invalid internal discovery path.")
    path = f"/apps/api/{app_id}{suffix}?access_token={urllib.parse.quote(token, safe='')}"
    connection = None
    try:
        connection = connection_factory(host, 80, timeout=TIMEOUT_SECONDS)
        connection.request("GET", path, headers={"Accept": "application/json", "Connection": "close"})
        response = connection.getresponse()
        status = response.status
        if 300 <= status < 400:
            raise DiscoveryError("Hubitat returned a redirect; it was not followed.")
        if status != 200:
            raise DiscoveryError(f"Hubitat returned HTTP status {status}.")
        try:
            return json.loads(_read_limited(response))
        except (json.JSONDecodeError, UnicodeDecodeError):
            raise DiscoveryError("Hubitat returned invalid JSON.") from None
    except DiscoveryError:
        raise
    except DiscoveryTimeout:
        raise
    except Exception:
        # HTTP/client exceptions can contain the full request target, including token.
        raise DiscoveryError("Hubitat request failed; check the private address and network connection.") from None
    finally:
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass


def normalize_attributes(value: object) -> dict[str, object]:
    result: dict[str, object] = {}
    if isinstance(value, dict):
        entries = value.items()
        for name, raw in entries:
            if isinstance(raw, dict):
                raw = raw.get("currentValue", raw.get("value", raw))
            if isinstance(name, str):
                result[name] = raw
    elif isinstance(value, list):
        for item in value:
            if not isinstance(item, dict):
                continue
            name = item.get("name")
            if isinstance(name, str):
                result[name] = item.get("currentValue", item.get("value"))
    return dict(sorted(result.items(), key=lambda pair: pair[0].lower()))


def normalize_names(value: object) -> list[str]:
    if isinstance(value, dict):
        value = list(value.keys())
    if not isinstance(value, list):
        return []
    names = set()
    for item in value:
        name = (item if isinstance(item, str) else
                item.get("name", item.get("command")) if isinstance(item, dict) else None)
        if isinstance(name, str) and name.strip():
            names.add(name.strip())
    return sorted(names, key=str.lower)


def _device_id(device: dict) -> str | None:
    value = device.get("id", device.get("deviceId"))
    if isinstance(value, int) and not isinstance(value, bool):
        value = str(value)
    return value if isinstance(value, str) and re.fullmatch(r"[0-9]+", value) else None


def normalize_device(listing: dict, details: dict) -> dict:
    merged = {**listing, **details}
    attrs = normalize_attributes(details.get("attributes", listing.get("attributes")))
    capabilities = normalize_names(details.get("capabilities", listing.get("capabilities")))
    commands = normalize_names(details.get("commands", listing.get("commands")))
    device_id = _device_id(merged)
    if device_id is None:
        raise DiscoveryError("Hubitat returned a device without a numeric ID.")
    label = merged.get("label") or merged.get("name") or f"Device {device_id}"
    device_type = merged.get("type") or merged.get("deviceType") or ""
    return {
        "id": device_id,
        "label": str(label),
        "type": str(device_type),
        "attributes": attrs,
        "capabilities": capabilities,
        "commands": commands,
    }


def safe_kind(device: dict) -> str | None:
    """Allowlist by device metadata; risky labels may reject but never allow."""
    caps = {_normalize_name(x) for x in normalize_names(device.get("capabilities"))}
    commands = {_normalize_name(x) for x in normalize_names(device.get("commands"))}
    type_name = _normalize_name(device["type"])
    label_name = _normalize_name(device.get("label", ""))
    all_names = caps | commands | {type_name, label_name}
    if any(marker in name for name in all_names for marker in DENIED_MARKERS):
        return None
    if commands & DENIED_COMMANDS:
        return None
    if "thermostat" in caps or type_name == "thermostat":
        return "thermostat"
    if "switchlevel" in caps:
        return "dimmer"
    if "switch" in caps:
        return "switch"
    if caps & SENSOR_CAPABILITIES:
        return "sensor"
    return None


def discover(config: dict[str, str], connection_factory=http.client.HTTPConnection) -> list[dict]:
    listings = fetch_json(config["host"], config["app_id"], config["token"], "/devices", connection_factory)
    if not isinstance(listings, list):
        raise DiscoveryError("Hubitat device list was not a JSON array.")
    if len(listings) > MAX_DEVICES:
        raise DiscoveryError("Hubitat returned more than 500 devices; discovery stopped.")
    normalized = []
    for listing in listings:
        if not isinstance(listing, dict) or _device_id(listing) is None:
            raise DiscoveryError("Hubitat returned a device without a numeric ID.")
        details = fetch_json(config["host"], config["app_id"], config["token"],
                             f"/devices/{_device_id(listing)}", connection_factory)
        if not isinstance(details, dict):
            raise DiscoveryError("Hubitat returned an invalid device detail object.")
        normalized.append(normalize_device(listing, details))
    return sorted(normalized, key=lambda item: (item["label"].lower(), item["id"]))


def summarize(devices: list[dict]) -> list[str]:
    rows = []
    for device in devices:
        kind = safe_kind(device)
        classification = kind if kind else "excluded"
        label = "".join(ch if ch >= " " and ch != "\x7f" else " " for ch in device["label"])
        rows.append(
            f"{device['id']}  {label}  {classification}  "
            f"{len(device['capabilities'])} capabilities, {len(device['commands'])} commands"
        )
    return rows


def redact_token(value: object, token: str) -> object:
    if isinstance(value, str):
        return value.replace(token, "[redacted]")
    if isinstance(value, list):
        return [redact_token(item, token) for item in value]
    if isinstance(value, dict):
        return {key: redact_token(item, token) for key, item in value.items()}
    return value


def write_private_json(devices: list[dict], output: Path | None = None) -> Path:
    if output is None:
        directory = DEFAULT_PRIVATE_DIR.expanduser().resolve()
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(directory, 0o700)
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        output = directory / f"maker-api-discovery-{stamp}.json"
    else:
        output = output.expanduser().resolve()
        if not output.parent.exists():
            output.parent.mkdir(parents=True, mode=0o700)
            os.chmod(output.parent, 0o700)
    payload = json.dumps({"devices": devices}, indent=2, ensure_ascii=False) + "\n"
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(payload)
    except Exception:
        output.unlink(missing_ok=True)
        raise DiscoveryError("Could not write the private discovery file.") from None
    os.chmod(output, 0o600)
    return output


def write_mapping(devices: list[dict], selected_ids: list[str], output: Path = DEFAULT_MAPPING) -> Path:
    by_id = {device["id"]: device for device in devices}
    if not selected_ids or len(set(selected_ids)) != len(selected_ids):
        raise DiscoveryError("Select one or more unique device IDs.")
    selected = []
    for device_id in selected_ids:
        device = by_id.get(device_id)
        if device is None:
            raise DiscoveryError(f"Selected device ID {device_id} is not in the latest discovery.")
        kind = safe_kind(device)
        if kind is None:
            raise DiscoveryError(f"Selected device ID {device_id} is not in the safe mapping allowlist.")
        selected.append((device, kind))

    # JSON strings are valid YAML scalars; no extra YAML package is needed here.
    lines = [
        "# Generated by scripts/hubitat_discover.py --select; edit after review.",
        "# Read-only mapping metadata. This file contains no access token.",
        "hubitat_devices:",
    ]
    for device, kind in selected:
        lines.extend([
            f"  - id: {json.dumps(device['id'])}",
            f"    label: {json.dumps(device['label'], ensure_ascii=False)}",
            f"    kind: {json.dumps(kind)}",
        ])
        if device["capabilities"]:
            lines.append("    capabilities:")
            lines.extend(f"      - {json.dumps(name)}" for name in device["capabilities"])
        else:
            lines.append("    capabilities: []")
        if device["commands"]:
            lines.append("    commands:")
            lines.extend(f"      - {json.dumps(name)}" for name in device["commands"])
        else:
            lines.append("    commands: []")
        if device["attributes"]:
            lines.append("    attributes:")
            for name, value in device["attributes"].items():
                lines.append(f"      {json.dumps(name)}: {json.dumps(value, ensure_ascii=False)}")
        else:
            lines.append("    attributes: {}")
    text = "\n".join(lines) + "\n"
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    os.chmod(output, 0o600)
    return output


def run(args, connection_factory=http.client.HTTPConnection) -> int:
    config = validate_config(args.secrets)
    devices = redact_token(discover(config, connection_factory), config["token"])
    output = write_private_json(devices, args.output)
    for row in summarize(devices):
        print(row)
    print(f"Saved private discovery JSON for {len(devices)} devices.")
    if args.select:
        selected_ids = [part.strip() for part in args.select.split(",") if part.strip()]
        mapping_path = write_mapping(devices, selected_ids, args.mapping)
        print(f"Wrote a read-only mapping for {len(selected_ids)} selected safe devices.")
    return 0


class DiscoveryTimeout(Exception):
    """Hard deadline for a complete CLI discovery run."""


def _deadline_handler(_signum, _frame):
    raise DiscoveryTimeout()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--secrets", type=Path, default=DEFAULT_SECRETS,
                        help="YAML secrets file (default: firmware/secrets.yaml)")
    parser.add_argument("--output", type=Path, help="private JSON output path")
    parser.add_argument("--select", help="comma-separated safe device IDs to create firmware/devices.yaml")
    parser.add_argument("--mapping", type=Path, default=DEFAULT_MAPPING,
                        help="mapping output path used with --select")
    args = parser.parse_args(argv)
    started = time.monotonic()
    previous_handler = signal.getsignal(signal.SIGALRM)
    previous_timer = signal.getitimer(signal.ITIMER_REAL)
    try:
        signal.signal(signal.SIGALRM, _deadline_handler)
        signal.siginterrupt(signal.SIGALRM, True)
        signal.setitimer(signal.ITIMER_REAL, TOTAL_TIMEOUT_SECONDS)
        return run(args)
    except DiscoveryTimeout:
        print("Discovery stopped after reaching the two-minute time limit.", file=sys.stderr)
        return 1
    except DiscoveryError as exc:
        print(f"Discovery stopped: {exc}", file=sys.stderr)
        return 1
    except OSError:
        print("Discovery stopped: a local file operation failed.", file=sys.stderr)
        return 1
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_timer[0] > 0:
            remaining = max(0.0, previous_timer[0] - (time.monotonic() - started))
            signal.setitimer(signal.ITIMER_REAL, remaining, previous_timer[1])


if __name__ == "__main__":
    raise SystemExit(main())
