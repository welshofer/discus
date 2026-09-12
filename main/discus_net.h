#pragma once
#include "esp_err.h"
#include "discus_data.h"

/* Fetch the agent's /metrics document and parse it into `out`.
 * Returns ESP_OK only if the whole round trip and parse succeeded. */
esp_err_t discus_net_fetch(const char *url, discus_data_t *out);
