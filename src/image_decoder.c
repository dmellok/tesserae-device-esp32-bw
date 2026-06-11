#include "image_decoder.h"
#include "app_config.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "decode";

/* v1 wire format: panel-native 1-bpp packed buffer, exactly EPD_BUF_BYTES.
 * The Tesserae renderer for this device kind guarantees this length; any
 * deviation means either a half-downloaded body or a server bug, and
 * feeding garbage to the panel costs a full refresh of power for a useless
 * render. Hard-fail. */
esp_err_t image_decode_to_frame(const fetched_image_t *src,
                                const char *url,
                                uint8_t **out_frame)
{
    (void)url;
    if (!src || !out_frame) return ESP_ERR_INVALID_ARG;
    *out_frame = NULL;

    if (src->len != EPD_BUF_BYTES) {
        ESP_LOGE(TAG,
            "frame size mismatch: got %u bytes, expected %u "
            "(content-type='%s'); refusing to paint",
            (unsigned)src->len, (unsigned)EPD_BUF_BYTES,
            src->content_type);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "raw panel-native frame (%u bytes)", (unsigned)src->len);
    /* No PSRAM on this board; the 15000-byte frame lives in internal RAM. */
    uint8_t *frame = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_DEFAULT);
    if (!frame) return ESP_ERR_NO_MEM;
    memcpy(frame, src->data, EPD_BUF_BYTES);
    *out_frame = frame;
    return ESP_OK;
}
