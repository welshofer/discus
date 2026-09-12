/*
 * discus UI -- a wrapping carousel of round dashboard panels.
 *
 * Two hardware facts drive every layout decision here:
 *
 *  1. The framebuffer is 720x720 but the glass is a circle, so ~21% of every
 *     frame is hidden behind the bezel. Usable half-width shrinks fast off
 *     centre: +-327px at y=+-150, but only +-226px at y=+-280.
 *  2. This is read from across a room, so type is sized for glanceability.
 *     LVGL's largest built-in Montserrat is 48px, which is the ceiling for
 *     hero numbers without shipping a custom font.
 *
 * Carousel: NOT lv_tileview. Tileview is scrollable, and a scrollable object
 * swallows the drag -- LVGL then delivers neither LV_EVENT_GESTURE (only sent
 * when nothing scrolled) nor a usable RELEASED, so wrap-around cannot be
 * built on it at all. Instead the panels live on one wide strip that we
 * animate ourselves, and gestures arrive cleanly because nothing scrolls.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "discus_ui.h"

static const char *TAG = "discus_ui";

/* Purpose-built font: digits and a colon only (0x30-0x3A), 128px, 4bpp,
 * generated with lv_font_conv from the same Montserrat-Medium.ttf that
 * LVGL's own built-ins come from. LVGL ships nothing larger than 48px, and
 * this panel is meant to be read from across a room. Eleven glyphs keeps it
 * to ~190KB in a 6MB partition. */
LV_FONT_DECLARE(lv_font_clock_128);

#define BG              0x07070A
#define TRACK           0x1C1C22
#define FG              0xF5F5FA
#define DIM             0x9A9AAB
#define FAINT           0x5A5A6A

#define CYAN            0x4FC3F7
#define VIOLET          0xB388FF
#define AMBER           0xFFB74D
#define GREEN           0x66BB6A
#define RED             0xEF5350
#define SUN_Y           0xFFD54F
#define CLOUD_G         0xBFC7D5

#define W               BSP_LCD_H_RES
#define H               BSP_LCD_V_RES

#define AUTO_ROTATE_MS  90000
#define SLIDE_MS        280

#define RING_OUTER_PX   700
#define RING_STEP_PX    76
#define RING_WIDTH_PX   30
#define RING_SCALE      1000

/* esp_lv_adapter_lock(): -1 waits forever, 0 fails immediately. */
#define LVGL_FOREVER    ((uint32_t)-1)

enum {
    PANEL_CLOCK, PANEL_SYS, PANEL_WEATHER, PANEL_STOCKS,
    PANEL_NEWS0, PANEL_NEWS1, PANEL_NEWS2, PANEL_NEWS3,
    PANEL_CLAUDE, PANEL_COUNT
};
#define NEWS_FIRST PANEL_NEWS0

static lv_obj_t   *s_strip;
static lv_obj_t   *s_panels[PANEL_COUNT];
static lv_obj_t   *s_dots[PANEL_COUNT];
static lv_timer_t *s_rotate_timer;
static int         s_index;

/* system */
static lv_obj_t *s_cpu_arc, *s_mem_arc, *s_disk_arc;
static lv_obj_t *s_cpu_lbl, *s_mem_lbl, *s_disk_lbl;
static lv_obj_t *s_sys_value, *s_sys_host;

/* weather */
static lv_obj_t *s_wx_city, *s_wx_temp, *s_wx_text, *s_wx_hilo, *s_wx_detail;
static lv_obj_t *s_wx_icon;                 /* container */
static lv_obj_t *s_ic_sun, *s_ic_ray[8];
static lv_obj_t *s_ic_cloud[3], *s_ic_cloud_base;
static lv_obj_t *s_ic_drop[3], *s_ic_flake[3], *s_ic_bolt, *s_ic_fog[3];

/* stocks */
static lv_obj_t *s_stk_sym[DISCUS_MAX_QUOTES];
static lv_obj_t *s_stk_last[DISCUS_MAX_QUOTES];
static lv_obj_t *s_stk_pct[DISCUS_MAX_QUOTES];
static lv_obj_t *s_stk_status;

