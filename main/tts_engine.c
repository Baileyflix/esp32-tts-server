#include <stdlib.h>
#include <string.h>
#include "tts_engine.h"
#include "picotts.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "tts-engine";

// 8 seconds of 16 kHz mono 16-bit — enough for any station name
#define MAX_PCM_BYTES (8 * PICOTTS_SAMPLE_FREQ_HZ * sizeof(int16_t))

// Normalize buf in-place so the peak reaches TARGET (leave quieter audio alone).
#define NORMALIZE_TARGET 29491  // ~90% of INT16_MAX

static void normalize(int16_t *buf, size_t n) {
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) peak = v;
    }
    if (peak < 256 || peak >= NORMALIZE_TARGET) return;
    int32_t gain = ((int32_t)NORMALIZE_TARGET << 16) / peak;
    for (size_t i = 0; i < n; i++) {
        int32_t s = ((int32_t)buf[i] * gain) >> 16;
        buf[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : (int16_t)s);
    }
}

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_done;

// Valid only while s_mutex is held by tts_synthesize:
static int16_t          *s_collect_buf;
static size_t            s_collect_samples;
static volatile bool     s_expecting_idle;

static void output_cb(int16_t *samples, unsigned count) {
    size_t cap = MAX_PCM_BYTES / sizeof(int16_t);
    if (s_collect_samples + count > cap)
        count = cap - s_collect_samples;
    memcpy(s_collect_buf + s_collect_samples, samples, count * sizeof(int16_t));
    s_collect_samples += count;
}

static void idle_cb(void) {
    if (s_expecting_idle) {
        s_expecting_idle = false;
        xSemaphoreGive(s_done);
    }
}

esp_err_t tts_engine_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_done  = xSemaphoreCreateBinary();
    if (!s_mutex || !s_done)
        return ESP_ERR_NO_MEM;

    if (!picotts_init(5, output_cb, 1)) {
        ESP_LOGE(TAG, "picotts_init failed");
        return ESP_FAIL;
    }
    picotts_set_idle_notify(idle_cb);
    ESP_LOGI(TAG, "PicoTTS ready");
    return ESP_OK;
}

esp_err_t tts_synthesize(const char *text, uint16_t *rate_out,
                          int16_t **pcm_out, size_t *samples_out) {
    if (!text || !rate_out || !pcm_out || !samples_out)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_collect_buf = heap_caps_malloc(MAX_PCM_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_collect_buf) s_collect_buf = malloc(MAX_PCM_BYTES);
    if (!s_collect_buf) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_collect_samples = 0;
    s_expecting_idle  = true;

    picotts_add(text, strlen(text) + 1);

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(30000)) != pdPASS) {
        ESP_LOGW(TAG, "synthesis timeout");
        s_expecting_idle = false;
        free(s_collect_buf);
        s_collect_buf = NULL;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_TIMEOUT;
    }

    size_t n = s_collect_samples;

    int16_t *result = heap_caps_malloc(n * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) result = malloc(n * sizeof(int16_t));
    if (result)
        memcpy(result, s_collect_buf, n * sizeof(int16_t));

    free(s_collect_buf);
    s_collect_buf = NULL;
    xSemaphoreGive(s_mutex);

    if (!result)
        return ESP_ERR_NO_MEM;

    normalize(result, n);

    ESP_LOGI(TAG, "%.40s -> %d samples @ %d Hz (%d ms)",
             text, (int)n, PICOTTS_SAMPLE_FREQ_HZ,
             (int)(n * 1000 / PICOTTS_SAMPLE_FREQ_HZ));

    *rate_out    = PICOTTS_SAMPLE_FREQ_HZ;
    *pcm_out     = result;
    *samples_out = n;
    return ESP_OK;
}
