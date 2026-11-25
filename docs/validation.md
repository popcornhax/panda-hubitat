# Bring-up validation

Record only observed results here. A successful firmware build does not verify
the panel or connectivity on the physical unit.

## Stage A: hardware identity and original firmware backup

- [x] Dock identified as a WCH CH340K USB-UART bridge; ESPHome work is kept
  separate from the serial backup session.
- [x] Device identity read as ESP32-S3 revision 0.2, 8 MB PSRAM, 16 MB flash,
  and the chip identity matched the backup; secure boot and flash encryption
  reported disabled.
- [x] Complete the full 16 MiB original flash backup and verify the chip digest
  and SHA256.
- [x] Verify a second private copy of the original image.

## Stage B: basic panel firmware

- [x] ESPHome 2026.10.2 validated the isolated LVGL display/touch smoke config.
- [x] ESPHome compiled the smoke config for ESP32-S3 / ESP-IDF (about 1.01 MB;
  GPIO46 strapping-pin warning, no compile errors).
- [x] ESPHome validated and compiled the main `panda-touch.yaml` (about 1.02
  MB) with the actual local secrets file and UART0 logging at 115200 baud for
  the CH340K dock.
- [x] Main artifacts are available at
  `firmware/.esphome/build/panda-touch/build/firmware.factory.bin` and
  `firmware/.esphome/build/panda-touch/build/firmware.ota.bin`.
- [x] After the Stage A backup was verified, flash the main firmware using the
  authorized recovery procedure.
- [x] Factory image write completed and matched its write hash; independent
  full-image verification passed.
- [x] Initial serial boot-log capture completed with setup successful and Wi-Fi
  connected; the panel resolved by its local mDNS name.
- [x] Encrypted OTA upload completed; after reboot, the device was reachable
  over ESPHome OTA again.
- [x] Captured post-OTA boot showed setup complete and Wi-Fi connected, with no
  crash or component errors during the observed 15-second window.
- [x] User confirmed the dark test screen is upright and stable (2026-11-05 local).
- [x] User confirmed touch input increases the on-screen touch counter.
- [x] User confirmed the brightness slider changes the backlight.
- [ ] Confirm the on-screen Wi-Fi status after the panel is visually checked.
- [ ] Test Wi-Fi loss and restoration behavior.

## Stage C: Hubitat discovery and mapping

- [x] Read-only discovery utility and synthetic security tests are implemented.
- [x] Run read-only discovery against the original shared Maker API (46 devices).
- [x] Create dedicated local-only Maker API `Panda Touch Dashboard` (942),
  authorize exactly the twelve selected devices, and disable cloud, mode, and
  HSM control. Shared instances are unchanged.
- [x] Verify all twelve device details through the dedicated API; the excluded
  lock endpoint returns HTTP 404.
- [x] Select 12 devices: seven lights plus five genuine read-only sensors.
- [x] Exclude Office LED Strip ID 911 after confirming it is offline; exclude
  security, blinds, heater, and office equipment devices.
- [x] Verify all seven selected lights with actual Maker API commands followed
  by fresh state reads; restore every original state.
- [x] Confirm command HTTP 200 alone does not confirm device state; one
  separately tested offline device produced no confirming event and was
  removed from the mapping.
- [x] Add temperature, weather, motion, presence, water readings and battery
  warnings without sensor command controls.
- [x] Measure weather response body at 8,984 bytes (chunked); transport cap is
  32 KiB.

## Stage D: dashboard

- [x] Build Home, Lights, Climate & Sensors, and Settings pages from the private
  selected-device mapping.
- [x] Add disabled-until-fresh light controls, command confirmation, and
  Wi-Fi/Hubitat health status.
- [x] Compile the final 12-device dashboard firmware with ESPHome 2026.10.2 / ESP-IDF.
- [ ] Verify all four pages and sensor readings visually on the panel.
- [ ] Recheck Wi-Fi loss/restoration and user-facing stale/error states.

