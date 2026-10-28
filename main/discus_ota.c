/*
 * Over-the-air updates.
 *
 * The agent serves two endpoints: /firmware.json (a manifest carrying the
 * version string ESP-IDF baked into the image from `git describe`, plus the
 * image's signature) and /firmware.bin. The device compares the manifest
 * version against its own esp_app_get_description()->version, so there is no
 * separate version file anyone has to remember to bump -- rebuild, sign, and
 * the version changes.
 *
 * Signed images: the transport is plain HTTP on the LAN, so nothing about the
 * download is trusted. While streaming the image into the spare slot we hash
 * it, and only if an ECDSA-P256 / SHA-256 signature over those exact bytes
 * verifies against the public key compiled into THIS firmware
 * (main/ota_pubkey.pem; agent/ota_sign.py holds the private half) is the slot
 * made bootable. Any failure leaves the running slot as the boot partition
 * and wipes the rejected image's header so not even a rollback can boot it.
 * A build with no public key refuses every update rather than accepting
 * unsigned ones.
 *
 * Anti-bricking: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE means a newly flashed
 * image boots in ESP_OTA_IMG_PENDING_VERIFY and is only confirmed once it has
 * actually talked to the agent. Push firmware that cannot join WiFi or reach
 * the Mac and the bootloader reverts to the previous slot on next boot --
 * which matters a great deal when the device lives in another room.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#if defined(MBEDTLS_USE_PSA_CRYPTO)
#include "psa/crypto.h"
#endif

#include "discus_ota.h"
#include "discus_creds.h"

static const char *TAG = "discus_ota";

#define MANIFEST_MAX      1024
#define FIRST_CHECK_MS    60000      /* settle before touching flash */
#define CHECK_INTERVAL_MS 300000     /* 5 minutes */
#define SIG_MAX           80         /* DER ECDSA-P256 is at most 72 bytes */
#define OTA_CHUNK         4096

/* main/ota_pubkey.pem, embedded by main/CMakeLists.txt. EMBED_TXTFILES
 * NUL-terminates it; an empty string means no key was present at build. */
extern const char ota_pubkey_pem[] asm("_binary_ota_pubkey_pem_start");

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

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns the decoded length, or -1 on bad hex / too long. */
static int hex_decode(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    if (n == 0 || n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

/* Fills `version`, `url` and `sig`; returns ESP_OK only on a complete,
 * signed manifest. */
static esp_err_t fetch_manifest(char *version, size_t vn, char *url, size_t un,
                                uint8_t *sig, size_t *sig_len)
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
    discus_add_token(c);

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
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "sig");
    if (cJSON_IsString(v) && cJSON_IsString(u) && cJSON_IsString(s)) {
        int n = hex_decode(s->valuestring, sig, SIG_MAX);
        if (n > 0) {
            strncpy(version, v->valuestring, vn - 1); version[vn - 1] = '\0';
            strncpy(url, u->valuestring, un - 1);     url[un - 1]     = '\0';
            *sig_len = (size_t)n;
            out = ESP_OK;
        } else {
            ESP_LOGW(TAG, "manifest signature is malformed; ignoring");
        }
    } else if (cJSON_IsString(v)) {
        ESP_LOGW(TAG, "manifest has no signature; ignoring");
    }
    cJSON_Delete(root);
    return out;
}

static bool have_pubkey(void)
{
    return ota_pubkey_pem[0] != '\0';
}

/* ESP_OK only if `sig` is a valid signature over the image whose SHA-256 is
 * `hash`, made by the private half of the compiled-in public key. */
