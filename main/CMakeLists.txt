idf_component_register(
    SRCS "discus_main.c" "discus_ui.c" "discus_net.c" "discus_ota.c" "fonts/lv_font_clock_128.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client json nvs_flash esp_wifi esp_netif esp_event esp_timer
             esp_https_ota app_update esp_app_format
)
