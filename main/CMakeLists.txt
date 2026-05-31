idf_component_register(
    SRCS "main.c" "wifi_manager.c" "tts_engine.c" "tts_httpd.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_server picotts freertos
)
