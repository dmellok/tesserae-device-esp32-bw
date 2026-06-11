/*
 * Waveshare 4.2" e-paper (400x300, 1-bpp B/W) driver.
 *
 * Command sequences are ported from Waveshare's official demos:
 *   SSD1683 (V2) -> EPD_4in2_V2.c
 *   UC8176       -> EPD_4in2.c  (cross-checked against GxEPD2_420)
 * The init blobs are panel-specific; don't "clean them up."
 *
 * Buffer convention (matches tools/gen_splash.py and the strict
 * image_decoder): 50 bytes/row * 300 rows = 15000 bytes, MSB = leftmost
 * pixel, bit-set (1) = white, bit-clear (0) = black.
 */
#include "epd_driver.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* Row stride in bytes (8 px/byte, no padding since 400 % 8 == 0). */
#define EPD_ROW_BYTES   ((EPD_WIDTH + 7) / 8)        /* 50 */

#if (EPD_ROW_BYTES * EPD_HEIGHT) != EPD_BUF_BYTES
#  error "EPD_BUF_BYTES does not match a 1-bpp 400x300 frame; check EPD_BPP"
#endif

/* BUSY polarity differs by controller: SSD1683 drives BUSY HIGH while
 * busy; UC8176 drives it LOW while busy. */
#if EPD_DRIVER == EPD_DRIVER_SSD1683
#  define EPD_BUSY_ACTIVE  1
#elif EPD_DRIVER == EPD_DRIVER_UC8176
#  define EPD_BUSY_ACTIVE  0
#else
#  error "EPD_DRIVER must be EPD_DRIVER_SSD1683 or EPD_DRIVER_UC8176"
#endif

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* ---------- low-level SPI / pin wrappers ---------- */

static esp_err_t spi_tx_raw(const uint8_t *data, size_t len)
{
    /* Chunk so we never exceed the bus max_transfer_sz and other tasks
     * aren't starved on a long frame push. */
    const size_t CHUNK = 4096;
    spi_transaction_t t;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off > CHUNK) ? CHUNK : (len - off);
        memset(&t, 0, sizeof(t));
        t.length = n * 8;
        t.tx_buffer = data + off;
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi tx fail at off=%u: %s",
                     (unsigned)off, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void send_cmd(uint8_t cmd)
{
    gpio_set_level(EPD_PIN_DC, 0);
    spi_tx_raw(&cmd, 1);
}

static void send_data_buf(const uint8_t *buf, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);
    spi_tx_raw(buf, len);
}

static void send_data_byte(uint8_t b)
{
    send_data_buf(&b, 1);
}

/* Block until the panel reports idle. A 4.2" full refresh is ~1-4 s, so
 * we only warn after 30 s -- anything beyond that is a stuck panel. */
static void wait_idle(void)
{
    int ticks = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY) == EPD_BUSY_ACTIVE) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!warned && ++ticks >= 3000) {
            ESP_LOGW(TAG, "BUSY stuck after 30 s -- check wiring / variant");
            warned = true;
        }
    }
}

static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---------- public API ---------- */

esp_err_t epd_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_DC, 1);

    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_PIN_MOSI,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_BYTES,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = EPD_PIN_CS,   /* single device; let SPI drive CS */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

#if EPD_DRIVER == EPD_DRIVER_SSD1683
/* ===================== SSD1683 (Waveshare 4.2" V2) ===================== */

static void set_window_full(void)
{
    /* RAM X range in bytes; RAM Y range in lines. */
    send_cmd(0x44);                       /* SET_RAM_X start/end           */
    send_data_byte(0x00);
    send_data_byte((EPD_WIDTH - 1) >> 3);
    send_cmd(0x45);                       /* SET_RAM_Y start/end           */
    send_data_byte(0x00);
    send_data_byte(0x00);
    send_data_byte((EPD_HEIGHT - 1) & 0xFF);
    send_data_byte((EPD_HEIGHT - 1) >> 8);

    send_cmd(0x4E);                       /* SET_RAM_X counter -> 0        */
    send_data_byte(0x00);
    send_cmd(0x4F);                       /* SET_RAM_Y counter -> 0        */
    send_data_byte(0x00);
    send_data_byte(0x00);
}

void epd_init(void)
{
    hw_reset();
    wait_idle();

    send_cmd(0x12);                       /* SWRESET                       */
    wait_idle();

    send_cmd(0x21);                       /* Display update control        */
    send_data_byte(0x40);
    send_data_byte(0x00);

    send_cmd(0x3C);                       /* BorderWaveform                */
    send_data_byte(0x05);

    send_cmd(0x11);                       /* Data entry mode: X++ Y++      */
    send_data_byte(0x03);

    set_window_full();
    wait_idle();

    ESP_LOGI(TAG, "init complete (SSD1683)");
}