## RGB scanout investigation

- [x] The user observed vertical display shifting after deploying the dashboard;
  the earlier standalone touch-test image was stable.
- [x] The deployed build used ESP-IDF 5.5.6 with 32-byte ESP32-S3 data-cache
  lines. ESPHome's MIPI RGB driver configures PSRAM frame storage and a
  ten-line internal bounce buffer.
- [x] Set `CONFIG_ESP32S3_DATA_CACHE_LINE_64B` for the bounce-buffer path,
  matching Espressif's RGB LCD guidance.
- [x] The rebuilt ESP-IDF configuration selects 64-byte cache lines.
- [x] User reports the 64-byte cache-line update improved the display, but
  scanout still shows a large shift and noisy upper-half artifacts.
- [x] A 10 MHz PCLK image was tried; the user reported a solid white screen.
- [x] Restored the previously deployed 14.8 MHz PCLK with the 64-byte cache
  setting retained; rebuild confirmed both values in generated configuration.
- [x] Enabled ESPHome's `framework.advanced.execute_from_psram` option; the
  generated ESP-IDF configuration enables XIP, instruction fetch, and read-only
  data from octal PSRAM at 80 MHz, with 64-byte cache lines. The chip was
  identified as having 8 MB PSRAM.
- [x] Avoided unchanged LVGL label/color updates and skipped server-driven
  dimmer thumb updates while a slider is being dragged.
- [x] Full firmware build completed with these changes; all 22 host tests pass.
- [x] Deploy the XIP/redraw-guard build through encrypted OTA and confirm the
  device returns online.
- [x] Enabled inverted pixel clock (`pclk_inverted: true`) to match the BTT
  panel configuration; rebuilt main firmware and verified the generated driver
  call with PCLK still at 14.8 MHz.
- [x] Match BTT pixel-clock polarity with `pclk_inverted: true`, retaining
  14.8 MHz, PSRAM execution, and 64-byte cache lines; encrypted OTA passed.
- [x] User described the corrected scanout and text edges as "perfect".
- [x] Replace unsupported middle-dot separators with ASCII `|`; compile and
  all 22 host tests passed, and encrypted OTA completed.

## Runtime editor and touch UI refresh

- [x] Compile the larger-text, full-card toggle, slim-slider, dynamic-page UI
  and local browser editor; all 27 host tests pass.
- [x] Browser harness: change title/font, reorder cards, save, and verify the
  resulting JSON; select and preview an image and submit its upload.
- [x] Encrypted OTA and on-device editor access without login.
- [x] Reject unknown IDs, invalid fonts, trailing data, oversized requests,
  and cross-origin writes while preserving the existing layout.
- [x] Persist six pages with repeated devices, reversed card order, a new
  accent color, and Large text across restart.
- [x] The first LVGL 2x wallpaper-scaling build failed to return online after
  image activation; ESPHome safe-mode OTA allowed recovery without USB.
- [x] Replace runtime scaling with a native-size PSRAM bitmap. The persisted
  image and six-page layout returned online after the recovery update.
- [x] Upload and replace two 192,000-byte backgrounds, restart after each,
  and compare downloaded bytes exactly.
- [x] Restore the original three dashboard pages plus Settings and remove
  the test wallpaper. These checks sent no light commands.
- [x] Open the deployed editor in a browser, restore defaults through its
  confirmation dialog, reload, and verify the original configuration online.
- [ ] Visually review the refreshed screen, full-card toggles, slider touch
  area, photo rendering, and swipes on the physical panel.
- [ ] Deliberately fault-inject the wallpaper startup crash fallback.

## Device screen capture

- [x] Add read-only `/api/screen.bmp`, with main-loop LVGL capture and a
  persistent PSRAM buffer, bounded wait, and retry after failed/timed-out work.
- [x] Compile firmware and pass all 27 host tests.
- [x] Capture a valid 800x480 top-down RGB565 BMP directly from the device.
- [x] The first capture revealed ESPHome demo widgets behind transparent
  dashboard regions. Suppress the implicit demo with explicit `widgets: []`
  and register the custom dashboard's LVGL widget dependencies directly.