/* news: one panel per feed */
static lv_obj_t *s_news_hdr[DISCUS_FEED_COUNT];
static lv_obj_t *s_news_lbl[DISCUS_FEED_COUNT][DISCUS_MAX_HEADLINES];

/* clock */
static lv_obj_t *s_clk_sec_arc, *s_clk_time, *s_clk_ampm, *s_clk_date, *s_clk_tz;
static int  s_utc_offset;
static char s_tz[8] = "";

/* claude */
static lv_obj_t *s_cl_arc, *s_cl_big, *s_cl_band, *s_cl_row[3];

static lv_obj_t *s_status;
static lv_obj_t *s_dim;        /* stale-data scrim */

/* Polls run every 3s, so two consecutive misses is a real outage rather
 * than a dropped packet. */
#define STALE_AFTER_S 12

static bool ui_lock(void)
{
    if (bsp_display_lock(LVGL_FOREVER) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return false;
    }
    return true;
}

/* LVGL's lv_snprintf has no %f, and the built-in Montserrat range is only
 * 0x20-0x7F plus 0xB0 (degree) and 0x2022 (bullet). Anything else -- U+00B7
 * middle dot, en dash, arrows -- renders as an empty box. Integers and that
 * exact character set only. */
#define SEP "\xE2\x80\xA2"          /* U+2022 BULLET, in range */

static void fmt_tokens(char *buf, size_t n, long long v)
{
    if (v < 1000)                   snprintf(buf, n, "%lld", v);
    else if (v < 1000000)           snprintf(buf, n, "%lldK", v / 1000);
    else if (v < 1000000000LL)      snprintf(buf, n, "%lld.%lldM", v / 1000000,
                                             (v % 1000000) / 100000);
    else                            snprintf(buf, n, "%lld.%lldB", v / 1000000000LL,
                                             (v % 1000000000LL) / 100000000LL);
}

static void fmt_age(char *b, size_t n, int secs)
{
    if (secs < 60)         snprintf(b, n, "%ds", secs);
    else if (secs < 3600)  snprintf(b, n, "%dm", secs / 60);
    else if (secs < 86400) snprintf(b, n, "%dh", secs / 3600);
    else                   snprintf(b, n, "%dd", secs / 86400);
}

static lv_obj_t *make_label(lv_obj_t *p, const lv_font_t *font, uint32_t color,
                            int x, int y, const char *text)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, x, y);
    return l;
}

static lv_obj_t *make_ring(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, RING_SCALE);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, RING_WIDTH_PX, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, RING_WIDTH_PX, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    return arc;
}

/* ── weather icon, drawn from primitives (no image assets) ────────────── */

static lv_obj_t *blob(lv_obj_t *p, int d, uint32_t color, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_align(o, LV_ALIGN_CENTER, x, y);
    return o;
}

static lv_obj_t *bar(lv_obj_t *p, int w, int h, uint32_t color, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_align(o, LV_ALIGN_CENTER, x, y);
    return o;
}

/* One lv_line per ray. A single 16-point line does NOT draw eight separate
 * spokes -- lv_line connects every point in sequence, so it renders as one
 * continuous zigzag star outline. The points must be static: lv_line stores
 * the pointer, it does not copy the array. */
static lv_point_precise_t s_ray_pts[8][2];

/* The icon box must be big enough that the sun can slide up-left for the
 * partly-cloudy composition without its rays being clipped by the parent. */
#define ICON_W   320
#define ICON_H   260
#define ICON_CX  (ICON_W / 2)
#define ICON_CY  (ICON_H / 2)
#define SUN_DX   (-62)           /* offset applied in partly-cloudy mode */
#define SUN_DY   (-44)

