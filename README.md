# panda-hubitat

ESPHome firmware that turns a BIGTREETECH Panda Touch (ESP32-S3, 800×480 touch
display) into a local wall-panel dashboard for a
[Hubitat Elevation](https://hubitat.com/) hub. The panel talks directly to
Hubitat's local [Maker API](https://docs2.hubitat.com/en/apps/maker-api) over
the LAN. It does not use Home Assistant, the native ESPHome API, MQTT, or any
cloud service.

This repository contains only panel firmware and host-side helper scripts.
There is no Hubitat driver or app to install on the hub; you use Hubitat's
built-in Maker API app.

## Features

- LVGL touch dashboard with up to eight pages plus Settings, 32 selected
  devices, and six device cards per page.
- On/off and dimmer (`setLevel`) control for verified lights. Motion,
  presence, contact, water, weather, temperature, humidity, illuminance,
  power, and voltage sensors are shown read-only.
- Built-in browser layout editor on port 80 for Maker API device import, page
  layout, labels, theme colors, text size, and a background image. Changes
  apply after a restart, without rebuilding the firmware.
- Optional backlight sleep with wake on motion, wake on touch, or wake from
  the editor.
- Encrypted ESPHome OTA updates.
- Host scripts for read-only Maker API discovery, explicit device selection,
  and a verified full-flash backup before you replace the factory firmware.

## Supported hardware

- BIGTREETECH Panda Touch (ESP32-S3, 16 MB flash, 8 MB octal PSRAM, 800×480
  RGB LCD, GT911 touch). Pin mappings are in
  [`firmware/packages/panda-hardware.yaml`](firmware/packages/panda-hardware.yaml)
  and come from the [BTT pinout](https://raw.githubusercontent.com/bigtreetech/PandaTouch_IDF/master/docs/pinout.md).
- The Panda Touch magnetic USB-C dock (CH340K USB-UART) for the first serial
  install, flash backup, and recovery.
- A Hubitat Elevation hub reachable from the panel at a private IPv4 address,
  with the built-in Maker API app.

## Repository layout

| Path | Purpose |
| --- | --- |
| `firmware/panda-touch.yaml` | Main dashboard firmware configuration |
| `firmware/display-touch-test.yaml` | Minimal display and touch smoke test |
| `firmware/packages/panda-hardware.yaml` | Shared Panda Touch hardware configuration |
| `components/hubitat_local/` | Local Maker API HTTP client |
| `components/hubitat_dashboard/` | LVGL dashboard, polling, and command state logic |
| `components/dashboard_config/` | Browser layout editor, device catalog, and layout storage |
| `scripts/` | Bootstrap, validation, build, discovery, config generation, and flash backup |
| `tests/` | Host unit tests, including a native C++ state-logic test |
| `examples/dashboard-layout.json` | Example layout for the browser editor |
| `docs/` | Hubitat setup, layout editor, recovery, and validation notes |

## Status

The firmware has been flashed and tested on one Panda Touch. Display, touch,
backlight, Wi-Fi, encrypted OTA, Hubitat discovery, light control, the layout
editor, and screen capture have all been exercised on hardware. Tests for
Wi-Fi loss and for brightness persistence across restarts are still pending.
[docs/validation.md](docs/validation.md) has the full bring-up record.

## Setup

Requires macOS or Linux, [`uv`](https://docs.astral.sh/uv/), and network access.
The bootstrap script creates a Python 3.12–3.15 virtual environment and installs
the pinned ESPHome requirements. The first firmware build also downloads the
ESP-IDF toolchain and components. Install `uv` using its official instructions,
then run:

```sh
./scripts/bootstrap.sh
cp secrets.example.yaml firmware/secrets.yaml
```

On the Hubitat hub, set up a dedicated Maker API instance for the panel:

1. In the Hubitat web UI, open **Apps → Add Built-In App → Maker API**.
2. Enable local access only. Leave cloud access off.
3. Select only the lights and sensors the panel should show or control.
4. Note the hub's LAN IPv4 address, the numeric app ID from the local endpoint
   URLs, and the access token.

Edit `firmware/secrets.yaml` and replace every example value. Generate
`panda_ota_key` with `openssl rand -base64 32`. Put the hub address, app ID, and
token in `hubitat_host`, `app_id`, and `access_token`. More detail is in
[docs/hubitat.md](docs/hubitat.md). Use a 2.4 GHz Wi-Fi network. `app_id` must
contain only digits. Next, discover devices and explicitly select the IDs to
include:

```sh
.venv/bin/python scripts/hubitat_discover.py
.venv/bin/python scripts/hubitat_discover.py --select 12,27
```

Replace `12,27` with IDs reviewed from your discovery results. This creates the
ignored local `firmware/devices.yaml` mapping required by the main build.
`firmware/secrets.yaml`, the selected mapping, and generated firmware
configuration are ignored by Git; keep them private.

## Configuration

| Setting | Where | Notes |
| --- | --- | --- |
| `wifi_ssid`, `wifi_password` | `firmware/secrets.yaml` | 2.4 GHz network |
| `panda_ota_key` | `firmware/secrets.yaml` | Base64 32-byte key for encrypted OTA (`openssl rand -base64 32`) |
| `hubitat_host` | `firmware/secrets.yaml` | Hubitat's literal RFC1918 IPv4 address; hostnames are rejected |
| `app_id` | `firmware/secrets.yaml` | Maker API app ID, digits only |
| `access_token` | `firmware/secrets.yaml` | Maker API access token (1–512 characters) |
| Compiled device selection | `firmware/devices.yaml` | Written by `scripts/hubitat_discover.py --select`. It is the fallback when no runtime selection has been saved. |
| Pages, labels, colors, fonts, background, screen sleep | Browser editor at `http://panda-touch.local/` | Stored on the device; see [docs/dashboard-layout.md](docs/dashboard-layout.md) |
| Temperature scale | `scripts/generate_dashboard_config.py` | Generated as `temperature_scale: F`; change it to `C` there to show Celsius |
| Time zone | `components/hubitat_dashboard/hubitat_dashboard.cpp` | Fixed POSIX TZ `CST6CDT,M3.2.0,M11.1.0` (US Central). Edit the `setenv("TZ", ...)` call and rebuild for another zone. |

`firmware/secrets.yaml`, `firmware/devices.yaml`, and the generated
`firmware/generated-dashboard.yaml` are all ignored by Git.

## Validate and build

```sh
./scripts/validate.sh
./scripts/build.sh
./scripts/build.sh --test-config
```

The build commands compile firmware only. They do not flash or upload to a
device. Main configuration is `firmware/panda-touch.yaml`; the separate
`firmware/display-touch-test.yaml` is useful for the first display and touch
bring-up.
The main build requires the secrets file and selected `firmware/devices.yaml`
mapping from the Setup steps.

Use temporary credentials in `firmware/secrets.yaml` for local validation when
the real Wi-Fi values are not available. Do not commit generated build output
or temporary secrets.

Run host tests with `.venv/bin/python -m unittest discover -s tests`. The suite
includes a native C++ test of the state logic used by the dashboard.

## First installation over USB

Before replacing the factory firmware, identify the dock's serial port and
make a verified full-flash backup. Follow [docs/recovery.md](docs/recovery.md);
do not proceed if the chip identity, flash size, or security checks do not
match that procedure. Then run ESPHome's serial upload, replacing `PORT` with
the current WCH serial device:

```sh
.venv/bin/python scripts/generate_dashboard_config.py
.venv/bin/esphome run firmware/panda-touch.yaml --device PORT --upload_speed 115200
```

The [`esphome run` command](https://esphome.io/guides/cli/) validates,
compiles, uploads, and starts logs. After the panel joins Wi-Fi, later updates
can use the encrypted [OTA workflow](#tested-ota-workflow).

## Tested OTA workflow

Keep the existing `panda_ota_key` in the private secrets file, edit configuration,
then build and upload:

```sh
./scripts/build.sh
.venv/bin/esphome upload firmware/panda-touch.yaml --device panda-touch.local
```

The first hardware test used the panel's private LAN address. ESPHome confirmed
an encrypted connection and successful upload; a subsequent reset booted the
new OTA slot and reconnected to Wi-Fi. Use `panda-touch.local` when it resolves
on your network. OTA requires the computer to reach the panel on the local
network. If it cannot connect, use the dock and the
[serial recovery procedure](docs/recovery.md).

Encrypted ESPHome OTA works independently of the native ESPHome API; no Home
Assistant installation is needed.

## Network security

The browser editor is intentionally unauthenticated and uses plain HTTP for
trusted home-LAN use. Do not port-forward it or expose the panel to guest or
untrusted networks. Same-origin checks help prevent browser cross-site requests;
they do not authenticate other software on the LAN. Keep local secrets, device
selections, discovery snapshots, and flash backups private and out of Git.

## Hubitat discovery

Discovery uses read-only Maker API requests; explicitly selecting devices
creates the private mapping used to generate firmware permissions. See
[Hubitat access and dashboard details](docs/hubitat.md) for selection review,
supported types, and safety boundaries. See [browser layout editor](docs/dashboard-layout.md)
for runtime customization and the distinction between uploaded layouts and
firmware device permissions.

## Flash backup

Inspect the backup utility before connecting to a device:

```sh
.venv/bin/python scripts/backup_flash.py --help
```

Backups default to `~/Library/Application Support/panda-hubitat/backups` on
macOS (or the documented equivalent on other systems), with private directory
permissions. See [recovery documentation](docs/recovery.md) for the verified
backup and recovery procedure.

## Hardware references

- [BTT Panda Touch pinout](https://raw.githubusercontent.com/bigtreetech/PandaTouch_IDF/master/docs/pinout.md)
- [ESPHome MIPI RGB display](https://esphome.io/components/display/mipi_rgb/)
- [ESPHome GT911 touchscreen](https://esphome.io/components/touchscreen/gt911/)
- [ESPHome encrypted OTA](https://esphome.io/components/ota/esphome/)

## Battery operation

The Panda Touch's rear switch selects battery power separately from the
firmware. `DC 5V` intentionally powers off when undocked; select the battery
position for portable use. `OFF` still permits charging with the screen off.
BTT specifies roughly 1.2 hours to charge and up to 30 minutes of portable use.
There is no controller battery gauge in this firmware; battery percentages on
sensor cards belong to those Hubitat sensors. See the
[BTT power guide](https://global.bttwiki.com/PandaTouch.html#wiring-and-power-connection).

## License

Released under the MIT License. See [LICENSE](LICENSE).
