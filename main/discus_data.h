/* Shared data model: what the agent sends, in a form the UI can render.
 * Fixed-size buffers throughout -- no allocation on the render path. */
#pragma once

#include <stdbool.h>

#define DISCUS_MAX_QUOTES     4
#define DISCUS_MAX_HEADLINES  3
#define DISCUS_HEADLINE_LEN   160

typedef struct {
    bool  ok;
    float cpu, mem, disk;          /* percent */
    float mem_used_gb, mem_total_gb, disk_free_gb;
    float load1;
    int   uptime_s;
    char  host[40];
} discus_system_t;

typedef struct {
    bool  ok;
    char  city[40];
    char  text[40];                /* "Partly cloudy" */
    int   code;                    /* WMO weather code -> icon selection */
    char  unit;                    /* 'C' or 'F' -- chosen by the agent */
    float temp, hi, lo;            /* already converted to `unit` */
    int   humidity;                /* percent */
    float wind_kph;
} discus_weather_t;

typedef struct {
    char  sym[10];
    float last;
    float pct;
    bool  up;
} discus_quote_t;

typedef struct {
    bool           ok;
    int            count;
    discus_quote_t items[DISCUS_MAX_QUOTES];
    char           status[16];     /* OPEN / POST_MKT / ... */
} discus_stocks_t;

#define DISCUS_FEED_COUNT 4      /* world, us, tech, local */

typedef struct {
    bool ok;
    int  count;
    char label[16];          /* WORLD / US / TECH / SUNNYVALE */
    char source[20];         /* BBC / NPR / ... */
    char titles[DISCUS_MAX_HEADLINES][DISCUS_HEADLINE_LEN];
} discus_feed_t;

/* Claude Code token usage for one time bucket. `pct` is negative when no
 * budget is configured on the agent -- nothing on the Mac records the real
 * subscription quota, so "remaining" is opt-in, not discovered. */
typedef struct {
    long long billable;        /* input + output + cache creation */
    long long cache_read;
    int       msgs;            /* assistant turns */
    int       prompts;         /* human-initiated prompts */
    float     pct;             /* < 0 == no token budget configured */
} discus_claude_period_t;

typedef struct {
    bool ok;
    discus_claude_period_t block_5h, today, week, month;
    /* Prompts are the unit the plan limit is actually measured in. The
     * ceiling is a documented RANGE (heavy agentic work exhausts it far
     * sooner), so both ends are carried and the UI shows both. */
    int   prompt_used;
    int   prompt_limit;        /* optimistic ceiling */
    int   prompt_floor;        /* heavy-use lower bound */
    float prompt_pct;
} discus_claude_t;

typedef struct {
    long long epoch;           /* UTC seconds from the agent */
    int       utc_offset_s;    /* current local offset, DST already applied */
    char      tz[8];           /* "PDT" -- display only */
} discus_clock_t;

typedef struct {
    int  brightness;           /* 0-100, decided by the agent */
    char phase[12];            /* day / evening / night */
} discus_display_t;

typedef struct {
    discus_display_t display;
    discus_clock_t   clock;
    discus_system_t  sys;
    discus_weather_t weather;
    discus_stocks_t  stocks;
    discus_feed_t    feeds[DISCUS_FEED_COUNT];
    discus_claude_t  claude;
} discus_data_t;
