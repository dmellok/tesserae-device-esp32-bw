# tesserae-esp32-bw-client

Battery-powered **generic ESP32** firmware that's the embedded client for the [Tesserae](https://github.com/dmellok/tesserae) server — drives a [Waveshare 4.2" e-paper module](https://www.waveshare.com/4.2inch-e-paper-module.htm) (400×300, 1-bpp black/white, SSD1683 / UC8176).

It's the small-panel sibling of [tesserae-esp32-bin-client](https://github.com/dmellok/tesserae-esp32-bin-client) (ESP32-S3 + 13.3" 6-colour Spectra E6). **Everything but the panel driver, the pinout/panel constants, and the heartbeat `kind` is shared 1:1 with that firmware** — same wake state machine, captive portal, NVS schema, smart-sync wire contract, and release pipeline.

The device wakes on a timer, pulls a retained MQTT message containing a `.bin` frame URL, downloads the panel-native 1-bpp buffer, paints the panel, publishes a heartbeat with its battery/RSSI/IP, and goes back to deep sleep. WiFi credentials and MQTT broker details are provisioned on first boot via a SoftAP captive portal.

**Multiple panels:** each device has a `device_id` (default `esp32`) that prefixes its MQTT topics, so several panels can share one broker and one Tesserae server. Set it from the setup portal or bake it in at compile time with `MQTT_DEFAULT_DEVICE_ID` in `secrets.h`.

## Hardware

| Component | Detail |
| --- | --- |
| Board | Generic ESP32 dev board ("ESP-32S Type C, CP2102"). Original ESP32 (Xtensa LX6 dual-core), CP2102 USB-UART bridge (**not** native USB-JTAG), 4 MB flash, **no PSRAM** |
| Panel | Waveshare 4.2" e-paper, 400×300 native, 1-bpp B/W, SSD1683 (V2) or UC8176 (older) |
| Frame buffer | 15000 bytes (`400 × 300 / 8`), packed 8 px/byte, MSB = leftmost pixel, bit-set = white |
| Battery (optional) | Single-cell Li-Po via a resistor divider into an ADC1 pin; opt-in, see [`src/heartbeat.c`](src/heartbeat.c) |

### Wiring

The Waveshare 4.2" board doesn't fix a pinout — you wire it yourself. The pin assignments live in one place, [`include/app_config.h`](include/app_config.h), and are easy to edit. Defaults are the common Waveshare-ESP32-driver mapping on the VSPI bus:

