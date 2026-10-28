# Public half of the OTA signing key (agent/ota_sign.py keygen). Not secret,
# so it is committed. Without it the firmware still builds -- CI included --
# but refuses every OTA image: failing closed rather than accepting unsigned
# updates. After adding the key, `./dev.sh reconfigure` (or fullclean) so
# CMake notices it.
set(ota_pubkey "${CMAKE_CURRENT_LIST_DIR}/ota_pubkey.pem")
if(NOT EXISTS "${ota_pubkey}")
    set(ota_pubkey "${CMAKE_CURRENT_BINARY_DIR}/ota_pubkey.pem")
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        message(WARNING "main/ota_pubkey.pem not found: this build will refuse "
                        "all OTA updates. Run agent/ota_sign.py keygen.")
        file(WRITE "${ota_pubkey}" "")
    endif()
endif()

idf_component_register(
    SRCS "discus_main.c" "discus_ui.c" "discus_net.c" "discus_ota.c" "fonts/lv_font_clock_128.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client json nvs_flash esp_wifi esp_netif esp_event esp_timer
             app_update esp_app_format esp_partition mbedtls
    EMBED_TXTFILES "${ota_pubkey}"
)
