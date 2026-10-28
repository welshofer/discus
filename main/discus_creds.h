#pragma once

/* Secrets come from main/wifi_creds.h when present (gitignored, so they never
 * reach version control), otherwise from menuconfig.
 *
 * Every file that needs one includes this header rather than wifi_creds.h
 * directly, so `touch main/discus_creds.h` after creating wifi_creds.h is
 * enough to rebuild all of them (see README gotcha 8). */

#include <string.h>

#include "sdkconfig.h"
#include "esp_http_client.h"

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
#ifndef DISCUS_AGENT_TOKEN_OVERRIDE
#  define DISCUS_AGENT_TOKEN_OVERRIDE CONFIG_DISCUS_AGENT_TOKEN
#endif

#define DISCUS_TOKEN_HEADER "X-Discus-Token"

/* Attach the shared agent token to a request. An agent bound to the LAN
 * answers 401 without it; a loopback-only agent ignores it. */
static inline void discus_add_token(esp_http_client_handle_t c)
{
    if (strlen(DISCUS_AGENT_TOKEN_OVERRIDE) > 0) {
        esp_http_client_set_header(c, DISCUS_TOKEN_HEADER, DISCUS_AGENT_TOKEN_OVERRIDE);
    }
}
