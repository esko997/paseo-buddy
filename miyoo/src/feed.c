#include "feed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 16

/* Splits one line in place on tabs. Returns the number of fields. */
static int split(char *line, char **fields) {
    int n = 0;
    fields[n++] = line;
    for (char *p = line; *p && n < MAX_FIELDS; p++) {
        if (*p == '\t') {
            *p = 0;
            fields[n++] = p + 1;
        }
    }
    return n;
}

static void copy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static Family parse_family(const char *s) {
    if (!strcmp(s, "claude")) return FAM_CLAUDE;
    if (!strcmp(s, "gpt")) return FAM_GPT;
    if (!strcmp(s, "gemini")) return FAM_GEMINI;
    return FAM_OTHER;
}

static Bucket parse_bucket(const char *s) {
    if (!strcmp(s, "needs_input")) return B_NEEDS_INPUT;
    if (!strcmp(s, "failed")) return B_FAILED;
    if (!strcmp(s, "running")) return B_RUNNING;
    if (!strcmp(s, "attention")) return B_ATTENTION;
    return B_DONE;
}

bool feed_parse(const char *body, Feed *out) {
    memset(out, 0, sizeof *out);
    if (strncmp(body, "PAGER 1\n", 8) != 0) return false;
    char *copy_buf = strdup(body);
    if (!copy_buf) return false;
    char *save = NULL;
    for (char *line = strtok_r(copy_buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[MAX_FIELDS];
        int n = split(line, f);
        if (!strcmp(f[0], "rev") && n >= 2) {
            out->rev = atoll(f[1]);
        } else if (!strcmp(f[0], "now") && n >= 3) {
            out->now = atoll(f[1]);
            out->tz_offset = atoi(f[2]);
        } else if (!strcmp(f[0], "server") && n >= 3) {
            copy(out->ledger_id, sizeof out->ledger_id, f[1]);
            copy(out->host, sizeof out->host, f[2]);
        } else if (!strcmp(f[0], "daemon") && n >= 2) {
            out->daemon_up = !strcmp(f[1], "up");
        } else if (!strcmp(f[0], "feed") && n >= 7) {
            for (int i = 0; i < N_FAMILIES; i++) out->fed[i] = atoll(f[1 + i]);
            out->turns = atoll(f[5]);
            out->errors = atoll(f[6]);
        } else if (!strcmp(f[0], "today") && n >= 4) {
            out->today_day = atoll(f[1]);
            out->today_tokens = atoll(f[2]);
            out->today_cost_mc = atoll(f[3]);
        } else if (!strcmp(f[0], "usage_at") && n >= 2) {
            out->usage_at = atoll(f[1]);
        } else if (!strcmp(f[0], "usage") && n >= 7 && out->n_usage < MAX_USAGE) {
            UsageRow *u = &out->usage[out->n_usage++];
            copy(u->provider, sizeof u->provider, f[1]);
            copy(u->window, sizeof u->window, f[2]);
            copy(u->label, sizeof u->label, f[3]);
            u->used_pm = atoi(f[4]);
            if (u->used_pm < 0) u->used_pm = 0;
            if (u->used_pm > 1000) u->used_pm = 1000;
            u->resets_at = atoll(f[5]);
            copy(u->status, sizeof u->status, f[6]);
        } else if (!strcmp(f[0], "agent") && n >= 13 && out->n_agents < MAX_AGENTS) {
            Agent *a = &out->agents[out->n_agents++];
            copy(a->id, sizeof a->id, f[1]);
            a->bucket = parse_bucket(f[2]);
            copy(a->status, sizeof a->status, f[3]);
            copy(a->attention, sizeof a->attention, f[4]);
            a->out_tokens = atoll(f[5]);
            a->cost_cents = atoi(f[6]);
            a->ctx_pct = atoi(f[7]);
            a->updated = atoll(f[8]);
            a->family = parse_family(f[9]);
            copy(a->model, sizeof a->model, f[10]);
            copy(a->cwd, sizeof a->cwd, f[11]);
            copy(a->title, sizeof a->title, f[12]);
        }
    }
    free(copy_buf);
    return out->ledger_id[0] != 0;
}

const char *family_name(Family f) {
    static const char *names[] = {"Claude", "GPT", "Gemini", "Other"};
    return names[f];
}

const char *bucket_label(Bucket b) {
    static const char *labels[] = {"NEEDS YOU", "FAILED", "RUNNING", "FINISHED", "IDLE"};
    return labels[b];
}

void format_tokens(int64_t n, char *buf, size_t len) {
    /* Thresholds sit at the rounding boundary so 999,999 prints "1.00M", not "1000.0k". */
    if (n < 1000) snprintf(buf, len, "%lld", (long long)n);
    else if (n < 999950) snprintf(buf, len, "%.1fk", (double)n / 1e3);
    else if (n < 999995000) snprintf(buf, len, "%.2fM", (double)n / 1e6);
    else snprintf(buf, len, "%.2fB", (double)n / 1e9);
}
