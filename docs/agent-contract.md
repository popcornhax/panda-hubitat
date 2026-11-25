# Agent contract

## Stack and layout

- ESPHome 2026.10.2, ESP-IDF, ESP32-S3, LVGL for the Panda Touch display.
- `firmware/panda-touch.yaml` is the maintainable starter UI and device config.
- `firmware/packages/panda-hardware.yaml` owns shared hardware configuration.
- `firmware/display-touch-test.yaml` is the minimal LVGL first-hardware smoke test.
- `scripts/bootstrap.sh`, `scripts/validate.sh`, and `scripts/build.sh` are the
  canonical local setup and firmware commands.
- `scripts/hubitat_discover.py` performs read-only host-side Maker API discovery;
  see `docs/hubitat.md` before changing it.
- Recovery and flash backup workflow is maintained separately in the recovery
  documentation and `scripts/backup_flash.py`.

## Architecture and boundaries

The firmware owns the local panel display, touch input, backlight, encrypted
ESPHome OTA, and the local Hubitat dashboard. It intentionally has no native
ESPHome API, Home Assistant dependency, MQTT dependency, or cloud service. The
dashboard starts from the ignored `firmware/devices.yaml` seed selection and
its generated configuration. The browser can import a server-verified Maker
API selection into an atomic NVS registry/layout bundle; only verified on/off
and dimmer setLevel commands are available. Sensors are read-only. `components/dashboard_config` serves a
local browser layout editor on port 80, intentionally without login. Runtime
Layout JSON can style and arrange selected device IDs, never grant commands.
Provisioning derives permissions from fresh Maker API metadata, not browser fields.
HTTP handlers persist validated data; only a safe reboot activates it. Never
mutate/free the boot-time model or wallpaper while LVGL holds references.
The Panda Touch expansion I2C bus on
GPIO3/4 remains unused; GT911 uses the dedicated GPIO2/1 bus.

## Hardware assumptions

Use an `esp32-s3-devkitc-1` board target, ESP-IDF, 16 MB flash, and octal 8 MB
PSRAM at 80 MHz. The LCD is 800x480 RGB565 in DE mode, `mipi_rgb` model `RPI`,
with a 14.8 MHz pixel clock, GPIO38 DE, GPIO5 PCLK, reset GPIO46, horizontal
porches 16/16 and vertical porches 32/32. RGB data is R `[6,7,8,9,10]`, G
`[11,12,13,14,15,16]`, B `[17,18,48,47,39]`. Display transforms start disabled.
GT911 uses SDA GPIO2, SCL GPIO1, reset GPIO41, IRQ GPIO40. LEDC backlight is
GPIO21. These mappings are sourced from the BTT pinout linked in README and
were physically confirmed by the user on this unit on 2026-11-05.
The UART logger uses GPIO43/44 through the Panda Touch CH340K dock.

## Credentials and generated files

`secrets.yaml` and local validation secrets are ignored. Never put real Wi-Fi,
OTA, or other credentials in firmware YAML, examples, or logs. Build products
and ESPHome's `.esphome/` state are generated artifacts and must not be checked
in. The separate flash-backup script writes only to the user-level application
support backup directory documented by its own help and recovery docs.

## Commands and completion evidence

- Bootstrap: `./scripts/bootstrap.sh` (creates `.venv` and installs pinned
  requirements).
- Validate: `./scripts/validate.sh` (runs ESPHome configuration validation).
- Build: `./scripts/build.sh` (compiles the primary firmware; no upload).
- Display/touch smoke config: `./scripts/build.sh --test-config`.
- Flash backup: `.venv/bin/python scripts/backup_flash.py --help` (read-only
  backup workflow; see recovery docs for exact device and port handling).
- Host tests: `.venv/bin/python -m unittest discover -s tests` (discovery and
  backup safety tests plus native dashboard state-machine checks).

Firmware completion evidence is successful config validation and compilation
for each affected config. A successful build does not verify updated UI pages,
sensor formatting, Wi-Fi loss behavior, or command feedback on the physical
panel; record each only after exercising it.

## Known limitations

The WCH USB-UART dock is installed, the ESP32-S3 identity is confirmed, and a
verified original flash backup plus a second private copy exist. The starter
firmware write and independent chip verification succeeded. Wi-Fi connection,
encrypted OTA, and reset into the OTA slot passed on hardware. The user
confirmed display appearance, touch input, and backlight response. Actual
Hubitat discovery and seven light command/state tests passed; the full
12-device dashboard is deployed through encrypted OTA using the dedicated
Maker API. The user confirmed stable scanout and text edges after the clock,
cache, PSRAM, and redraw fixes. Wi-Fi-loss and brightness-persistence tests
remain pending. Battery undocking blacks out the display while the ESP32
stays reachable; reboot restores the panel. This does not establish battery
health, and battery/display-transition diagnostics are in progress.

## Validated RGB runtime settings

Retain 14.8 MHz PCLK with `pclk_inverted: true`, 64-byte ESP32-S3 data-cache
lines, and `execute_from_psram: true`. The user confirmed stable scanout with
this profile. The tested 10 MHz clock produced a white screen and was reverted.
Avoid unnecessary redraws of identical labels and never overwrite a dimmer
thumb while it is being dragged. The rear battery switch is hardware-controlled;
no battery gauge is exposed by the current implementation.

## Runtime layout limits

Up to eight configurable pages plus Settings, 32 selected devices, and at most
six device slots per page (eight views per device). Uploads are bounded
to 16 KiB of versioned JSON and 192,000 bytes of 400x240 RGB565 wallpaper. The
wallpaper expands once to native 800x480 in PSRAM; retain the untransformed
LVGL image path. LVGL 2x scaling failed on this unit during on-device testing.
Keep image allocations in PSRAM and retain the startup crash guard. Font
presets use device-name/sensor sizes 22/18 or 28/22; compact On/Off badges use
14px and weather temperature uses 28px. Normal density is three columns and
Large is two, with bottom navigation. Preserve a separate 46px touch row
around the 6px dimmer track. See docs/dashboard-layout.md.
Keep `lvgl.widgets: []` in the main YAML: otherwise ESPHome inserts its demo
UI behind the transparent dashboard. The custom dashboard explicitly registers
its label/button/bar/slider/image dependencies and enables LVGL snapshots.
Screen capture runs on the main loop, never in the HTTP task; its persistent
PSRAM buffer must remain immutable while the HTTP response is being sent.

## Display sleep

Sleep is disabled until configured in the editor. A selected motion-capable
sensor is polled with priority about once per second. Fresh active motion,
touch, or the local web wake action wakes the black curtain and fades the
backlight to its previous brightness. The ESP32, RGB scanout, Wi-Fi, and polling
remain active; this is backlight sleep, not deep sleep or LCD power cycling.
Missing, offline, stale, or failed motion readings and disconnected Wi-Fi keep
the screen awake. Sleep does not persist an off state or change saved brightness.