- [x] Deploy the corrected firmware, fetch repeated 768,066-byte BMPs, and
  visually inspect the decoded PNG: dashboard regions have the intended dark
  background and no demo controls. Physical LCD noise is outside this capture.

## Dense visual refresh

- [x] Replace the sidebar with bottom navigation and show six normal-size
  tiles per screen; Large text uses four tiles in two columns.
- [x] Add verified font icons, a small vector weather mark, warm lit-light
  styling, compact state badges, and separate temperature/detail typography.
- [x] Compile and pass all 27 host tests. Retain the validated RGB timings,
  native wallpaper path, command confirmation, and slider gesture isolation.
- [x] Apply six reviewed Home favorites, then capture the actual Home,
  sensor, and Large-text views over Wi-Fi. Sensor captures show all five
  devices, including offline/last-reported state and low battery readings.
- [x] Restore the normal six-tile Home configuration after inspection; no
  Maker API control commands were sent during these checks.
- [ ] Check the new bottom navigation and denser slider rows by physical touch.

## Local-time greeting

- [x] Test the morning/day/evening/night boundaries in the native C++ helper;
  all 27 host tests pass and the firmware compiles.
- [x] Deploy through encrypted OTA and capture `Good evening` alongside the
  device's local 18:00 clock. Custom first-page titles remain literal; the
  default `Good day` title is automatic. Theme and brightness are unchanged.

## Weather detail view

- [x] Inspect the actual Maker API weather fields and confirm three days of
  highs/lows, forecast conditions and precipitation chances are exposed.
- [x] Add a read-only weather tile action and Back overlay, with explicit
  missing-data placeholders and the source report timestamp.
- [x] Build and pass all 27 host tests, then deploy through encrypted OTA.
- [x] Invoke the tile's LVGL click event through a forecast capture request;
  inspect the real 800x480 result against the reported weather attributes.
- [x] Reject an unknown capture view, fetch the ordinary Home capture again,
  and verify the layout configuration was unchanged. No device commands sent.
- [ ] Check the Weather tile and Back button with physical touch.

## Runtime device selection and screen sleep (2026-11-06)

- [x] Validate and build the expanded firmware: 145,755 bytes static RAM,
  1,290,375 bytes flash (15.9% of the OTA application slot).
- [x] Run 31 host tests, including actual native layout/catalogue parsing,
  32-device/eight-page bounds, command classification, and sleep decisions.
- [x] Deploy through encrypted OTA and discover all 12 dedicated Maker API
  devices on the panel. Reject an unknown ID without changing saved settings.
- [x] Persist an 11-device/eight-page bundle, restart, then reimport the twelfth
  device and restore the original three pages without rebuilding firmware.
- [x] Fix discovery allocation failure identified by the retained crash
  backtrace: concurrent 32 KiB response buffers now allocate explicitly in
  PSRAM through a nullable allocator, rather than fragmented internal RAM.
- [x] Temporarily use Entry Motion with a 30-second timeout: verify a black
  800x480 capture, zero backlight, and continued HTTP reachability while asleep.
- [x] Web wake restores the original brightness with observed intermediate
  levels 0, 0.311, 0.943, and 1 during the configured 800 ms fade.
- [x] Restore all 12 devices, original pages, and disabled sleep with no wake
  sensor selected, so the owner can choose in the editor. Keep the background.
- [x] Live Chrome browser flow: scan, select all 12, inspect motion choices,
  add/remove a page, stage selection, save/restart, and reload with no JS errors.
- [ ] Physically trigger the owner's selected motion sensor and verify wake;
  physically check that the first waking touch does not toggle a light.

These checks used read-only Maker metadata/state requests and local panel
configuration/backlight actions; no household device control commands were sent.
The 32-device bound is tested in the native codec, not with 32 physical devices.
