#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Synthesize text to 16-bit PCM mono. Caller must free *pcm_out with free().
esp_err_t tts_synthesize(const char *text, uint16_t *sample_rate_out,
                          int16_t **pcm_out, size_t *samples_out);