static esp_err_t verify_signature(const uint8_t hash[32],
                                  const uint8_t *sig, size_t sig_len)
{
    if (!have_pubkey()) return ESP_ERR_INVALID_STATE;

#if defined(MBEDTLS_USE_PSA_CRYPTO)
    psa_crypto_init();          /* idempotent; PK ops need it in this mode */
#endif
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    /* PEM parsing wants the terminating NUL counted in the length. */
    int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)ota_pubkey_pem,
                                         strlen(ota_pubkey_pem) + 1);
    if (rc != 0) {
        ESP_LOGE(TAG, "compiled-in OTA public key does not parse (-0x%04x)", -rc);
    } else if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA)) {
        ESP_LOGE(TAG, "compiled-in OTA public key is not an EC key");
        rc = -1;
    } else {
        rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sig_len);
        if (rc != 0) ESP_LOGE(TAG, "image signature does not verify (-0x%04x)", -rc);
    }
    mbedtls_pk_free(&pk);
    return rc == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* Stream the image into the spare slot, hashing as we go, and verify the
 * signature before anything is made bootable. On any failure the running
 * slot stays the boot partition. */
static esp_err_t download_and_install(const char *url,
                                      const uint8_t *sig, size_t sig_len)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ESP_ERR_NOT_FOUND;

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&http);
    if (!c) return ESP_FAIL;
    discus_add_token(c);

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    int64_t len = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200 || len <= 0 || len > (int64_t)part->size) {
        ESP_LOGE(TAG, "bad image response: HTTP %d, %lld bytes", status, (long long)len);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(part, (size_t)len, &ota);
    bool begun = (err == ESP_OK);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);         /* 0 = SHA-256, not SHA-224 */

    int64_t got = 0;
    while (err == ESP_OK && got < len) {
        int n = esp_http_client_read(c, buf, OTA_CHUNK);
        if (n <= 0) {
            ESP_LOGE(TAG, "download stalled at %lld/%lld bytes",
                     (long long)got, (long long)len);
            err = ESP_FAIL;
            break;
        }
        mbedtls_sha256_update(&sha, (const unsigned char *)buf, (size_t)n);
        err = esp_ota_write(ota, buf, (size_t)n);
        got += n;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK) err = verify_signature(hash, sig, sig_len);

    if (err != ESP_OK) {
        if (begun) {
            esp_ota_abort(ota);
            /* Unverified bytes may now sit in the spare slot. Wipe its
             * header so the bootloader can never select it, even on a
             * rollback. */
            esp_partition_erase_range(part, 0, part->erase_size);
        }
        return err;
    }

    err = esp_ota_end(ota);                 /* checks image structure + hash */
    if (err != ESP_OK) return err;
    return esp_ota_set_boot_partition(part);
}

static bool running_is_pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK
        && state == ESP_OTA_IMG_PENDING_VERIFY;
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

    if (!have_pubkey()) {
        /* Fail closed: without a key we cannot tell a real update from an
         * attacker's, so we take neither. Reflash over USB with a key. */
        ESP_LOGE(TAG, "built without main/ota_pubkey.pem; OTA updates disabled");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(FIRST_CHECK_MS));

    while (true) {
        char version[48] = "", url[192] = "";
        uint8_t sig[SIG_MAX];
        size_t sig_len = 0;

        if (running_is_pending_verify()) {
            /* Overwriting the spare slot now would destroy the image a
             * rollback needs. Wait until this build has proven itself. */
            ESP_LOGD(TAG, "running image not yet confirmed; skipping check");
        } else if (fetch_manifest(version, sizeof version, url, sizeof url,
                                  sig, &sig_len) == ESP_OK) {
            if (strcmp(version, self->version) == 0) {
                ESP_LOGD(TAG, "up to date (%s)", version);
            } else {
                ESP_LOGI(TAG, "update available: %s -> %s", self->version, version);

                esp_err_t err = download_and_install(url, sig, sig_len);
                if (err == ESP_OK) {
                    ESP_LOGW(TAG, "signed update written, restarting into %s", version);
                    vTaskDelay(pdMS_TO_TICKS(500));   /* let the log drain */
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "update rejected, staying on %s: %s",
                             self->version, esp_err_to_name(err));
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
