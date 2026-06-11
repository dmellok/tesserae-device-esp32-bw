/*
 * Project-wide tunables. Edit here, not scattered across .c files.
 *
 * For local credential overrides (dev shortcut to bypass the captive
 * portal), copy include/secrets.example.h to include/secrets.h and fill
 * in the WIFI_DEFAULT_* / MQTT_DEFAULT_* macros there. secrets.h is
 * git-ignored.
 */
#pragma once

#include <stdint.h>

/* Pull in user-local overrides if they exist. Falls through silently if
 * secrets.h hasn't been created -- the build doesn't depend on it. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* ================================================================== */
/* Panel + wiring -- EDIT THIS BLOCK to match your board and variant.  */
/* ================================================================== */
/*
 * Panel: Waveshare 4.2" e-paper module (400x300).
 *   https://www.waveshare.com/4.2inch-e-paper-module.htm
 *
 * The 4.2" module has a SINGLE controller (no CS_M / CS_S half-split
 * like the 13.3"), and runs off the dev board's 3.3 V directly -- there
 * is no separate panel-power enable rail, so EPD_PIN_PWR is gone too.
 * That leaves SIX GPIOs to wire. The Waveshare board doesn't fix the
 * pinout; you choose the pins and reassign them here.
 *
 * Defaults below are the well-known Waveshare-ESP32-driver mapping
 * (BUSY=4, RST=16, DC=17, CS=5, CLK=18, DIN=23) on the VSPI bus. They
 * avoid the flash pins (6-11) and the input-only pins (34-39). Pick any
 * free pins you like; just keep BUSY on an input-capable GPIO.
 */
#define EPD_PIN_SCLK   18
#define EPD_PIN_MOSI   23   /* panel "DIN"                              */
#define EPD_PIN_CS      5   /* single chip-select (active low)          */
#define EPD_PIN_DC     17
#define EPD_PIN_RST    16
#define EPD_PIN_BUSY    4   /* input; panel drives it during refresh    */

#define EPD_SPI_HOST   SPI3_HOST          /* VSPI on the classic ESP32   */
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* ---- Variant selector: panel size + bit depth + driver IC ----------
 *
 * Swapping panel variants is meant to be a one-line change here:
 *
 *   Default V2 module (400x300, 1-bpp B/W):   SSD1683  +  EPD_BPP 1
 *   Older "IL0398" module  (400x300, 1-bpp):  UC8176   +  EPD_BPP 1
 *
 * EPD_BPP feeds EPD_BUF_BYTES, the strict image_decoder length check,
 * and the embedded splash blob size, so it must match what your dither
 * pipeline (tools/gen_splash.py --bpp) and the Tesserae renderer emit.
 */
#define EPD_DRIVER_SSD1683   1   /* current Waveshare 4.2" V2             */
#define EPD_DRIVER_UC8176    2   /* older 4.2" (IL0398 / "C" command set) */
#ifndef EPD_DRIVER
#define EPD_DRIVER     EPD_DRIVER_SSD1683
#endif

#define EPD_WIDTH      400
#define EPD_HEIGHT     300
#define EPD_BPP        1                  /* 1, 2, or 4 per panel variant */
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT * EPD_BPP) / 8)  /* 15000 */

/* Palette indices for epd_clear() and diagnostics. For 1-bpp B/W a
 * "color" is a single bit; the panel convention used by the driver and
 * tools/gen_splash.py is bit-set (1) = white, bit-clear (0) = black, so
 * a full byte of EPD_COL_WHITE is 0xFF. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1

/* ------------------------------------------------------------------ */
/* Application behavior                                               */
/* ------------------------------------------------------------------ */

/* Reported in the status heartbeat so Tesserae can show which firmware
 * each device is running. The authoritative value is set in platformio.ini
 * (build_flags = -DFW_VERSION=\"x.y.z\"); this is just a fallback so the
 * file still compiles outside PlatformIO. */
#ifndef FW_VERSION
#define FW_VERSION         "0.0.0-dev"
#endif

