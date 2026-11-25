# Local dashboard layout editor

The Panda Touch serves an editor at `http://panda-touch.local/` and at its
current Wi-Fi address. The editor changes the dashboard layout without a
firmware rebuild. Uploads take effect after the device restarts. Device selection can also be imported from Maker API in the browser. Layout
files arrange, relabel, and style the selected devices; the device derives
command permissions from verified Maker API metadata.

The editor supports up to eight pages, page titles and navigation labels, an
ordered device-card list per page, device label overrides, nine theme colors,
normal or large compiled fonts, and one background image. Images are cropped
in the browser to 400 × 240 and stored as RGB565. The editor can download or
upload a version 1 JSON layout, remove the image, or restore default pages for the current selection and remove the image.

Normal text shows six tiles in a three-column grid, with 22px device names and
18px readings. Large uses two columns with 28px names and 22px sensor readings.
The bottom navigation shows verified category icons beside page labels. With
one to four pages, the tabs stretch beside a fixed-width, icon-only Settings
button. With five to eight pages, the first strip has four tabs and a fixed
forward arrow; the second has a fixed back arrow, the remaining tabs, and
Settings. Both strips use the same page-label font size. Lit lights use warm
amber accents; sensor tiles use cool accents and weather has a separate 28px
temperature reading. Sliders retain a generous touch area around a slim track.
The compiled default Home page supports six favorites. Save applies the layout
and restarts the panel; reload the editor after the restart if restoring
defaults or removing an image. Supported device IDs can be imported without
recompiling firmware. New widget types still require firmware changes.

The default first-page title `Good day` changes with local America/Chicago
time: morning 05:00–11:59, day 12:00–16:59, evening 17:00–21:59, and night
22:00–04:59. A custom title remains literal. Hubitat's HTTP Date header supplies
the clock; daylight-saving rules are applied locally. Theme colors remain as
configured; there is no automatic day/night color or brightness change.

Use **Capture the current panel screen** in the editor to download a BMP of the
live 800 × 480 display. The endpoint is `GET /api/screen.bmp`; it captures the
active LVGL screen on the firmware main loop and returns a top-down RGB565 BMP.
If another capture is running or the main loop does not finish within five
seconds, the endpoint reports a temporary service error and can be retried.

Tap a weather tile to open a read-only three-day forecast; **Back** returns to
the same dashboard page. The view shows reported highs/lows, conditions,
precipitation chances, current readings, wind, sunrise/sunset, and the weather
driver's report timestamp. Missing attributes show unavailable values rather
than invented forecasts. Data comes from the existing Maker API weather device
and continues to use the normal read-only polling cadence. The current driver
exposes `forecastHigh/Low`, corresponding `1`/`2` fields, `forecast_text`
variants, `percentPrecip`, `PoP1/2`, `wind_string`, and local sunrise/sunset.
`GET /api/screen.bmp?view=weather` previews that same view without changing the
panel's current page; the unqualified capture endpoint remains unchanged.

## Adding devices

1. Add devices to the dedicated Hubitat Maker API instance.
2. Open the editor and choose **Scan Maker API**.
3. Check the devices to include, then choose **Use selected devices**.
4. Arrange cards on pages, add pages as needed, and **Save & restart**.

Up to 32 selected devices and eight custom pages plus Settings are supported, with six slots per page. Discovery
inspects at most 64 Maker API devices. Supported types are switches, dimmers,
motion, presence, contact, water, weather, and read-only temperature, humidity,
illuminance, power, or voltage sensors. Unsupported devices show a reason.
Only verified light on/off/setLevel controls are enabled; sensor commands and
security/safety actuators are not imported. Saving rechecks every selected
device and atomically stores the registry and layout together. A failed
verification preserves the old setup. The compiled selection is the fallback
for a device without a saved registry. Reset keeps the imported selection.

## Sleep and motion wake

Under **Screen sleep**, choose a motion-capable sensor, enable automatic sleep,
and select an idle timeout (30–3600 seconds) and fade duration (0–3000 ms).
Sleep is initially disabled. Active motion keeps the display awake; fresh
inactive motion allows the idle timer to expire. The backlight turns off over
a black screen while Wi-Fi and Hubitat polling continue. Motion is checked
about once per second, subject to Hubitat/network response time. Touch wakes
without activating the tile underneath; **Wake screen now** is also available.
The previous brightness is restored without overwriting saved brightness.
Missing/offline/stale readings or disconnected Wi-Fi keep the screen awake.
This does not power-cycle the LCD or put the ESP32 into deep sleep.

## Network boundary

The editor intentionally has no login, following the local-network-only use
case. HTTP is unencrypted. Use it only on a trusted home LAN; do not forward
port 80 or expose the device to guest Wi-Fi or the public internet. Write
requests require a same-origin `Origin` plus a custom request header, and the
server accepts only its local IP or `panda-touch` hostname. Those checks help
prevent browser cross-site requests and DNS rebinding; they do not authenticate
other software already on the LAN.

The service starts only after Wi-Fi connects and retries if startup fails.
Successful config/image writes are committed to NVS before the server replies.
The running LVGL layout and wallpaper stay unchanged until the delayed safe
restart, so LVGL never reads buffers while the HTTP task replaces them.
At boot, the 400x240 bitmap expands once to 800x480 in PSRAM; LVGL draws it
without a runtime scaling transform. A watchdog/panic during the first 30
seconds of startup removes the wallpaper on the next boot while retaining
the layout. Clean restarts and ordinary power cycles preserve it. This crash
fallback has been compiled and reviewed, but was not deliberately fault-injected.

## JSON format

See [`examples/dashboard-layout.json`](../examples/dashboard-layout.json).
The example uses placeholder IDs `1`, `2`, and `3`; replace them with IDs from
the layout downloaded from your own device. The upload is limited to 16 KiB.
The version 1 object has these keys (sleep is optional for older files):

- `version`: integer `1`.
- `theme`: `background`, `panel`, `text`, `muted`, `accent`, `active`,
  `updating`, `warning`, and `info` as integers from `0` through `16777215`
  (`0xFFFFFF`). Older files with the first five colors use defaults for the
  four semantic colors.
- `font`: `normal` or `large`, mapped to the compiled LVGL font sizes.
- `pages`: one to eight objects with unique `label` (1–10 printable ASCII
  characters), `title` (1–24), and up to six `devices` IDs. IDs must occur in
  the saved device selection. An ID can appear on different
  pages, but not twice on one page.
- `labels`: up to 32 `{ "id", "label" }` overrides for selected devices;
  labels are 1–32 printable ASCII characters.

- `sleep`: `enabled` (boolean), `motion_device_id` (selected motion-capable ID,
  or zero when disabled), `timeout_seconds` (30–3600), and `fade_ms` (0–3000).

Unknown fields, invalid types, unknown IDs, duplicate IDs within a page, and
out-of-range values are rejected without replacing the saved layout. A
background image is sent separately as exactly 192,000 RGB565 little-endian
bytes and is never embedded in JSON.