static void write_ram_and_refresh(const uint8_t *image)
{
    set_window_full();
    send_cmd(0x24);                       /* WRITE_RAM (B/W)               */
    send_data_buf(image, EPD_BUF_BYTES);

    send_cmd(0x22);                       /* Display update control 2      */
    send_data_byte(0xF7);                 /* full update, load LUT from OTP */
    send_cmd(0x20);                       /* Activate update sequence      */
    wait_idle();
    ESP_LOGI(TAG, "refresh done");
}

void epd_sleep(void)
{
    send_cmd(0x10);                       /* DEEP_SLEEP                    */
    send_data_byte(0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
}

#else /* EPD_DRIVER == EPD_DRIVER_UC8176 */
/* ===================== UC8176 / IL0398 (older 4.2") =================== */

void epd_init(void)
{
    hw_reset();

    send_cmd(0x01);                       /* POWER SETTING                 */
    send_data_byte(0x03);
    send_data_byte(0x00);
    send_data_byte(0x2B);
    send_data_byte(0x2B);

    send_cmd(0x06);                       /* BOOSTER SOFT START            */
    send_data_byte(0x17);
    send_data_byte(0x17);
    send_data_byte(0x17);

    send_cmd(0x04);                       /* POWER ON                      */
    wait_idle();

    send_cmd(0x00);                       /* PANEL SETTING (LUT from OTP)  */
    send_data_byte(0xBF);
    send_data_byte(0x0D);

    send_cmd(0x30);                       /* PLL CONTROL                   */
    send_data_byte(0x3C);

    send_cmd(0x61);                       /* RESOLUTION SETTING            */
    send_data_byte(EPD_WIDTH >> 8);       /* 0x01                          */
    send_data_byte(EPD_WIDTH & 0xFF);     /* 0x90 (400)                    */
    send_data_byte(EPD_HEIGHT >> 8);      /* 0x01                          */
    send_data_byte(EPD_HEIGHT & 0xFF);    /* 0x2C (300)                    */

    send_cmd(0x82);                       /* VCOM DC SETTING               */
    send_data_byte(0x12);

    send_cmd(0x50);                       /* VCOM AND DATA INTERVAL        */
    send_data_byte(0x97);                 /* white border                  */

    ESP_LOGI(TAG, "init complete (UC8176)");
}

static void write_ram_and_refresh(const uint8_t *image)
{
    /* Old buffer (0x10) cleared to white so the OTP full-refresh waveform
     * drives every pixel from a known state; new buffer (0x13) holds the
     * frame. */
    uint8_t *white = heap_caps_malloc(EPD_ROW_BYTES, MALLOC_CAP_DEFAULT);
    send_cmd(0x10);
    if (white) {
        memset(white, 0xFF, EPD_ROW_BYTES);
        for (int r = 0; r < EPD_HEIGHT; r++) send_data_buf(white, EPD_ROW_BYTES);
        free(white);
    } else {
        uint8_t ff = 0xFF;
        for (int i = 0; i < EPD_BUF_BYTES; i++) send_data_byte(ff);
    }

    send_cmd(0x13);                       /* DATA START TRANSMISSION 2     */
    send_data_buf(image, EPD_BUF_BYTES);

    send_cmd(0x12);                       /* DISPLAY REFRESH               */
    vTaskDelay(pdMS_TO_TICKS(100));
    wait_idle();
    ESP_LOGI(TAG, "refresh done");
}

void epd_sleep(void)
{
    send_cmd(0x02);                       /* POWER OFF                     */
    wait_idle();
    send_cmd(0x07);                       /* DEEP SLEEP                    */
    send_data_byte(0xA5);
    vTaskDelay(pdMS_TO_TICKS(100));
}

#endif /* EPD_DRIVER */

/* ---------- variant-independent helpers ---------- */

void epd_display(const uint8_t *image)
{
    write_ram_and_refresh(image);
}

void epd_clear(uint8_t color)
{
    uint8_t fill = (color == EPD_COL_WHITE) ? 0xFF : 0x00;
    uint8_t *buf = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte clear buffer",
                 (unsigned)EPD_BUF_BYTES);
        return;
    }
    memset(buf, fill, EPD_BUF_BYTES);
    write_ram_and_refresh(buf);
    free(buf);
}

void epd_show_color_bars(void)
{
    /* Four horizontal bands (black/white/black/white) -- enough to
     * confirm both inks and top-to-bottom orientation on a B/W panel. */
    uint8_t *buf = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte test buffer",
                 (unsigned)EPD_BUF_BYTES);
        return;
    }
    const int bands = 4;
    const int band_h = EPD_HEIGHT / bands;
    for (int r = 0; r < EPD_HEIGHT; r++) {
        int band = r / band_h;
        if (band >= bands) band = bands - 1;
        uint8_t v = (band % 2 == 0) ? 0x00 : 0xFF;  /* black, white, ... */
        memset(buf + (size_t)r * EPD_ROW_BYTES, v, EPD_ROW_BYTES);
    }
    write_ram_and_refresh(buf);
    free(buf);
}
