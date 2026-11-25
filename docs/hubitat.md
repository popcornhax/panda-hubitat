# Hubitat Maker API access and dashboard

The host utility inspects a Hubitat Maker API app using read-only `GET`
requests. The firmware uses the same local Maker API routes for state reads and
only enables the explicitly reviewed light commands in the private mapping.
All calls go to a numeric RFC1918 IPv4 address on the LAN; the Panda has no
Home Assistant API, MQTT, cloud time service, or dashboard server.

## Configure local secrets

After creating the Maker API app on Hubitat, put these keys in the ignored
`firmware/secrets.yaml` alongside the existing Wi-Fi and OTA values:

```yaml
hubitat_host: "192.168.1.10"  # Hubitat's RFC1918 IPv4 address
app_id: "123"                # digits only
access_token: "..."          # private Maker API token
```

Do not add the token to a command line, mapping file, terminal transcript, or
tracked file. The utility reads it from the secrets file. It accepts only a
literal RFC1918 IPv4 address. It connects directly to port 80, ignores proxy
environment variables, disables redirects, sets an eight-second socket
inactivity timeout, and enforces a hard two-minute deadline for the complete
CLI run. It stops if a response exceeds 2 MiB.
The Hubitat app ID and device IDs must be numeric.

The deployed firmware uses a dedicated Maker API app configured for local
access and the selected device IDs. Keep the host address, app ID, and token in
the ignored `firmware/secrets.yaml`; do not publish household-specific values.

## Run discovery

```sh
.venv/bin/python scripts/hubitat_discover.py
```

The tool requests `GET /apps/api/{appId}/devices`, then
`GET /apps/api/{appId}/devices/{deviceId}` for each listed numeric ID. It prints
only ID, label, safe classification or exclusion, and capability/command
counts. HTTP errors, transport errors, redirects, and invalid JSON are shown
without the request URL or token.

The full response is private because device labels and sensor values can reveal
personal information. The default JSON location is
`~/Library/Application Support/panda-hubitat/discovery/`; the directory is
mode 0700 and each file is mode 0600. `--output PATH` selects an alternate
private JSON path; the file remains mode 0600.

## Explicit mapping selection

Discovery alone does not create a firmware mapping. After reviewing the latest
results, explicitly select IDs:

```sh
.venv/bin/python scripts/hubitat_discover.py --select 12,27
```

The script rechecks those IDs against that discovery and rejects devices outside
the safe allowlist. It writes `firmware/devices.yaml` with ID, label, kind,
capabilities, supported-command names, and the attributes observed during
discovery. This is read-only metadata; the script does not issue any listed
command. The mapping file is created mode 0600 and can be edited after review.
Use `--mapping PATH` to choose another mapping destination.

Kinds are `switch`, `dimmer`, `thermostat`, and `sensor`. Classification uses
Hubitat device type, capabilities, and commands; labels do not decide whether
a device is safe. Devices with lock, door, garage, alarm, security, valve, gate,
barrier, shutter, blind, shade, or access-control markers in their type,
capabilities, or commands are excluded. A label containing those markers is
also conservatively excluded; labels can never make an unknown device eligible.
Classification does not guarantee that a human-selected mapping is safe to
automate: review each selected device's type, capabilities, commands, label,
and attributes. Allowlisted capabilities are `Switch`,
`SwitchLevel`, `Thermostat`, and named sensor capabilities in
`scripts/hubitat_discover.py`. Unknown or ambiguous devices are excluded for
manual review. Temperature attribute values and units are preserved as Hubitat
reported them; no Fahrenheit/Celsius conversion is performed.

## Discovery JSON and mapping schema

Private JSON has a top-level `devices` array; each normalized device contains
`id`, `label`, `type`, `attributes`, `capabilities`, and `commands`. Attributes
normalize both Hubitat's `{name, currentValue}` arrays and name/value mappings.
The generated YAML has a top-level `hubitat_devices` array with one entry per
explicitly selected safe ID:

```yaml
hubitat_devices:
  - id: "12"
    label: "Office lamp"
    kind: "switch"
    capabilities:
      - "Switch"
    commands:
      - "off"
      - "on"
    attributes:
      "switch": "off"
```

No access token or Wi-Fi credential is stored in the mapping. Device attributes
may be sensitive, so retain the generated mapping as private local data.

## Validation

Run the complete host-side suite. It includes synthetic discovery/backup cases
and native C++ dashboard state tests, and it does not contact Hubitat:

```sh
.venv/bin/python -m unittest discover -s tests
```

Tests check normalizers, allowlist exclusions, private file permissions,
RFC1918 validation, token redaction, body and time limits, and the two read-only
request paths. Native state tests exercise confirmation, queue, freshness,
backoff, and offline-control behavior.

## Firmware transport and dashboard

`components/hubitat_local/` and `components/hubitat_dashboard/` are local
ESPHome external components used by `firmware/panda-touch.yaml`. The Hubitat
access token is read from `firmware/secrets.yaml` and compiled into ignored
firmware artifacts. The transport has one bounded
FreeRTOS worker and a one-request queue. The worker owns ESP-IDF's
`esp_http_client` and performs only direct requests to the configured numeric
local address. It does not call ESPHome, LVGL, or JSON APIs. A pointer to a
fixed-size result structure is passed back to `loop()`, where JSON is parsed,
last result fields are updated, and registered callbacks run.
`last_response_completed_ms()` records every finished result;
`last_successful_state_ms()` changes only after a complete HTTP 200 state response
with a numeric or digit-string `id` matching the requested device and an
`attributes` array or object. Empty JSON, an error object, a missing/mismatched
ID, or missing attributes is reported as invalid state and does not advance the
state timestamp. A command's accepted HTTP response never advances the state
timestamp; it only means Maker API accepted the request.