static void build_weather_icon(lv_obj_t *parent, int y_off)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, ICON_W, ICON_H);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, y_off);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    s_wx_icon = c;

    /* 8 discrete spokes around the icon-box centre. */
    static const int dx8[8] = {  0,  1, 1, 1, 0, -1, -1, -1 };
    static const int dy8[8] = { -1, -1, 0, 1, 1,  1,  0, -1 };
    for (int i = 0; i < 8; i++) {
        /* Diagonal unit vectors are sqrt(2) long, so their scalars are
         * divided by ~1.414 to land every tip on one circle. */
        bool diag = (dx8[i] && dy8[i]);
        int  r_in  = diag ? 46 : 64;
        int  r_out = diag ? 60 : 84;

        s_ray_pts[i][0].x = ICON_CX + dx8[i] * r_in;
        s_ray_pts[i][0].y = ICON_CY + dy8[i] * r_in;
        s_ray_pts[i][1].x = ICON_CX + dx8[i] * r_out;
        s_ray_pts[i][1].y = ICON_CY + dy8[i] * r_out;

        lv_obj_t *ray = lv_line_create(c);
        lv_line_set_points(ray, s_ray_pts[i], 2);
        lv_obj_set_style_line_width(ray, 8, 0);
        lv_obj_set_style_line_color(ray, lv_color_hex(SUN_Y), 0);
        lv_obj_set_style_line_rounded(ray, true, 0);
        lv_obj_set_pos(ray, 0, 0);
        s_ic_ray[i] = ray;
    }

    s_ic_sun = blob(c, 86, SUN_Y, 0, 0);

    /* cloud: three lobes on a rounded base */
    s_ic_cloud[0] = blob(c, 74, CLOUD_G, -38, 6);
    s_ic_cloud[1] = blob(c, 98, CLOUD_G,   2, -12);
    s_ic_cloud[2] = blob(c, 68, CLOUD_G,  44, 8);
    s_ic_cloud_base = bar(c, 170, 52, CLOUD_G, 2, 22);

    for (int i = 0; i < 3; i++) {
        s_ic_drop[i]  = bar(c, 9, 30, CYAN, -46 + i * 46, 76);
        s_ic_flake[i] = blob(c, 14, FG,    -46 + i * 46, 78);
    }
    s_ic_bolt = bar(c, 14, 62, AMBER, 4, 80);

    for (int i = 0; i < 3; i++) {
        s_ic_fog[i] = bar(c, 150 - i * 18, 14, CLOUD_G, 0, -20 + i * 34);
    }
}