| Panel pin | ESP32 GPIO | Notes |
| --- | --- | --- |
| BUSY | 4 | input; panel drives it during refresh |
| RST  | 16 | |
| DC   | 17 | |
| CS   | 5  | single chip-select (no `CS_M`/`CS_S` split like the 13.3") |
| CLK  | 18 | SPI SCLK |
| DIN  | 23 | SPI MOSI |
| VCC  | 3V3 | runs off the board's 3.3 V directly — no `EPD_PIN_PWR` gate |
| GND  | GND | |

Defaults avoid the flash pins (6–11) and the input-only pins (34–39). Pick any free pins you like; just keep BUSY on an input-capable GPIO.

### Panel variant (one-line swap)

[`include/app_config.h`](include/app_config.h) keeps panel **size + bit-depth + driver IC** in one block so swapping variants is a single change:

```c
#define EPD_DRIVER     EPD_DRIVER_SSD1683   /* current 4.2" V2 (default)   */
// #define EPD_DRIVER  EPD_DRIVER_UC8176    /* older 4.2" (IL0398)         */
#define EPD_WIDTH      400
#define EPD_HEIGHT     300
#define EPD_BPP        1                    /* 1, 2, or 4 per variant      */
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT * EPD_BPP) / 8)   /* 15000 */
```

`EPD_BUF_BYTES` flows through to the strict frame-size check in [`src/image_decoder.c`](src/image_decoder.c) and to the embedded splash blob size. The two controller variants differ in command set **and** BUSY polarity (SSD1683 is busy-high; UC8176 is busy-low) — both are handled in [`src/epd_driver.c`](src/epd_driver.c).

## Differences from the ESP32-S3 sibling

This is a straight fork; the panel-driver layer and a few constants are all that changed. The platform-specific edits beyond the panel:

- **No PSRAM.** Frame/scratch buffers (15000 bytes) live in internal RAM (`MALLOC_CAP_DEFAULT`); the download cap is 64 KB instead of 4 MB.
- **No USB-serial-JTAG.** The classic ESP32 can't auto-detect a connected laptop the way the S3 does (`usb_serial_jtag_is_connected()`), so the firmware defaults to the battery (deep-sleep) path. Use `DEV_DISABLE_SLEEP` in `secrets.h` to loop while iterating with the monitor open.
- **ADC calibration.** The classic ESP32 ships the line-fitting calibration scheme, not curve-fitting; [`src/heartbeat.c`](src/heartbeat.c) selects whichever the target SoC provides.
- **4 MB flash.** `partitions.csv` is a 3 MB factory app (vs 14 MB); `sdkconfig.defaults` drops all PSRAM/octal-flash settings and sets 4 MB.
- **`kind` = `esp32_bw_client`** in the heartbeat (vs `esp32_client`).

## Wake cycle

```
boot
  ├─ double-tap RESET?               → serve LAN settings editor + mDNS → sleep/reboot
  ├─ cold boot + WiFi creds present? → paint logo splash
  ├─ no WiFi creds anywhere?         → paint portal splash (logo + WPA QR) → captive portal → reboot
  ├─ STA connect fails?              → paint portal splash → captive portal → reboot
  ├─ STA connect
  ├─ NTP sync (cold boot only; RTC carries time across deep sleep)
  ├─ fetch retained frame URL        (no heartbeat yet)
  ├─ no URL? / URL unchanged?        → publish heartbeat → sleep
  ├─ render path: download → free WiFi → paint
  ├─ reconnect WiFi briefly          (for the post-paint heartbeat)
  ├─ publish heartbeat               (sleep_until = now + sleep_s, smart-sync feed)
  └─ deep sleep for the configured interval
```

The heartbeat lands at the **end** of each wake (post-paint on render wakes) so `sleep_until` reflects the actual sleep about to start — Tesserae's smart-sync scheduler uses it to JIT-render the next frame so the device sees a fresh artifact the moment it wakes.

### Smart-sync defensive guards (shipped from v0.1.0)

A router intercepting NTP can leave the RTC stuck at a wrong time forever. To avoid shipping a bad `sleep_until`:

- `ensure_time_synced()` **force-resyncs on any cold boot** (POWERON / EXT); timer wakes keep the cheap RTC-skip path. NTP wait is ≥ 5 s.
- Before publishing `sleep_until`, `now` is sanity-checked against `[1700000000, 2200000000]` (2023-11 … 2039-09) **and** cross-checked that `(sleep_until - now) ≈ next_sleep_s` within ±5 s.
- If either guard trips, `sleep_until` is **omitted** entirely (never `0`); the server falls back to its tolerance-window math.

`next_sleep_s` is always present.

## Build & flash

Requires [PlatformIO](https://platformio.org/). ESP-IDF 5.x and the Xtensa toolchain are pulled automatically on first build.

```bash
pio run                                              # build
pio run -e tesserae-esp32-bw-client -t upload \
    --upload-port /dev/cu.usbserial-...              # flash via the CP2102
pio device monitor                                   # 115200 baud
```

A clean build produces four artifacts under `.pio/build/tesserae-esp32-bw-client/`: `bootloader.bin`, `partitions.bin`, `firmware.bin`, `firmware.factory.bin`.

### Dev shortcut: `secrets.h`

To skip the captive portal during iteration, copy [include/secrets.example.h](include/secrets.example.h) to `include/secrets.h` and uncomment the `WIFI_DEFAULT_*` / `MQTT_DEFAULT_*` macros you want baked in. `secrets.h` is git-ignored. Precedence on each wake is NVS (set via portal) → `secrets.h` values → empty (portal triggers). Define `DEV_DISABLE_SLEEP` to loop instead of deep-sleeping while the monitor is open.

## Boot splashes

Two panel-native splashes are baked into the firmware (built from the brand PNG via [`tools/gen_splash.py`](tools/gen_splash.py), Floyd-Steinberg dithered to 1-bpp B/W, embedded as 15000-byte blobs via CMake's `EMBED_FILES`):

- **Logo splash** ([`assets/splash_logo.bin`](assets/splash_logo.bin)) — Tesserae logo centered on white. Painted on cold-boot when WiFi creds are present.
- **Portal splash** ([`assets/splash_portal.bin`](assets/splash_portal.bin)) — logo, a baked WPA-format QR (`WIFI:T:WPA;S:Tesserae-Setup;P:tesserae;;`), and the network/password labels. Scan with a phone to join the SoftAP without typing. Painted whenever the captive portal is about to come up.

To regenerate after editing the logo or `PROVISION_AP_SSID` / `PROVISION_AP_PASS`:

```bash
LOGO=/path/to/tesserae-splash-1024.png   # tesserae repo: static/brand/firmware/
python3 tools/gen_splash.py --logo "$LOGO" --out assets/splash_logo.bin \
    --width 400 --height 300 --bpp 1 --logo-size 200
python3 tools/gen_splash.py --logo "$LOGO" --out assets/splash_portal.bin \
    --width 400 --height 300 --bpp 1 \
    --logo-size 64 --logo-y 4 \
    --qr-data 'WIFI:T:WPA;S:Tesserae-Setup;P:tesserae;;' \
    --qr-size 160 --qr-y 72 \
    --label 'Network: Tesserae-Setup' --label 'Password: tesserae' \
    --label-y 244 --label-px 18
```

The script is parameterised (`--width / --height / --bpp`) so the same tool generates blobs for this client and the 6-colour 4-bpp sibling (`--bpp 4`). The packing for `--bpp 1` is MSB = leftmost pixel, bit-set (1) = white — verify against your panel's `epd_display` byte order if you change variants.

## Provisioning & settings

All settings (WiFi SSID + password, MQTT broker URI + credentials, `device_id`) are entered through one HTML form, reachable two ways:

**First-boot captive portal.** With no WiFi creds in NVS, the device brings up a SoftAP named `Tesserae-Setup` (password `tesserae`). Join it from a phone — the captive-portal prompt opens the Tesserae-styled form (teal `#0d8c7e`, card layout, WiFi scan picker, Show/Hide on the password fields). After submit the device reboots and joins your network.

**Always-on settings editor (double-tap RESET).** Once on the LAN, tap RESET twice in quick succession to serve the form on the STA IP, advertised over mDNS at `http://tesserae-<device_id>.local/`. (Relies on the board preserving RTC memory across resets, which is board-specific; fall back to the captive portal if it doesn't fire.)

## MQTT contract

Three topics under `tesserae/<device_id>/` (default `device_id` = `esp32`):

| Topic | Direction | Retained | QoS | Purpose |
|---|---|---|---|---|
| `tesserae/<device_id>/frame/bin` | server → device | yes | 1 | URL of the next `.bin` frame |
| `tesserae/<device_id>/config` | server → device | yes | 1 | Runtime settings (sleep interval) |
| `tesserae/<device_id>/status` | device → broker | yes | 1 | Wake-time heartbeat + LWT |

A non-retained **last-will** `{"state":"offline"}` is registered on the status topic so the broker flags ungraceful disconnects without overwriting the retained heartbeat.

### Frame payload

```json
{ "url": "http://192.168.1.10:8000/renders/3f7a91b2c4e5d6f8.bin" }
```

**Wire format** of the file at that URL: raw, headerless, exactly **15000 bytes** (`400 × 300 / 8`), scanline order, 8 pixels per byte, MSB = leftmost pixel, bit-set (1) = white, bit-clear (0) = black. The buffer is panel-native — no rotation/decode/resize. If the body isn't exactly 15000 bytes the firmware logs `frame size mismatch` and sleeps without painting.

### Manual test push

```bash
mosquitto_pub -t tesserae/esp32/frame/bin -r \
  -m '{"url":"http://192.168.1.10:8000/renders/test.bin"}'
```

### Status (heartbeat) payload

Published once per wake at the **end** of the cycle:

```json
{
  "battery_mv": 3950, "battery_pct": 67, "rssi": -42,
  "ip": "192.168.50.234", "fw_version": "0.1.0",
  "kind": "esp32_bw_client", "panel_w": 400, "panel_h": 300,
  "sleep_interval_s": 900, "next_sleep_s": 900,
  "wake_reason": "timer", "sleep_until": 1759264800
}
```

`next_sleep_s` is always present; `sleep_until` is omitted when wall-clock time isn't trusted (see the smart-sync guards above). `panel_w`/`panel_h` pull from `EPD_WIDTH`/`EPD_HEIGHT`, so they track the variant automatically. Together `next_sleep_s` + `sleep_until` feed Tesserae's smart-sync scheduler; after 3 on-time wakes the device flips to **trusted**.

> **Server-side note — the `esp32_bw_client` kind.** The heartbeat's `kind` lets Tesserae pre-fill the Register chip for a discovered device. As of this writing the Tesserae catalog enumerates `esp32_client`, `pi_bin_client`, `pi_png_client`, `trmnl_client` — there is **no** 1-bpp/4.2" kind yet, and one-click Register rejects an unknown kind. To complete the loop, add a `devices/esp32_bw_client/` entry **and** a renderer that packs the composition into a 1-bpp raw `.bin` (the existing `esp32_bin` renderer emits 4-bpp 6-colour bytes, which this firmware's strict 15000-byte decoder rejects) to the [tesserae](https://github.com/dmellok/tesserae) repo. Until then the device still **appears** under Discovered with kind/panel pre-filled, but frame rendering and Register need that server-side companion.

## Flashing a pre-built release

Each release tag ships four artifacts plus a checksum file:

```
bootloader.bin         second-stage bootloader
partitions.bin         partition table (matches partitions.csv)
firmware.bin           the application image
firmware.factory.bin   combined image (bootloader + partitions + firmware at 0x0)
SHA256SUMS             SHA-256 of each .bin above
```

Verify, then flash the combined image to offset 0:

```bash
shasum -a 256 -c SHA256SUMS
esptool.py --chip esp32 --port /dev/cu.usbserial-... \
    write_flash 0x0 firmware.factory.bin
```

Or flash the three pieces at their native offsets (what `pio run -t upload` does):

```bash
esptool.py --chip esp32 --port /dev/cu.usbserial-... \
    write_flash 0x1000  bootloader.bin \
                0x8000  partitions.bin \
                0x10000 firmware.bin
```

**Finding the port.** The CP2102 bridge shows up on macOS as `/dev/cu.usbserial-<serial>` (or `/dev/cu.SLAB_USBtoUART`), on Linux as `/dev/ttyUSB<n>`. `pio device list` lists candidates.

**Erase NVS first** for a clean state on a previously-provisioned board (otherwise stored WiFi creds + MQTT URI + `device_id` survive):

```bash
esptool.py --chip esp32 --port /dev/cu.usbserial-... erase_flash
```

The release pipeline at [`tools/release.sh`](tools/release.sh) rebuilds the current `FW_VERSION` from a clean tree, **moves `secrets.h` aside and post-build scans `firmware.bin` for any string literal from it** (the two layers that stop credentials leaking into a public artifact), stages artifacts under `release/<version>/`, computes SHA-256, tags `vX.Y.Z`, and creates a GitHub Release.

## Project layout

```
tesserae-esp32-bw-client/
├── platformio.ini             # esp32dev, 4 MB flash, partitions, FW_VERSION
├── partitions.csv             # 3 MB factory app + NVS (4 MB flash)
├── sdkconfig.defaults         # no PSRAM, mbedTLS cert bundle, MQTT 3.1.1
├── assets/                    # 15000-byte 1-bpp splash blobs
├── include/
│   ├── app_config.h           # pinout + panel variant + all behaviour tunables
│   └── secrets.example.h      # template for local credential overrides
├── src/
│   ├── main.c                 # wake state machine + smart-sync guards
│   ├── epd_driver.{c,h}       # 4.2" SSD1683 / UC8176 driver  ← panel-specific
│   ├── heartbeat.{c,h}        # battery / RSSI / IP / kind=esp32_bw_client JSON
│   ├── wifi_manager.{c,h}     # NVS-backed STA connect
│   ├── provisioning.{c,h}     # captive portal + LAN settings editor + mDNS
│   ├── mqtt_config.{c,h}      # NVS broker URI / device_id + legacy migration
│   ├── mqtt_handler.{c,h}     # single-shot subscribe + heartbeat + LWT
│   ├── image_fetcher.{c,h}    # HTTP download into internal RAM
│   └── image_decoder.{c,h}    # strict 15000-byte panel-native pass-through
└── tools/
    ├── gen_splash.py          # parameterised dither/pack (--width/--height/--bpp)
    └── release.sh             # clean-tree build + secrets leak scan + gh release
```

## Credits

The wake state machine, captive-portal provisioning, NVS schema, smart-sync contract, and battery curve are forked from [tesserae-esp32-bin-client](https://github.com/dmellok/tesserae-esp32-bin-client). The 4.2" panel driver in [src/epd_driver.c](src/epd_driver.c) is ported from Waveshare's official `EPD_4in2_V2.c` (SSD1683) and `EPD_4in2.c` (UC8176) demos.

## License


AGPL-3.0-or-later. See [LICENSE](LICENSE).

