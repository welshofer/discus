/*
 * Agent client: one plaintext HTTP GET, one JSON parse.
 *
 * The device deliberately polls rather than being pushed to. Polling means no
 * held socket, no reconnect state machine on the MCU, and the whole contract
 * is reproducible from a laptop with `curl`. The agent does the TLS and the
 * API keys; nothing secret lives on this side.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "discus_net.h"

static const char *TAG = "discus_net";

/* The payload is a few KB; cap generously and bail rather than overrun. */
#define RX_MAX  16384

typedef struct {
    char  *buf;
    int    len;
} rx_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    rx_ctx_t *ctx = (rx_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        if (ctx->len + evt->data_len >= RX_MAX) {
            ESP_LOGE(TAG, "response exceeds %d bytes; truncating", RX_MAX);
            return ESP_FAIL;
        }
        memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
        ctx->len += evt->data_len;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

static float jnum(const cJSON *o, const char *key, float dflt)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(i) ? (float)i->valuedouble : dflt;
}

static long long jll(const cJSON *o, const char *key)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(i) ? (long long)i->valuedouble : 0;
}

static void jstr(const cJSON *o, const char *key, char *dst, size_t n)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(i) && i->valuestring) {
        strncpy(dst, i->valuestring, n - 1);
        dst[n - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static bool jok(const cJSON *o)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, "ok");
    return cJSON_IsTrue(i);
}

static void parse_claude_period(const cJSON *root, const char *key,
                                discus_claude_period_t *p)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(o)) {
        memset(p, 0, sizeof(*p));
        p->pct = -1.0f;
        return;
    }
    p->billable   = jll(o, "billable");
    p->cache_read = jll(o, "cache_read");
    p->msgs       = (int)jnum(o, "msgs", 0);
    p->prompts    = (int)jnum(o, "prompts", 0);

    const cJSON *pct = cJSON_GetObjectItemCaseSensitive(o, "pct");
    /* null pct means the agent has no budget configured -- not 0%. */
    p->pct = cJSON_IsNumber(pct) ? (float)pct->valuedouble : -1.0f;
}