static void show(lv_obj_t *o, bool v)
{
    if (o) lv_obj_set_style_opa(o, v ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/* WMO weather code -> which primitives to light up. */
static void set_weather_icon(int code)
{
    bool sun = false, cloud = false, rain = false, snow = false;
    bool bolt = false, fog = false, offset_sun = false;

    if (code <= 1)                      { sun = true; }
    else if (code == 2)                 { sun = true; cloud = true; offset_sun = true; }
    else if (code == 3)                 { cloud = true; }
    else if (code == 45 || code == 48)  { fog = true; }
    else if (code >= 95)                { cloud = true; bolt = true; }
    else if ((code >= 71 && code <= 77) || code == 85 || code == 86)
                                        { cloud = true; snow = true; }
    else                                { cloud = true; rain = true; }

    show(s_ic_sun, sun);
    for (int i = 0; i < 8; i++) show(s_ic_ray[i], sun);
    if (sun) {
        /* tuck the sun behind the cloud when both are showing */
        lv_obj_align(s_ic_sun, LV_ALIGN_CENTER,
                     offset_sun ? SUN_DX : 0, offset_sun ? SUN_DY : 0);
        /* Rays are drawn from absolute points, so they must be translated by
         * the same delta or the disc slides out from under its own spokes. */
        for (int i = 0; i < 8; i++) {
            lv_obj_set_pos(s_ic_ray[i], offset_sun ? SUN_DX : 0,
                                        offset_sun ? SUN_DY : 0);
            lv_obj_set_style_opa(s_ic_ray[i],
                                 offset_sun ? LV_OPA_40 : LV_OPA_COVER, 0);
        }
    }
    for (int i = 0; i < 3; i++) show(s_ic_cloud[i], cloud);
    show(s_ic_cloud_base, cloud);
    for (int i = 0; i < 3; i++) {
        show(s_ic_drop[i], rain);
        show(s_ic_flake[i], snow);
    }
    show(s_ic_bolt, bolt);
    for (int i = 0; i < 3; i++) show(s_ic_fog[i], fog);
}

/* ── panels ───────────────────────────────────────────────────────────── */

static void build_system(lv_obj_t *t)
{
    s_cpu_arc  = make_ring(t, RING_OUTER_PX,                  CYAN);
    s_mem_arc  = make_ring(t, RING_OUTER_PX - RING_STEP_PX,   VIOLET);
    s_disk_arc = make_ring(t, RING_OUTER_PX - 2*RING_STEP_PX, AMBER);

    s_sys_host  = make_label(t, &lv_font_montserrat_28, FAINT, 0, -170, "");
    s_sys_value = make_label(t, &lv_font_montserrat_48, FG,    0, -70,  "--");
    make_label(t, &lv_font_montserrat_28, FAINT, 0, -16, "CPU");

    s_cpu_lbl  = make_label(t, &lv_font_montserrat_36, CYAN,   0, 60,  "cpu  --");
    s_mem_lbl  = make_label(t, &lv_font_montserrat_36, VIOLET, 0, 120, "mem  --");
    s_disk_lbl = make_label(t, &lv_font_montserrat_36, AMBER,  0, 180, "disk --");
}

static void build_weather(lv_obj_t *t)
{
    s_wx_city = make_label(t, &lv_font_montserrat_36, DIM, 0, -280, "--");
    build_weather_icon(t, -150);
    s_wx_temp   = make_label(t, &lv_font_montserrat_48, FG,   0, -10, "--");
    s_wx_text   = make_label(t, &lv_font_montserrat_40, CYAN, 0, 70,  "--");
    s_wx_hilo   = make_label(t, &lv_font_montserrat_36, DIM,  0, 150, "");
    s_wx_detail = make_label(t, &lv_font_montserrat_28, FAINT, 0, 215, "");
}

static void build_stocks(lv_obj_t *t)
{
    make_label(t, &lv_font_montserrat_28, FAINT, 0, -270, "MARKETS");
    for (int i = 0; i < DISCUS_MAX_QUOTES; i++) {
        int y = -140 + i * 100;
        s_stk_sym[i]  = make_label(t, &lv_font_montserrat_44, FG,  -175, y, "");
        s_stk_last[i] = make_label(t, &lv_font_montserrat_44, DIM,   35, y, "");
        s_stk_pct[i]  = make_label(t, &lv_font_montserrat_32, DIM,  200, y, "");
    }
    s_stk_status = make_label(t, &lv_font_montserrat_28, FAINT, 0, 265, "");
}

static void build_news(lv_obj_t *t, int feed)
{
    s_news_hdr[feed] = make_label(t, &lv_font_montserrat_32, CYAN, 0, -290, "NEWS");

    /* The lead story gets real weight; the follow-ups stay readable but
     * clearly secondary. Widths taper with the circle. */
    static const int   y[DISCUS_MAX_HEADLINES]  = { -150, 40, 200 };
    static const int   wd[DISCUS_MAX_HEADLINES] = { 560, 520, 470 };
    const lv_font_t   *fonts[DISCUS_MAX_HEADLINES] = {
        &lv_font_montserrat_36, &lv_font_montserrat_28, &lv_font_montserrat_28
    };
    const uint32_t cols[DISCUS_MAX_HEADLINES] = { FG, DIM, DIM };

    for (int i = 0; i < DISCUS_MAX_HEADLINES; i++) {
        lv_obj_t *l = lv_label_create(t);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, wd[i]);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(cols[i]), 0);
        lv_obj_set_style_text_font(l, fonts[i], 0);
        lv_obj_set_style_text_line_space(l, 6, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, y[i]);
        lv_label_set_text(l, "");
        s_news_lbl[feed][i] = l;
    }
}

static const char *DAYS[7]  = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday" };
static const char *MONTHS[12] = { "January", "February", "March", "April",
                                  "May", "June", "July", "August",
                                  "September", "October", "November", "December" };

/* Runs inside an lv_timer, i.e. on the LVGL task with the lock already held.
 * Taking bsp_display_lock() here would be locking against ourselves. */
