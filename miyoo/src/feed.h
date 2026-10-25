#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_AGENTS 32
#define N_FAMILIES 4

typedef enum { FAM_CLAUDE, FAM_GPT, FAM_GEMINI, FAM_OTHER } Family;

/* Ordered by urgency; matches the bridge's sort order. */
typedef enum { B_NEEDS_INPUT, B_FAILED, B_RUNNING, B_ATTENTION, B_DONE } Bucket;

typedef struct {
    char id[8];
    Bucket bucket;
    char status[16];
    char attention[16];
    int64_t out_tokens;
    int cost_cents;
    int ctx_pct;
    int64_t updated;
    Family family;
    char model[64];
    char cwd[80];
    char title[208];
} Agent;

#define MAX_USAGE 16

/* One subscription-limit window from `omp usage` on a bridge with PAGER_USAGE=1:
 * `usage\t<provider>\t<window id>\t<label>\t<used per-mille 0..1000>\t<resets unix s|0>\t<status>`
 * Status is omp's ("ok", "warning", "exhausted", ...). */
typedef struct {
    char provider[24]; /* omp provider id: anthropic, openai-codex, google-antigravity, zai, ... */
    char window[16];   /* omp window id: 5h, 7d, weekly, 1w, ... */
    char label[40];    /* omp label, e.g. "Claude 7 Day (Fable)" */
    int used_pm;       /* used, per mille */
    int64_t resets_at; /* unix seconds, 0 if unknown */
    char status[16];
} UsageRow;
typedef struct {
    char ledger_id[16];
    char host[40];
    bool daemon_up;
    int64_t rev;
    int64_t now;       /* bridge clock, unix seconds */
    int32_t tz_offset; /* bridge local time offset from UTC, seconds */
    int64_t fed[N_FAMILIES]; /* cumulative output tokens by model family */
    int64_t turns;
    int64_t errors;
    /* `today` line (zero from older bridges): the bridge-local day number, i.e.
     * floor((now + tz_offset) / 86400), and that day's output tokens and cost across all agents. */
    int64_t today_day;
    int64_t today_tokens;
    int64_t today_cost_mc; /* millicents (1/1000 of a cent) */
    /* `usage_at\t<unix s>` + `usage` lines: only from the bridge that polls omp (0 / none otherwise). */
    int64_t usage_at;
    int n_usage;
    UsageRow usage[MAX_USAGE];
    int n_agents;
    Agent agents[MAX_AGENTS];
} Feed;

/* Parses a bridge /pager response body. Returns false if it is not a PAGER 1 document. */
bool feed_parse(const char *body, Feed *out);
const char *family_name(Family f);
const char *bucket_label(Bucket b);
/* 950 -> "950", 12345 -> "12.3k", 2500000 -> "2.50M" */
void format_tokens(int64_t n, char *buf, size_t len);
