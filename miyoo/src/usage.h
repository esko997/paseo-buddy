#pragma once
/* Subscription limits (App.usage, from `omp usage` on one bridge) summarised per provider for the
 * agents strip, the header fuel icon and the den's fuel panel. Pure functions of the App. */
#include <stddef.h>
#include <stdint.h>

#include "app.h"

#define USAGE_WARN_PM 800 /* used >= 80%: yellow, robots of that provider look tired */
#define USAGE_FULL_PM 950 /* used >= 95% (or omp says "exhausted"): red, robots look out of fuel */
#define USAGE_STALE_S 1200 /* data older than 20 minutes is drawn dimmed */
#define MAX_USAGE_PROVIDERS 8

typedef enum { USAGE_OK, USAGE_WARN, USAGE_FULL } UsageLevel;
typedef enum { USAGE_NONE, USAGE_FRESH, USAGE_STALE } UsageState;

typedef struct {
    char provider[24];
    char name[12]; /* Claude, Codex, Antigrav, Z.ai, else the provider id */
    Family family;
    int worst_pm;        /* highest used per mille over the provider's windows (exhausted = 1000) */
    char worst_tag[4];   /* that window: 5h, wk, mo, day, ... */
    int64_t worst_reset; /* unix s, 0 if unknown */
    int short_pm;        /* hour-scale (5h) window, -1 if the provider has none */
    UsageLevel level;
    char short_tag[4];   /* its tag, e.g. "5h" */
} UsageSummary;

/* NONE if no bridge has reported usage, STALE if the newest report is older than USAGE_STALE_S. */
UsageState usage_state(const App *app);
/* Fills out[] with one summary per provider, in a stable order (anthropic, openai-codex,
 * google-antigravity, zai, then others as reported); returns the count (<= max). Windows whose
 * reset time has passed since the report count as unused. */
int usage_summarize(const App *app, UsageSummary out[], int max);
/* Highest level over all providers (USAGE_OK when there is no data). */
UsageLevel usage_worst_level(const App *app);
/* How spent the robots of a model family should look. Only fresh data counts, and windows whose
 * label names other model families are skipped ("Claude & GPT (shared)" doesn't tire Gemini). */
UsageLevel usage_family_level(const App *app, Family f);
/* OK below USAGE_WARN_PM, WARN below USAGE_FULL_PM, else FULL. */
UsageLevel usage_level_of(int used_pm);
/* Seconds until reset as "now", "45m", "2h18m", "4d16h". */
void usage_format_eta(int64_t secs, char *buf, size_t len);
/* Green / yellow / red, matching the context-window colours. */
uint32_t usage_level_col(UsageLevel l);