static void clock_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    time_t now = time(NULL);

    /* Before the first sync the epoch is ~1970; do not show a fake time. */
    if (now < 1600000000) {
        lv_label_set_text(s_clk_time, "--:--");
        lv_label_set_text(s_clk_date, "waiting for time");
        lv_label_set_text(s_clk_ampm, "");
        lv_arc_set_value(s_clk_sec_arc, 0);
        return;
    }

    /* gmtime_r on (UTC + offset) gives local fields without needing tzdata,
     * which newlib does not ship. */
    time_t local = now + s_utc_offset;
    struct tm tm;
    gmtime_r(&local, &tm);

    int h12 = tm.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    lv_label_set_text_fmt(s_clk_time, "%d:%02d", h12, tm.tm_min);
    lv_label_set_text(s_clk_ampm, tm.tm_hour < 12 ? "AM" : "PM");
    lv_label_set_text_fmt(s_clk_date, "%s, %s %d",
                          DAYS[tm.tm_wday % 7], MONTHS[tm.tm_mon % 12], tm.tm_mday);
    lv_label_set_text(s_clk_tz, s_tz);
    lv_arc_set_value(s_clk_sec_arc, tm.tm_sec);
}

static void build_clock(lv_obj_t *t)
{
    /* A ring that fills once a minute -- the one panel where the round
     * display is doing exactly what a round display is for. */
    s_clk_sec_arc = make_ring(t, RING_OUTER_PX, CYAN);
    lv_arc_set_range(s_clk_sec_arc, 0, 59);

    /* Time and AM/PM share one flex row. Cross-axis END baseline-aligns the
     * small suffix to the bottom of the big digits instead of centring it
     * against their full height, which would float it awkwardly mid-glyph. */
    lv_obj_t *row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 660, 175);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -55);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    s_clk_time = lv_label_create(row);
    lv_label_set_text(s_clk_time, "--:--");
    lv_obj_set_style_text_color(s_clk_time, lv_color_hex(FG), 0);
    lv_obj_set_style_text_font(s_clk_time, &lv_font_clock_128, 0);

    s_clk_ampm = lv_label_create(row);
    lv_label_set_text(s_clk_ampm, "");
    lv_obj_set_style_text_color(s_clk_ampm, lv_color_hex(DIM), 0);
    lv_obj_set_style_text_font(s_clk_ampm, &lv_font_montserrat_40, 0);
    lv_obj_set_style_margin_left(s_clk_ampm, 16, 0);
    lv_obj_set_style_margin_bottom(s_clk_ampm, 22, 0);

    s_clk_date = make_label(t, &lv_font_montserrat_36, DIM,   0, 110, "");
    s_clk_tz   = make_label(t, &lv_font_montserrat_28, FAINT, 0, 175, "");

    lv_timer_create(clock_tick, 1000, NULL);
}

static void build_claude(lv_obj_t *t)
{
    s_cl_arc = make_ring(t, RING_OUTER_PX, VIOLET);
    make_label(t, &lv_font_montserrat_28, FAINT, 0, -230, "CLAUDE  5H WINDOW");
    s_cl_big  = make_label(t, &lv_font_montserrat_48, FG,     0, -120, "--");
    make_label(t, &lv_font_montserrat_32, VIOLET, 0, -50, "prompts");
    s_cl_band = make_label(t, &lv_font_montserrat_28, FAINT, 0, -5, "");

    static const char *names[3] = { "today", "week", "month" };
    for (int i = 0; i < 3; i++) {
        int y = 75 + i * 62;
        make_label(t, &lv_font_montserrat_28, FAINT, -130, y, names[i]);
        s_cl_row[i] = make_label(t, &lv_font_montserrat_32, DIM, 70, y, "--");
    }
}

/* ── carousel ─────────────────────────────────────────────────────────── */

static void update_dots(void)
{
    for (int i = 0; i < PANEL_COUNT; i++) {
        bool on = (i == s_index);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(on ? FG : FAINT), 0);
        lv_obj_set_size(s_dots[i], on ? 12 : 7, on ? 12 : 7);
    }
}

static void strip_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

/* `animate` is false for wraps: sliding from the last panel to the first
 * would sweep through every panel in between and read as a fast rewind. */
static void goto_panel(int idx, bool animate)
{
    s_index = ((idx % PANEL_COUNT) + PANEL_COUNT) % PANEL_COUNT;
    int32_t target = -s_index * W;

    lv_anim_delete(s_strip, strip_x_cb);
    if (animate) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_strip);
        lv_anim_set_exec_cb(&a, strip_x_cb);
        lv_anim_set_values(&a, lv_obj_get_x(s_strip), target);
        lv_anim_set_duration(&a, SLIDE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        lv_obj_set_x(s_strip, target);
    }
    update_dots();
}

