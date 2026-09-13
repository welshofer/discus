/*
 * Over-the-air updates.
 *
 * The agent serves two endpoints: /firmware.json (a manifest carrying the
 * version string ESP-IDF baked into the image from `git describe`) and
 * /firmware.bin. The device compares the manifest version against its own
 * esp_app_get_description()->version, so there is no separate version file
 * anyone has to remember to bump -- rebuild, and the version changes.
 *
 * Anti-bricking: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE means a newly flashed
 * image boots in ESP_OTA_IMG_PENDING_VERIFY and is only confirmed once it has
 * actually talked to the agent. Push firmware that cannot join WiFi or reach
 * the Mac and the bootloader reverts to the previous slot on next boot --
 * which matters a great deal when the device lives in another room.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "cJSON.h"

#include "discus_ota.h"

static const char *TAG = "discus_ota";

#define MANIFEST_MAX      1024
#define FIRST_CHECK_MS    60000      /* settle before touching flash */
#define CHECK_INTERVAL_MS 300000     /* 5 minutes */

typedef struct {
    char *buf;
    int   len;
} rx_t;

static esp_err_t manifest_event(esp_http_client_event_t *evt)
{
    rx_t *ctx = (rx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        if (ctx->len + evt->data_len >= MANIFEST_MAX) return ESP_FAIL;
        memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
        ctx->len += evt->data_len;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

/* Fills `version` and `url`; returns ESP_OK only on a complete manifest. */
static esp_err_t fetch_manifest(char *version, size_t vn, char *url, size_t un)
{
    char buf[MANIFEST_MAX];
    buf[0] = '\0';
    rx_t ctx = { .buf = buf, .len = 0 };

    esp_http_client_config_t cfg = {
        .url = CONFIG_DISCUS_OTA_URL,
        .event_handler = manifest_event,
        .user_data = &ctx,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) return err;
    if (status != 200)  return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;

    esp_err_t out = ESP_FAIL;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (cJSON_IsString(v) && cJSON_IsString(u)) {
        strncpy(version, v->valuestring, vn - 1); version[vn - 1] = '\0';
        strncpy(url, u->valuestring, un - 1);     url[un - 1]     = '\0';
        out = ESP_OK;
    }
    cJSON_Delete(root);
    return out;
}

void discus_ota_confirm_running(void)
{
    static bool done = false;
    if (done) return;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        done = true;
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "update confirmed good; rollback cancelled");
        } else {
            ESP_LOGW(TAG, "failed to confirm image");
        }
    }
    done = true;
}

static void ota_task(void *arg)
{
    (void)arg;
    const esp_app_desc_t *self = esp_app_get_description();
    ESP_LOGI(TAG, "running version %s, manifest %s",
             self->version, CONFIG_DISCUS_OTA_URL);

    vTaskDelay(pdMS_TO_TICKS(FIRST_CHECK_MS));

    while (true) {
        char version[48] = "", url[192] = "";

        if (fetch_manifest(version, sizeof version, url, sizeof url) == ESP_OK) {
            if (strcmp(version, self->version) == 0) {
                ESP_LOGD(TAG, "up to date (%s)", version);
            } else {
                ESP_LOGI(TAG, "update available: %s -> %s", self->version, version);

                esp_http_client_config_t http = {
                    .url = url,
                    .timeout_ms = 30000,
                    .keep_alive_enable = true,
                };
                esp_https_ota_config_t ota = { .http_config = &http };

                esp_err_t err = esp_https_ota(&ota);
                if (err == ESP_OK) {
                    ESP_LOGW(TAG, "update written, restarting into %s", version);
                    vTaskDelay(pdMS_TO_TICKS(500));   /* let the log drain */
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

void discus_ota_start(void)
{
    xTaskCreate(ota_task, "discus_ota", 8192, NULL, 3, NULL);
}
