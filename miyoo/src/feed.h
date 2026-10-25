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
    int n_agents;
    Agent agents[MAX_AGENTS];
} Feed;

/* Parses a bridge /pager response body. Returns false if it is not a PAGER 1 document. */
bool feed_parse(const char *body, Feed *out);
const char *family_name(Family f);
const char *bucket_label(Bucket b);
/* 950 -> "950", 12345 -> "12.3k", 2500000 -> "2.50M" */
void format_tokens(int64_t n, char *buf, size_t len);