static void advance_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    int next = s_index + 1;
    ESP_LOGI(TAG, "auto-rotate: panel %d -> %d", s_index, next % PANEL_COUNT);
    goto_panel(next, next < PANEL_COUNT);     /* snap when rolling over */
}

static void gesture_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        int next = s_index + 1;
        goto_panel(next, next < PANEL_COUNT);
    } else if (dir == LV_DIR_RIGHT) {
        int prev = s_index - 1;
        goto_panel(prev, prev >= 0);
    } else {
        return;
    }
    if (s_rotate_timer) lv_timer_reset(s_rotate_timer);
}

void discus_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_strip = lv_obj_create(scr);
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, W * PANEL_COUNT, H);
    lv_obj_set_pos(s_strip, 0, 0);
    lv_obj_remove_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Nothing on the strip may be clickable, or it becomes the gesture
     * target instead of the screen and the swipe is lost. */
    lv_obj_remove_flag(s_strip, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < PANEL_COUNT; i++) {
        lv_obj_t *p = lv_obj_create(s_strip);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, W, H);
        lv_obj_set_pos(p, i * W, 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
        s_panels[i] = p;
    }

    build_clock(s_panels[PANEL_CLOCK]);
    build_system(s_panels[PANEL_SYS]);
    build_weather(s_panels[PANEL_WEATHER]);
    build_stocks(s_panels[PANEL_STOCKS]);
    for (int f = 0; f < DISCUS_FEED_COUNT; f++) {
        build_news(s_panels[NEWS_FIRST + f], f);
    }
    build_claude(s_panels[PANEL_CLAUDE]);

    /* A single translucent rectangle, not an opacity style on the strip:
     * lv_obj_set_style_opa() on a parent forces LVGL to composite the whole
     * subtree through a layer buffer, and a 5760x720 strip would need
     * megabytes. One scrim rectangle costs nothing. */
    s_dim = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dim);
    lv_obj_set_size(s_dim, W, H);
    lv_obj_center(s_dim);
    lv_obj_set_style_bg_color(s_dim, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s_dim, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < PANEL_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(FAINT), 0);
        lv_obj_align(d, LV_ALIGN_CENTER,
                     (int)((i - (PANEL_COUNT - 1) / 2.0f) * 26), 315);
        s_dots[i] = d;
    }

    s_status = make_label(scr, &lv_font_montserrat_28, AMBER, 0, 275, "");

    /* Gestures land on the screen because nothing above it is clickable. */
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    s_rotate_timer = lv_timer_create(advance_cb, AUTO_ROTATE_MS, NULL);
    set_weather_icon(3);
    goto_panel(0, false);
}

/* ── updates ──────────────────────────────────────────────────────────── */

static void set_ring(lv_obj_t *arc, lv_obj_t *lbl, const char *name, float pct)
{
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    lv_arc_set_value(arc, (int32_t)(pct * (RING_SCALE / 100.0f)));
    lv_label_set_text_fmt(lbl, "%s  %d%%", name, (int)(pct + 0.5f));
}

