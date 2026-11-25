# Original firmware backup and recovery

Status: the complete original flash backup passed chip-side verification on 2026-11-06 UTC (2026-11-05 local). A second private copy also passed SHA-256 verification. Recovery flashing has not been tested.

## Verified original image

- Chip: ESP32-S3 revision 0.2, 8 MB PSRAM, 16 MB flash.
- Secure boot and flash encryption: disabled, observed before backup and again before replacement firmware flashing.
- Private original: `~/Library/Application Support/panda-hubitat/backups/<timestamp>-<chip-id>/original-16MiB.bin`.
- Second private copy: the same filename under `~/Library/Application Support/panda-hubitat/recovery-copy/<timestamp>-<chip-id>/`.
- Length: `16,777,216` bytes.
- Verification: all 128 chunk reads completed successfully, `esptool verify-flash` matched the full image against the chip digest, and both local copies matched their recorded SHA-256 checksums.

Neither image is in this repository. These are copies on the same Mac; they do not protect against loss of that Mac's storage.

## Connection

Use the magnetic dock's USB-C data connection. The official Panda Touch pinout
identifies a CH340K bridge, UART0 GPIO43/44, and automatic BOOT/reset lines.
After the WCH driver is installed and enabled, macOS should expose a
`/dev/cu.wchusbserial…` device. Port names can change; identify the current
port each session and do not select an unrelated serial device.

Install the WCH driver from its vendor source if macOS does not expose the
CH340K serial device. Follow the vendor instructions, including enabling its
driver extension when requested. Reconnect the dock and restart only if macOS
requests it. Identify the current serial device each session; entering ESP32
boot mode cannot create a missing serial driver.

## Identify and back up

Activate the pinned project environment first. Replace `PORT` with the identified WCH port, never an unrelated serial device.

```sh
.venv/bin/python -m serial.tools.list_ports -v
.venv/bin/python -m esptool --chip esp32s3 --port PORT --baud 115200 flash-id
.venv/bin/python -m esptool --chip esp32s3 --port PORT --baud 115200 get-security-info
.venv/bin/python scripts/backup_flash.py --port PORT
```

Confirm the dock is the intended device, chip ESP32-S3, and flash is 16 MB. If secure boot or flash encryption is enabled, stop for recovery analysis before replacing firmware. Do not alter eFuses or bypass these checks.

The backup tool is read-only. It uses 128 KiB chunks at 115200 baud and up to 20 attempts per chunk, following Tiger Touch's workaround for CH340K byte loss. esptool verifies each read with a chip digest; the tool additionally verifies the complete concatenated image against the chip and rechecks the MAC. It does not resume partial files automatically. A failed attempt remains `.partial` and is never certified as verified.

Default location: `~/Library/Application Support/panda-hubitat/backups/<UTC timestamp>-<chip MAC>/`. Directory mode is 0700 and files are 0600. The image contains personal settings and must remain private and outside Git. A full image is exactly **16,777,216 bytes**. Successful completion creates `original-16MiB.bin`, `SHA256SUMS`, `identity.txt`, and `VERIFIED.json`. Keep a second private copy before relying on this as your only recovery image.

## Recovery procedure (documented, not hardware-tested)

1. Connect the same device and confirm its MAC against `identity.txt` and `VERIFIED.json`.
2. In the private backup directory, run `shasum -a 256 -c SHA256SUMS`. Confirm image length with `stat -f %z original-16MiB.bin` on macOS. Stop on any mismatch.
3. Restore the complete image at address zero; no separate erase is necessary:

```sh
/absolute/project/path/.venv/bin/python -m esptool --chip esp32s3 --port PORT --baud 115200 write-flash 0 original-16MiB.bin
/absolute/project/path/.venv/bin/python -m esptool --chip esp32s3 --port PORT --baud 115200 verify-flash 0 original-16MiB.bin
```

4. Require successful verification, then power cycle and check the original firmware boots and retained settings work. If automatic bootloader entry fails, consult the Panda hardware documentation before opening the device; do not randomly short pins.

Do not restore another unit's flash image. Never pass `--force` or write eFuses as part of this workflow.

## Sources

- [Official Panda Touch pinout](https://github.com/bigtreetech/PandaTouch_IDF/blob/master/docs/pinout.md)
- [Tiger Touch chunked backup reference](https://github.com/TigerTag-Project/Tiger-Touch/blob/main/scripts/backup-flash.sh)
- [Tiger Touch installation and restore guide](https://github.com/TigerTag-Project/Tiger-Touch/blob/main/docs/INSTALL.md)
- [WCH macOS driver installation](https://github.com/WCHSoftGroup/ch34xser_macos)