static esp_err_t parse(const char *body, discus_data_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *clk = cJSON_GetObjectItemCaseSensitive(root, "clock");
    if (cJSON_IsObject(clk)) {
        out->clock.epoch        = jll(clk, "epoch");
        out->clock.utc_offset_s = (int)jnum(clk, "utc_offset_s", 0);
        jstr(clk, "tz", out->clock.tz, sizeof out->clock.tz);
    }

    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "system");
    if (cJSON_IsObject(sys)) {
        out->sys.ok           = true;
        out->sys.cpu          = jnum(sys, "cpu", 0);
        out->sys.mem          = jnum(sys, "mem", 0);
        out->sys.disk         = jnum(sys, "disk", 0);
        out->sys.mem_used_gb  = jnum(sys, "mem_used_gb", 0);
        out->sys.mem_total_gb = jnum(sys, "mem_total_gb", 0);
        out->sys.disk_free_gb = jnum(sys, "disk_free_gb", 0);
        out->sys.load1        = jnum(sys, "load1", 0);
        out->sys.uptime_s     = (int)jnum(sys, "uptime_s", 0);
        jstr(sys, "host", out->sys.host, sizeof out->sys.host);
    }

    const cJSON *wx = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (cJSON_IsObject(wx) && jok(wx)) {
        out->weather.ok = true;
        jstr(wx, "city", out->weather.city, sizeof out->weather.city);
        jstr(wx, "text", out->weather.text, sizeof out->weather.text);
        out->weather.code = (int)jnum(wx, "code", 3);

        char unit[4];
        jstr(wx, "unit", unit, sizeof unit);
        out->weather.unit = unit[0] ? unit[0] : 'C';

        bool f = (out->weather.unit == 'F');
        out->weather.temp = f ? jnum(wx, "temp_f", 0) : jnum(wx, "temp_c", 0);
        float hi_c = jnum(wx, "hi_c", 0), lo_c = jnum(wx, "lo_c", 0);
        out->weather.hi = f ? hi_c * 9.0f / 5.0f + 32.0f : hi_c;
        out->weather.lo = f ? lo_c * 9.0f / 5.0f + 32.0f : lo_c;
        out->weather.humidity = (int)jnum(wx, "humidity", 0);
        out->weather.wind_kph = jnum(wx, "wind_kph", 0);
    }

    const cJSON *stk = cJSON_GetObjectItemCaseSensitive(root, "stocks");
    if (cJSON_IsObject(stk) && jok(stk)) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(stk, "items");
        const cJSON *it = NULL;
        int n = 0;
        cJSON_ArrayForEach(it, items) {
            if (n >= DISCUS_MAX_QUOTES) break;
            discus_quote_t *q = &out->stocks.items[n];
            jstr(it, "sym", q->sym, sizeof q->sym);
            q->last = jnum(it, "last", 0);
            q->pct  = jnum(it, "pct", 0);
            q->up   = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "up"));
            if (n == 0) jstr(it, "status", out->stocks.status, sizeof out->stocks.status);
            n++;
        }
        out->stocks.count = n;
        out->stocks.ok = (n > 0);
    }

    /* news is an object keyed by feed, not an array -- order is fixed here so
     * the panels never shuffle under the user when a feed fails. */
    static const char *feed_keys[DISCUS_FEED_COUNT] = { "world", "us", "tech", "local" };
    const cJSON *news = cJSON_GetObjectItemCaseSensitive(root, "news");
    if (cJSON_IsObject(news)) {
        for (int f = 0; f < DISCUS_FEED_COUNT; f++) {
            const cJSON *fd = cJSON_GetObjectItemCaseSensitive(news, feed_keys[f]);
            if (!cJSON_IsObject(fd)) continue;
            discus_feed_t *dst = &out->feeds[f];
            jstr(fd, "label",  dst->label,  sizeof dst->label);
            jstr(fd, "source", dst->source, sizeof dst->source);

            const cJSON *items = cJSON_GetObjectItemCaseSensitive(fd, "items");
            const cJSON *it = NULL;
            int n = 0;
            cJSON_ArrayForEach(it, items) {
                if (n >= DISCUS_MAX_HEADLINES) break;
                if (cJSON_IsString(it) && it->valuestring) {
                    strncpy(dst->titles[n], it->valuestring, DISCUS_HEADLINE_LEN - 1);
                    dst->titles[n][DISCUS_HEADLINE_LEN - 1] = '\0';
                    n++;
                }
            }
            dst->count = n;
            dst->ok = jok(fd) && n > 0;
        }
    }

    const cJSON *cl = cJSON_GetObjectItemCaseSensitive(root, "claude");
    if (cJSON_IsObject(cl) && jok(cl)) {
        out->claude.ok = true;
        parse_claude_period(cl, "block_5h", &out->claude.block_5h);
        parse_claude_period(cl, "today",    &out->claude.today);
        parse_claude_period(cl, "week",     &out->claude.week);
        parse_claude_period(cl, "month",    &out->claude.month);
        out->claude.prompt_used  = (int)jnum(cl, "prompt_used", 0);
        out->claude.prompt_limit = (int)jnum(cl, "prompt_limit", 900);
        out->claude.prompt_floor = (int)jnum(cl, "prompt_floor", 200);
        out->claude.prompt_pct   = jnum(cl, "prompt_pct", 0);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t discus_net_fetch(const char *url, discus_data_t *out)
{
    char *buf = malloc(RX_MAX);       /* lands in PSRAM; too big for stack */
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = '\0';

    rx_ctx_t ctx = { .buf = buf, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .user_data = &ctx,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        free(buf);
        return ESP_FAIL;
    }

    err = parse(buf, out);
    free(buf);
    return err;
}