void discus_ui_update(const discus_data_t *d)
{
    if (!ui_lock()) return;

    if (d->sys.ok) {
        set_ring(s_cpu_arc,  s_cpu_lbl,  "cpu",  d->sys.cpu);
        set_ring(s_mem_arc,  s_mem_lbl,  "mem",  d->sys.mem);
        set_ring(s_disk_arc, s_disk_lbl, "disk", d->sys.disk);
        lv_label_set_text_fmt(s_sys_value, "%d%%", (int)(d->sys.cpu + 0.5f));
        lv_label_set_text(s_sys_host, d->sys.host);
    }

    if (d->weather.ok) {
        lv_label_set_text(s_wx_city, d->weather.city);
        lv_label_set_text_fmt(s_wx_temp, "%d°%c", (int)(d->weather.temp + 0.5f),
                              d->weather.unit);
        lv_label_set_text(s_wx_text, d->weather.text);
        lv_label_set_text_fmt(s_wx_hilo, "H %d°   L %d°",
                              (int)(d->weather.hi + 0.5f), (int)(d->weather.lo + 0.5f));
        lv_label_set_text_fmt(s_wx_detail, "%d%% humidity   %d km/h",
                              d->weather.humidity, (int)(d->weather.wind_kph + 0.5f));
        set_weather_icon(d->weather.code);
    }

    for (int i = 0; i < DISCUS_MAX_QUOTES; i++) {
        if (i < d->stocks.count) {
            const discus_quote_t *q = &d->stocks.items[i];
            lv_label_set_text(s_stk_sym[i], q->sym);
            lv_label_set_text_fmt(s_stk_last[i], "%d.%02d", (int)q->last,
                                  (int)((q->last - (int)q->last) * 100 + 0.5f));
            float ap = q->pct < 0 ? -q->pct : q->pct;
            lv_label_set_text_fmt(s_stk_pct[i], "%s%d.%02d%%", q->pct < 0 ? "-" : "+",
                                  (int)ap, (int)((ap - (int)ap) * 100 + 0.5f));
            lv_obj_set_style_text_color(s_stk_pct[i],
                                        lv_color_hex(q->up ? GREEN : RED), 0);
        } else {
            lv_label_set_text(s_stk_sym[i], "");
            lv_label_set_text(s_stk_last[i], "");
            lv_label_set_text(s_stk_pct[i], "");
        }
    }
    lv_label_set_text(s_stk_status, d->stocks.status);

    for (int f = 0; f < DISCUS_FEED_COUNT; f++) {
        const discus_feed_t *fd = &d->feeds[f];
        lv_label_set_text(s_news_hdr[f], fd->label[0] ? fd->label : "NEWS");
        for (int i = 0; i < DISCUS_MAX_HEADLINES; i++) {
            lv_label_set_text(s_news_lbl[f][i], i < fd->count ? fd->titles[i] : "");
        }
    }

    if (d->claude.ok) {
        char buf[24];
        float pct = d->claude.prompt_pct;
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        lv_arc_set_value(s_cl_arc, (int32_t)(pct * (RING_SCALE / 100.0f)));

        bool past_floor = d->claude.prompt_used >= d->claude.prompt_floor;
        lv_obj_set_style_arc_color(s_cl_arc,
                                   lv_color_hex(past_floor ? AMBER : VIOLET),
                                   LV_PART_INDICATOR);

        lv_label_set_text_fmt(s_cl_big, "%d", d->claude.prompt_used);
        lv_label_set_text_fmt(s_cl_band, "of ~%d-%d",
                              d->claude.prompt_floor, d->claude.prompt_limit);

        const discus_claude_period_t *p[3] = {
            &d->claude.today, &d->claude.week, &d->claude.month
        };
        for (int i = 0; i < 3; i++) {
            fmt_tokens(buf, sizeof buf, p[i]->billable);
            lv_label_set_text_fmt(s_cl_row[i], "%d  " SEP "  %s", p[i]->prompts, buf);
        }
    }

    bsp_display_unlock();
}

/* age_s < 0 means we have never had a successful fetch. */
void discus_ui_set_stale(int age_s)
{
    if (!ui_lock()) return;

    bool stale = (age_s < 0) || (age_s >= STALE_AFTER_S);
    if (s_dim) {
        if (stale) lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_dim, stale ? LV_OPA_60 : LV_OPA_TRANSP, 0);
    }
    if (s_status) {
        if (!stale) {
            lv_label_set_text(s_status, "");
        } else if (age_s < 0) {
            lv_obj_set_style_text_color(s_status, lv_color_hex(AMBER), 0);
            lv_label_set_text(s_status, "waiting for agent");
        } else {
            char age[16];
            fmt_age(age, sizeof age, age_s);
            lv_obj_set_style_text_color(s_status, lv_color_hex(AMBER), 0);
            lv_label_set_text_fmt(s_status, "stale " SEP " %s ago", age);
        }
    }
    bsp_display_unlock();
}

void discus_ui_set_utc_offset(int offset_s, const char *tz)
{
    s_utc_offset = offset_s;
    if (tz) {
        strncpy(s_tz, tz, sizeof s_tz - 1);
        s_tz[sizeof s_tz - 1] = '\0';
    }
}

void discus_ui_set_status(const char *text)
{
    if (!ui_lock()) return;
    if (s_status) lv_label_set_text(s_status, text);
    bsp_display_unlock();
}
