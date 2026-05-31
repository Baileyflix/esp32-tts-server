#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "tts_httpd.h"
#include "tts_engine.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "tts-httpd";

// --- WAV cache ---------------------------------------------------------------

#define CACHE_MAX 32

typedef struct {
    char    *text;
    uint8_t *wav;
    size_t   len;
} cache_entry_t;

static cache_entry_t     s_cache[CACHE_MAX];
static int               s_cache_n;
static SemaphoreHandle_t s_cache_mutex;

static const uint8_t *cache_get(const char *text, size_t *len_out) {
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < s_cache_n; i++) {
        if (strcmp(s_cache[i].text, text) == 0) {
            *len_out = s_cache[i].len;
            const uint8_t *p = s_cache[i].wav;
            xSemaphoreGive(s_cache_mutex);
            return p;
        }
    }
    xSemaphoreGive(s_cache_mutex);
    return NULL;
}

static void cache_put(const char *text, const uint8_t *wav, size_t len) {
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (s_cache_n >= CACHE_MAX) {
        xSemaphoreGive(s_cache_mutex);
        return;
    }
    char *key = strdup(text);
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(len);
    if (!key || !buf) {
        free(key);
        free(buf);
        xSemaphoreGive(s_cache_mutex);
        return;
    }
    memcpy(buf, wav, len);
    s_cache[s_cache_n++] = (cache_entry_t){ key, buf, len };
    xSemaphoreGive(s_cache_mutex);
}

// --- helpers -----------------------------------------------------------------

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(const char *src) {
    size_t len = strlen(src);
    char *dst = malloc(len + 1);
    if (!dst) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            continue;
        } else if (src[i] == '%' && i + 2 < len) {
            int h = hex_val(src[i + 1]), l = hex_val(src[i + 2]);
            if (h >= 0 && l >= 0) {
                dst[j++] = (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
    return dst;
}

static void write_wav_header(uint8_t *buf, uint32_t samples,
                              uint16_t rate, uint16_t bits) {
    uint32_t data_sz  = samples * (bits / 8u);
    uint32_t byte_rate = rate * (bits / 8u);
    memcpy(buf,       "RIFF", 4);
    *(uint32_t *)(buf +  4) = 36 + data_sz;
    memcpy(buf +  8,  "WAVE", 4);
    memcpy(buf + 12,  "fmt ", 4);
    *(uint32_t *)(buf + 16) = 16;
    *(uint16_t *)(buf + 20) = 1;         // PCM
    *(uint16_t *)(buf + 22) = 1;         // mono
    *(uint32_t *)(buf + 24) = rate;
    *(uint32_t *)(buf + 28) = byte_rate;
    *(uint16_t *)(buf + 32) = bits / 8u; // block align
    *(uint16_t *)(buf + 34) = bits;
    memcpy(buf + 36,  "data", 4);
    *(uint32_t *)(buf + 40) = data_sz;
}

// --- handlers ----------------------------------------------------------------

static esp_err_t tts_handler(httpd_req_t *req) {
    char qs[512];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK || !qs[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
        return ESP_FAIL;
    }

    char raw[512];
    if (httpd_query_key_value(qs, "text", raw, sizeof(raw)) != ESP_OK || !raw[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'text' parameter");
        return ESP_FAIL;
    }

    char *text = url_decode(raw);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    // Cache hit — serve immediately
    size_t cached_len = 0;
    const uint8_t *cached = cache_get(text, &cached_len);
    if (cached) {
        ESP_LOGI(TAG, "TTS (cached): \"%s\"", text);
        free(text);
        httpd_resp_set_type(req, "audio/wav");
        httpd_resp_send(req, (const char *)cached, (ssize_t)cached_len);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "TTS: \"%s\"", text);

    uint16_t rate;
    int16_t *pcm = NULL;
    size_t samples = 0;
    esp_err_t ret = tts_synthesize(text, &rate, &pcm, &samples);
    if (ret != ESP_OK) {
        free(text);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Synthesis failed");
        return ESP_FAIL;
    }

    size_t data_sz = samples * sizeof(int16_t);
    size_t total   = 44 + data_sz;

    uint8_t *wav = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) wav = malloc(total);
    if (!wav) {
        free(text);
        free(pcm);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    write_wav_header(wav, (uint32_t)samples, rate, 16);
    memcpy(wav + 44, pcm, data_sz);
    free(pcm);

    cache_put(text, wav, total);
    free(text);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_send(req, (char *)wav, (ssize_t)total);
    free(wav);
    return ESP_OK;
}

static esp_err_t health_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// --- start -------------------------------------------------------------------

esp_err_t tts_httpd_start(void) {
    s_cache_mutex = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = CONFIG_TTS_HTTP_PORT;
    cfg.stack_size     = 8192;
    cfg.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t uri_tts = {
        .uri     = "/tts",
        .method  = HTTP_GET,
        .handler = tts_handler,
    };
    static const httpd_uri_t uri_health = {
        .uri     = "/health",
        .method  = HTTP_GET,
        .handler = health_handler,
    };
    httpd_register_uri_handler(server, &uri_tts);
    httpd_register_uri_handler(server, &uri_health);

    ESP_LOGI(TAG, "Listening on port %d", cfg.server_port);
    return ESP_OK;
}