The public methods are `request_state(device_id)` and
`request_command(device_id, command, value)`. IDs must be in the configured
selection (one to twelve). Commands use a fixed enum (`on`, `off`, `setLevel`,
`setHeatingSetpoint`, `setCoolingSetpoint`) and must also appear in the
per-device `command_allowlist`. Level is bounded to 0–100; thermostat setpoints
are bounded to 41–95 F or 5–35 C according to `temperature_scale` (default F).
No unit conversion or magnitude inference occurs. Verify the scale Hubitat
reports before enabling thermostat commands. Example shape, using synthetic
IDs only:

```yaml
hubitat_local:
  id: hubitat_transport
  host: !secret hubitat_host
  app_id: !secret app_id
  access_token: !secret access_token
  device_ids: [12, 27]
  temperature_scale: F
  command_allowlist:
    - device_id: 12
      commands: ["on", "off", "setLevel"]
```

The generated firmware configuration is created from `firmware/devices.yaml`
by `scripts/generate_dashboard_config.py`, which the build script runs
automatically. It includes read-only state polling for selected devices and
only allowlists `on`, `off`, and `setLevel` for reviewed switch/dimmer devices.
Sensor commands such as `configure`, `refresh`, or `setMotion` are never
included. The browser editor can import up to 32 devices from this Maker API,
verify their supported capabilities, and arrange them on eight custom pages
plus Settings without rebuilding firmware. Permissions come from fresh server
metadata and are saved atomically with the layout. See
[dashboard-layout.md](dashboard-layout.md) for device selection and motion sleep.
The private mapping remains the initial fallback selection. Layout JSON alone
cannot grant commands.

The current selection contains seven tested lights (IDs 28, 372, 481, 813,
905, 399, and 23), one weather driver (449), one temperature/motion sensor
(257), one motion/presence sensor (1009), and two water sensors (386 and
1008). The known-offline Office LED Strip (911) is excluded. The laundry
presence device reports offline and a 5% battery; the UI labels its motion as
last reported and does not present it as current occupancy. Water readings are
shown read-only with battery warnings. The weather driver's Maker API body was
measured at 8,984 bytes and uses chunked transfer, so the response cap is 32
KiB. Weather condition text is stripped of HTML and clipped; remote markup,
images, and URLs are not rendered. Fahrenheit is configured explicitly.

The dashboard has uploaded pages plus a fixed Settings page. Use the left
navigation or swipe horizontally across the content area. Home can show
favorites including actual weather readings; other pages show the selected
devices in their configured order. Light cards use large, full-card toggles and
separate dimmer sliders. The Settings page shows
Wi-Fi IP/RSSI, Hubitat availability, backlight, firmware/OTA status, and a
two-tap restart confirmation, and the editor address. Touch controls stay disabled unless Wi-Fi,
Hubitat availability, and recent device state are all valid. A queued user
command waits behind an in-flight poll; command requests are never retried
after dispatch. HTTP 200 is only a dispatch acknowledgement. The card stays
pending until a fresh state GET reports the requested state, then shows an
error if no confirmation arrives within 15 seconds.
Dimmer changes are sent once on slider release. At most one user intent per
selected device is queued. An unsent intent is discarded when its confirmation
window expires; a dispatched command keeps that device's control locked until
its request completes and a later state GET refreshes the displayed state.

Each light is polled on a roughly five-second cadence; sensors are polled every
30 seconds. Only one request is in flight. Failed state requests use bounded
backoff and never advance the successful-state timestamp. Disconnected Wi-Fi
pauses polling and disables all device controls while LVGL remains local.
Hubitat's HTTP `Date` header supplies the clock; it is formatted in
`America/Chicago` using `CST6CDT,M3.2.0,M11.1.0`. No fallback cloud time source
is configured.

On ESP-IDF 5.5, the HTTP client uses a one-second socket inactivity timeout,
checks a five-second request budget between HTTP-client operations, rejects
redirects, and caps the response body at 32 KiB. ESP-IDF's header and body read
calls may each perform multiple socket reads, so a server that slowly trickles
headers or body bytes can keep one request pending beyond the five-second
budget. This is a practical timeout for the trusted local Hubitat network, not
a strict wall-clock guarantee against a slow-drip peer. Low-level
`HTTP_CLIENT` logging is disabled because debug
output can include the URL; component logs never include host, token, URL, or
response body. A successful HTTP command result means the API accepted the
request, not that the device reached the requested state; a follow-up state
request must confirm it.

The dashboard supports up to 32 selected devices, six devices per page, and eight configurable pages
plus Settings. These are software limits; the current physical selection is 12.

The transport and full dashboard were compiled with ESPHome 2026.10.2 / ESP-IDF.
The owner tested all seven selected lights: each requested state change was
confirmed by a fresh Maker API state read and each original state was restored.
The Office LED Strip was found offline and removed from the mapping. These
checks establish local request and command behavior, but do not replace the
owner's remaining physical page-by-page screen review.

Hubitat references: [Maker API overview](https://hubitat.com/home-automation/maker-api)
and [App Object local API URL methods](https://docs2.hubitat.com/en/developer/app/app-object).
