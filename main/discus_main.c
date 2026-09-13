/*
 * discus -- a round-display dashboard on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C.
 *
 * The device is a renderer. All data collection (host stats, weather, quotes,
 * headlines, Claude token usage) happens in agent/discus_agent.py on the Mac;
 * this side does one plaintext HTTP GET every few seconds and draws the result.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "discus_data.h"
#include "discus_net.h"
#include "discus_ui.h"
#include "discus_ota.h"

static const char *TAG = "discus";

/* Credentials come from main/wifi_creds.h when present (gitignored, so the
 * password never reaches version control), otherwise from menuconfig. */
#if defined(__has_include)
#  if __has_include("wifi_creds.h")
#    include "wifi_creds.h"
#  endif
#endif

#ifndef DISCUS_WIFI_SSID_OVERRIDE
#  define DISCUS_WIFI_SSID_OVERRIDE CONFIG_DISCUS_WIFI_SSID
#endif
#ifndef DISCUS_WIFI_PASSWORD_OVERRIDE
#  define DISCUS_WIFI_PASSWORD_OVERRIDE CONFIG_DISCUS_WIFI_PASSWORD
#endif

#define LVGL_FOREVER    ((uint32_t)-1)
#define POLL_INTERVAL_MS 3000

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        discus_ui_set_status("wifi: reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    if (strlen(DISCUS_WIFI_SSID_OVERRIDE) == 0) {
        ESP_LOGW(TAG, "no SSID configured");
        discus_ui_set_status("wifi: not configured");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Ordinary esp_wifi calls -- esp_wifi_remote forwards them over SDIO to
     * the onboard ESP32-C6, which is where the radio actually lives. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     DISCUS_WIFI_SSID_OVERRIDE,     sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, DISCUS_WIFI_PASSWORD_OVERRIDE, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    discus_ui_set_status("wifi: connecting");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) {
        discus_ui_set_status("");
        return ESP_OK;
    }
    discus_ui_set_status("wifi: timeout");
    return ESP_ERR_TIMEOUT;
}

static void poll_task(void *arg)
{
    LV_UNUSED(arg);
    static discus_data_t data;          /* static: too big for a task stack */
    int64_t last_ok_us = 0;

    while (true) {
        EventBits_t bits = s_wifi_events
            ? xEventGroupGetBits(s_wifi_events) : 0;

        static int polls = 0;
        polls++;
        if (polls <= 3) {
            ESP_LOGI(TAG, "poll #%d: wifi_bits=0x%02x", polls, (unsigned)bits);
        }

        if (bits & WIFI_CONNECTED_BIT) {
            esp_err_t err = discus_net_fetch(CONFIG_DISCUS_AGENT_URL, &data);
            if (polls <= 3 || err != ESP_OK) {
                ESP_LOGI(TAG, "poll #%d fetch: %s", polls,
                         err == ESP_OK ? "OK" : esp_err_to_name(err));
            }
            if (err == ESP_OK) {
                last_ok_us = esp_timer_get_time();

                /* Reaching the agent is our definition of "this build works".
                 * Until this runs, a freshly OTA'd image is still on
                 * probation and a reboot would roll it back. */
                discus_ota_confirm_running();

                /* Adopt the agent's wall clock, but only when we have
                 * genuinely drifted: re-setting every 3s would make a
                 * seconds display stutter backwards. Between corrections the
                 * ESP32 keeps its own time, so the clock carries on ticking
                 * even while the Mac is asleep. */
                if (data.clock.epoch > 0) {
                    time_t now = time(NULL);
                    long drift = (long)(data.clock.epoch - (long long)now);
                    if (drift > 2 || drift < -2) {
                        struct timeval tv = {
                            .tv_sec = (time_t)data.clock.epoch, .tv_usec = 0
                        };
                        settimeofday(&tv, NULL);
                        ESP_LOGI(TAG, "clock set (drift was %lds)", drift);
                    }
                }
                discus_ui_set_utc_offset(data.clock.utc_offset_s, data.clock.tz);
                discus_ui_set_brightness(data.display.brightness);
                discus_ui_update(&data);
            }
        }

        /* Numbers that have stopped moving must not keep looking authoritative
         * -- if the Mac sleeps overnight you would otherwise walk up to a
         * confident "cpu 34%" that is eight hours old. */
        int age_s = last_ok_us
            ? (int)((esp_timer_get_time() - last_ok_us) / 1000000)
            : -1;
        discus_ui_set_stale(age_s);
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "starting display (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    if (bsp_display_lock(LVGL_FOREVER) != ESP_OK) {
        ESP_LOGE(TAG, "cannot build UI without the LVGL lock");
        return;
    }
    discus_ui_create();
    bsp_display_unlock();

    bsp_display_backlight_on();
    ESP_LOGI(TAG, "display up");

    wifi_start();   /* non-fatal: the panels still render, just without data */

    xTaskCreatePinnedToCore(poll_task, "discus_poll", 6144, NULL, 4, NULL, 1);
    discus_ota_start();
    ESP_LOGI(TAG, "polling %s every %d ms", CONFIG_DISCUS_AGENT_URL, POLL_INTERVAL_MS);
}
