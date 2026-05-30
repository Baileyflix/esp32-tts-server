// Placeholder TTS: sine tone proportional to text length.
// Replace tts_synthesize() body with ESP-ADF TTS or another engine.
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tts_engine.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define SAMPLE_RATE   16000
#define TONE_HZ       440
#define MS_PER_CHAR   70
#define MIN_MS        400
#define MAX_MS        8000
#define FADE_MS       20

static const char *TAG = "tts-engine";

esp_err_t tts_synthesize(const char *text, uint16_t *sample_rate_out,
                          int16_t **pcm_out, size_t *samples_out) {
    if (!text || !sample_rate_out || !pcm_out || !samples_out)
        return ESP_ERR_INVALID_ARG;

    size_t chars = strlen(text);
    uint32_t ms = (uint32_t)(chars * MS_PER_CHAR);
    if (ms < MIN_MS) ms = MIN_MS;
    if (ms > MAX_MS) ms = MAX_MS;

    size_t n = (size_t)SAMPLE_RATE * ms / 1000;
    size_t fade = (size_t)SAMPLE_RATE * FADE_MS / 1000;

    // Prefer PSRAM for large buffers
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(n * sizeof(int16_t));
    if (!buf) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double amp = 1.0;
        if (i < fade)
            amp = (double)i / (double)fade;
        else if (i > n - fade)
            amp = (double)(n - i) / (double)fade;
        buf[i] = (int16_t)(amp * 20000.0 * sin(2.0 * M_PI * TONE_HZ * t));
    }

    ESP_LOGI(TAG, "%.40s -> %d samples (%d ms)", text, (int)n, (int)ms);
    *sample_rate_out = SAMPLE_RATE;
    *pcm_out = buf;
    *samples_out = n;
    return ESP_OK;
}
