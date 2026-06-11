/*
 * Waveshare 4.2" e-paper (400x300, 1-bpp B/W) driver.
 *
 * Two controller variants are supported, selected by EPD_DRIVER in
 * app_config.h:
 *   - EPD_DRIVER_SSD1683 : current Waveshare 4.2" V2 module. Command
 *                          set ported from Waveshare's EPD_4in2_V2.c.
 *   - EPD_DRIVER_UC8176  : older 4.2" (IL0398) module. Command set
 *                          ported from Waveshare's EPD_4in2.c / GxEPD2.
 *
 * Single controller (one CS), single 3.3 V rail (no power-enable GPIO).
 * The public API is identical to the 13.3" sibling so main.c / splash.c
 * are unchanged across the two firmwares.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_config.h"

/* One-time SPI bus + GPIO setup. Safe to call multiple times. */
esp_err_t epd_port_init(void);

/* Full panel reset + init sequence. Call after epd_port_init() and
 * before epd_clear()/epd_display(). */
void epd_init(void);

/* Fill the entire panel with a single palette colour (EPD_COL_BLACK /
 * EPD_COL_WHITE). Diagnostic; not used in the production paint path. */
void epd_clear(uint8_t color);

/* Push a full-frame buffer. `image` must point to EPD_BUF_BYTES (15000)
 * bytes laid out as 300 rows of 50 packed bytes -- 8 pixels per byte,
 * MSB = leftmost pixel, bit-set (1) = white, bit-clear (0) = black. */
void epd_display(const uint8_t *image);

/* Paint a black/white test pattern (horizontal bands). The user-facing
 * sanity check -- if the bands render with crisp ink and correct
 * top-to-bottom orientation, the panel + driver + wiring are healthy. */
void epd_show_color_bars(void);

/* Send the panel's deep-sleep command. After this, epd_init() must be
 * called again before the next refresh. */
void epd_sleep(void);
