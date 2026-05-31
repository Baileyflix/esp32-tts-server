#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "tts_engine.h"
#include "tts_httpd.h"

static const char *TAG = "tts-main";

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(wifi_manager_start());

    ret = wifi_manager_wait_connected(30000);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "WiFi not connected — HTTP server still starting");

    ESP_ERROR_CHECK(tts_engine_init());
    ESP_ERROR_CHECK(tts_httpd_start());
    ESP_LOGI(TAG, "Ready");
}
