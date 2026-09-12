#pragma once
#include "discus_data.h"

/* Build the carousel. Must be called with the LVGL lock held. */
void discus_ui_create(void);

/* Push new data into the panels. Takes the LVGL lock itself. */
void discus_ui_update(const discus_data_t *d);

/* Connectivity / error line shown on every panel. */
void discus_ui_set_status(const char *text);

/* Seconds since the last successful fetch, or < 0 if there has never been
 * one. Dims the panels and shows the data age once past the stale threshold. */
void discus_ui_set_stale(int age_s);

/* Current local UTC offset (DST already applied) and its short name. */
void discus_ui_set_utc_offset(int offset_s, const char *tz);
