#include "usage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *id, *name;
    Family family;
} KNOWN[] = {
    {"anthropic", "Claude", FAM_CLAUDE},
    {"openai-codex", "Codex", FAM_GPT},
    {"google-antigravity", "Antigrav", FAM_GEMINI},
    {"zai", "Z.ai", FAM_OTHER},
};
#define N_KNOWN (int)(sizeof KNOWN / sizeof KNOWN[0])

static int known_index(const char *provider) {
    for (int i = 0; i < N_KNOWN; i++)
        if (!strcmp(provider, KNOWN[i].id)) return i;
    return N_KNOWN;
}

UsageState usage_state(const App *app) {
    if (app->usage_at <= 0 || app->n_usage <= 0) return USAGE_NONE;
    return app_now(app) - app->usage_at > USAGE_STALE_S ? USAGE_STALE : USAGE_FRESH;
}

/* Used per mille as of now: exhausted reads full, a window that has reset since the report reads empty. */
static int row_pm(const UsageRow *r, int64_t now) {
    if (r->resets_at > 0 && r->resets_at <= now) return 0;
    if (!strcmp(r->status, "exhausted")) return 1000;
    return r->used_pm < 0 ? 0 : r->used_pm > 1000 ? 1000 : r->used_pm;
}

UsageLevel usage_level_of(int used_pm) {
    return used_pm >= USAGE_FULL_PM ? USAGE_FULL : used_pm >= USAGE_WARN_PM ? USAGE_WARN : USAGE_OK;
}

/* "5h" -> 5h (short), "7d"/"weekly"/"1w" -> wk, "30d"/"monthly" -> mo, "1d"/"daily" -> day. */
static bool window_tag(const char *id, char tag[4]) {
    char *end;
    long v = strtol(id, &end, 10);
    int n = end == id || v < 1 ? 1 : v > 999 ? 999 : (int)v;
    char u[16];
    size_t k = 0;
    for (; end[k] && k < sizeof u - 1; k++) u[k] = (char)tolower((unsigned char)end[k]);
    u[k] = 0;
    if (u[0] == 'h') {
        if (n < 24) {
            snprintf(tag, 4, "%dh", n);
            return true;
        }
        n /= 24;
        snprintf(u, sizeof u, "d");
    }
    if (!strncmp(u, "mo", 2)) snprintf(tag, 4, "mo");
    else if (u[0] == 'w') snprintf(tag, 4, n == 1 ? "wk" : "%dw", n % 100);
    else if (!strcmp(u, "d") || !strncmp(u, "da", 2)) {
        if (n == 1) snprintf(tag, 4, "day");
        else if (n == 7) snprintf(tag, 4, "wk");
        else if (n >= 28 && n <= 31) snprintf(tag, 4, "mo");
        else snprintf(tag, 4, "%dd", n % 100);
    } else snprintf(tag, 4, "%.3s", id);
    return false;
}

int usage_summarize(const App *app, UsageSummary out[], int max) {
    if (usage_state(app) == USAGE_NONE) return 0;
    int64_t now = app_now(app);
    UsageSummary all[MAX_USAGE];
    int order[MAX_USAGE], n = 0;
    bool worst_hourly[MAX_USAGE];
    for (int i = 0; i < app->n_usage && i < MAX_USAGE; i++) {
        const UsageRow *r = &app->usage[i];
        int j = 0;
        while (j < n && strcmp(all[j].provider, r->provider)) j++;
        UsageSummary *s = &all[j];
        if (j == n) {
            memset(s, 0, sizeof *s);
            snprintf(s->provider, sizeof s->provider, "%s", r->provider);
            int k = known_index(r->provider);
            snprintf(s->name, sizeof s->name, "%s", k < N_KNOWN ? KNOWN[k].name : r->provider);
            s->family = k < N_KNOWN ? KNOWN[k].family : FAM_OTHER;
            s->worst_pm = -1;
            s->short_pm = -1;
            order[n++] = k;
            worst_hourly[j] = false;
        }
        int pm = row_pm(r, now);
        char tag[4];
        bool hourly = window_tag(r->window, tag);
        if (hourly && pm > s->short_pm) {
            s->short_pm = pm;
            memcpy(s->short_tag, tag, sizeof tag);
        }
        /* on a tie the longer window says more ("wk 100%" beats "5h 100%") */
        if (pm > s->worst_pm || (pm == s->worst_pm && worst_hourly[j] && !hourly)) {
            worst_hourly[j] = hourly;
            s->worst_pm = pm;
            memcpy(s->worst_tag, tag, sizeof tag);
            s->worst_reset = pm && r->resets_at > now ? r->resets_at : 0;
        }
    }
    /* stable sort: known providers first in KNOWN order, others as reported */
    int m = 0;
    for (int k = 0; k <= N_KNOWN && m < max; k++)
        for (int j = 0; j < n && m < max; j++)
            if (order[j] == k) {
                out[m] = all[j];
                out[m].level = usage_level_of(out[m].worst_pm);
                m++;
            }
    return m;
}

UsageLevel usage_worst_level(const App *app) {
    UsageSummary s[MAX_USAGE_PROVIDERS];
    int n = usage_summarize(app, s, MAX_USAGE_PROVIDERS);
    UsageLevel worst = USAGE_OK;
    for (int i = 0; i < n; i++)
        if (s[i].level > worst) worst = s[i].level;
    return worst;
}

static bool has_word(const char *label, const char *word) {
    size_t wl = strlen(word);
    for (const char *p = label; *p; p++) {
        size_t k = 0;
        while (k < wl && p[k] && tolower((unsigned char)p[k]) == word[k]) k++;
        if (k == wl) return true;
    }
    return false;
}

UsageLevel usage_family_level(const App *app, Family f) {
    if (usage_state(app) != USAGE_FRESH) return USAGE_OK;
    static const char *const WORD[N_FAMILIES] = {"claude", "gpt", "gemini", NULL};
    int64_t now = app_now(app);
    int worst = 0;
    for (int i = 0; i < app->n_usage && i < MAX_USAGE; i++) {
        const UsageRow *r = &app->usage[i];
        int k = known_index(r->provider);
        if ((k < N_KNOWN ? KNOWN[k].family : FAM_OTHER) != f) continue;
        bool names_any = false, names_f = false;
        for (int w = 0; w < N_FAMILIES; w++)
            if (WORD[w] && has_word(r->label, WORD[w])) {
                names_any = true;
                if (w == (int)f) names_f = true;
            }
        if (names_any && !names_f) continue;
        int pm = row_pm(r, now);
        if (pm > worst) worst = pm;
    }
    return usage_level_of(worst);
}

void usage_format_eta(int64_t secs, char *buf, size_t len) {
    if (secs < 60) snprintf(buf, len, "now");
    else if (secs < 3600) snprintf(buf, len, "%lldm", (long long)(secs / 60));
    else if (secs < 86400) snprintf(buf, len, "%lldh%02lldm", (long long)(secs / 3600), (long long)(secs % 3600 / 60));
    else snprintf(buf, len, "%lldd%lldh", (long long)(secs / 86400), (long long)(secs % 86400 / 3600));
}

uint32_t usage_level_col(UsageLevel l) { return l == USAGE_FULL ? 0xe5484d : l == USAGE_WARN ? 0xf5c542 : 0x46c06f; }