/* How long to deep-sleep between MQTT checks. 15 minutes is a good
 * trade for a 6-color panel that itself takes ~30s to refresh. */
#define SLEEP_INTERVAL_S   (15 * 60)

/* Cap on how long we'll wait for a retained MQTT message after
 * subscribing, before giving up and going back to sleep. */
#define MQTT_WAIT_MS       8000

/* WiFi STA connect attempt: retry count and per-attempt timeout. */
#define WIFI_CONNECT_RETRIES   5
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* How long the provisioning portal stays up after a failed STA
 * connect. Power gets burned the whole time, so don't make it huge. */
#define PROVISION_PORTAL_TIMEOUT_S  (10 * 60)

/* SoftAP credentials shown to the user during provisioning. */
#define PROVISION_AP_SSID    "Tesserae-Setup"
#define PROVISION_AP_PASS    "tesserae"     /* >= 8 chars or use open AP */

/* ------------------------------------------------------------------ */
/* WiFi / MQTT compile-time defaults                                  */
/* ------------------------------------------------------------------ */
/* Precedence on each wake:
 *     NVS (set via portal)  >  these defaults  >  empty (portal triggers)
 *
 * secrets.h may override any of these; otherwise WiFi defaults to empty
 * (no auto-connect) and MQTT defaults to placeholders that will fail
 * gracefully if the user hasn't run the portal yet. */

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID   ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS   ""
#endif

#ifndef MQTT_DEFAULT_URI
#define MQTT_DEFAULT_URI    "mqtt://homeassistant.local:1883"
#endif
/* Per-device topic namespace is tesserae/<device_id>/{frame/bin,config,status}.
 * device_id defaults to "esp32" (matches Tesserae's built-in esp32_client kind);
 * a second physical panel just needs a different id, set via the portal. */
#ifndef MQTT_DEFAULT_DEVICE_ID
#define MQTT_DEFAULT_DEVICE_ID  "esp32"
#endif
#ifndef MQTT_DEFAULT_USER
#define MQTT_DEFAULT_USER   ""
#endif
#ifndef MQTT_DEFAULT_PASS
#define MQTT_DEFAULT_PASS   ""
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID      "tesserae-esp32-bin-1"
#endif

/* Dev shortcut: define DEV_DISABLE_SLEEP (in secrets.h) to swap the
 * 15-min deep sleep for a short delay + software restart loop. Useful
 * while iterating with the serial monitor open. Cold-boot splash only
 * fires on power-on / RESET button, not on the software restart, so
 * each iteration is fast. */
#ifndef DEV_LOOP_INTERVAL_S
#define DEV_LOOP_INTERVAL_S 10
#endif

/* NVS namespaces / keys */
#define NVS_NS_WIFI        "wifi"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"

#define NVS_NS_MQTT          "mqtt"
#define NVS_KEY_MQTT_URI     "uri"
#define NVS_KEY_MQTT_TOPIC   "topic"      /* legacy; read once at migration, then erased */
#define NVS_KEY_MQTT_DEVICE_ID "device_id"
#define NVS_KEY_MQTT_USER    "user"
#define NVS_KEY_MQTT_PASS    "pass"

/* device_id charset/length: 2-32 chars, [a-z0-9_-], must start with a letter.
 * Keep in sync with Tesserae's device.json validation. */
#define DEVICE_ID_MIN_LEN   2
#define DEVICE_ID_MAX_LEN   32

#define NVS_NS_STATE       "state"
#define NVS_KEY_LAST_HASH  "last_hash"   /* sha256 of last rendered URL */
#define NVS_KEY_SLEEP_S    "sleep_s"     /* user-configured deep-sleep duration */

/* Sanity bounds on sleep interval. The lower bound stops a publisher
 * accidentally turning the device into a 1-Hz spinner; the upper bound
 * is just "this is probably a bug". */
#define SLEEP_INTERVAL_MIN_S  30
#define SLEEP_INTERVAL_MAX_S  (7 * 24 * 60 * 60)
