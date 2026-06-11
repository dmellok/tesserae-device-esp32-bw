/*
 * Local credential overrides — copy to secrets.h (which is git-ignored)
 * and uncomment the lines you want to bake into the build.
 *
 * Precedence on each wake:
 *   1. Values in NVS (set via the captive portal)  -- always win if present
 *   2. Values defined here                          -- fallback for empty NVS
 *   3. Otherwise                                    -- captive portal triggers
 *
 * Use these to bypass the portal during development -- no NVS, no phone-tap
 * dance every time you flash a fresh board. Leaving any of them undefined is
 * fine; the portal will collect whatever's missing.
 *
 * IMPORTANT: secrets.h itself must NEVER be committed. .gitignore is wired
 * to ignore it; double-check before pushing.
 */
#pragma once

/* ---- WiFi ---------------------------------------------------------- */
// #define WIFI_DEFAULT_SSID  "your-network"
// #define WIFI_DEFAULT_PASS  "your-password"     /* "" for open networks */

/* ---- MQTT ---------------------------------------------------------- */
// #define MQTT_DEFAULT_URI          "mqtt://192.168.1.50:1883"   /* mqtts:// for TLS */
// #define MQTT_DEFAULT_USER         "broker-user"     /* leave undefined if open */
// #define MQTT_DEFAULT_PASS         "broker-pass"
//
// device_id is the topic-namespace prefix: tesserae/<device_id>/{frame/bin,
// config,status}. Defaults to "esp32" (matches Tesserae's built-in
// esp32_client kind). Set a distinct id per physical panel on a shared broker.
// #define MQTT_DEFAULT_DEVICE_ID    "esp32"
//
// #define MQTT_CLIENT_ID            "tesserae-esp32-bin-1"     /* unique per device on the broker */

/* ---- Dev mode ------------------------------------------------------ */
/* Unlike the ESP32-S3 sibling, the classic ESP32 has no USB-serial-JTAG
 * peripheral (this board's USB is a CP2102 UART bridge), so the firmware
 * CANNOT auto-detect a connected laptop. It defaults to the battery path
 * (real deep sleep). Define DEV_DISABLE_SLEEP while iterating with the
 * serial monitor open: it swaps the deep sleep for a short delay + software
 * restart loop so you see logs every cycle without power-cycling. */
// #define DEV_DISABLE_SLEEP
// #define DEV_LOOP_INTERVAL_S 10                /* defaults to 10 */

/* Opposite of DEV_DISABLE_SLEEP: force ALWAYS deep-sleep. On this board
 * that's already the default, so this is mostly documentation of intent
 * (e.g. a wall-powered install). The CP2102 UART port stays enumerated
 * across sleeps; esptool's RTS/DTR reset still wakes the chip for
 * re-flashing. Mutually exclusive with DEV_DISABLE_SLEEP. */
// #define DEV_FORCE_SLEEP
