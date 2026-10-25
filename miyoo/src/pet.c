#include "pet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHEER_S (10 * 60)
#define EAT_ANIM_S 4

static const int64_t THRESHOLDS[N_STAGES] = {5000, 100000, 500000, 2000000, 0};
static const char *STAGE_NAMES[N_STAGES] = {"Egg", "Baby", "Kid", "Teen", "Adult"};

const char *stage_name(Stage s) { return STAGE_NAMES[s]; }
int64_t stage_threshold(Stage s) { return THRESHOLDS[s]; }

static int64_t local_day(int64_t now, int32_t tz_offset) {
    int64_t local = now + tz_offset;
    return local >= 0 ? local / 86400 : (local - 86399) / 86400;
}

void pet_init(Pet *p, const char *name, int64_t now) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->born = now;
    p->stage = STAGE_EGG;
    p->adult_family = FAM_OTHER;
}

bool pet_asleep(int64_t now, int32_t tz_offset) {
    int64_t local = now + tz_offset;
    int hour = (int)(((local % 86400) + 86400) % 86400 / 3600);
    return hour >= 23 || hour < 7;
}

void pet_roll_day(Pet *p, int64_t now, int32_t tz_offset) {
    int64_t day = local_day(now, tz_offset);
    if (day != p->day) {
        p->day = day;
        p->today_tokens = 0;
        p->today_turns = 0;
    }
}

Family pet_favorite(const Pet *p) {
    Family best = FAM_OTHER;
    int64_t most = 0;
    for (int i = 0; i < N_FAMILIES; i++) {
        if (p->diet[i] > most) {
            most = p->diet[i];
            best = (Family)i;
        }
    }
    return best;
}

static LedgerSeen *seen_for(Pet *p, const char *ledger_id, int64_t now, bool *created) {
    *created = false;
    LedgerSeen *oldest = &p->seen[0];
    for (int i = 0; i < PET_LEDGERS; i++) {
        if (!strcmp(p->seen[i].ledger_id, ledger_id)) return &p->seen[i];
        if (p->seen[i].last_seen < oldest->last_seen) oldest = &p->seen[i];
    }
    /* First time meeting this ledger: it started at zero when its bridge was installed, so all of it is food. */
    memset(oldest, 0, sizeof *oldest);
    snprintf(oldest->ledger_id, sizeof oldest->ledger_id, "%s", ledger_id);
    oldest->last_seen = now;
    *created = true;
    return oldest;
}

FeedResult pet_feed(Pet *p, const Feed *f, int64_t now, int32_t tz_offset) {
    FeedResult r = {0};
    bool backlog; /* first meal from this ledger: its whole history, not today's work */
    LedgerSeen *s = seen_for(p, f->ledger_id, now, &backlog);
    s->last_seen = now;
    for (int i = 0; i < N_FAMILIES; i++) {
        int64_t d = f->fed[i] - s->fed[i];
        if (d < 0) d = 0; /* ledger shrank: just rebaseline */
        p->diet[i] += d;
        r.tokens += d;
        s->fed[i] = f->fed[i];
    }
    r.turns = f->turns > s->turns ? f->turns - s->turns : 0;
    r.errors = f->errors > s->errors ? f->errors - s->errors : 0;
    s->turns = f->turns;
    s->errors = f->errors;
    if (!r.tokens && !r.turns) return r;

    pet_roll_day(p, now, tz_offset);
    Stage before = p->stage;
    p->xp += r.tokens;
    if (!backlog) {
        p->today_tokens += r.tokens;
        p->today_turns += r.turns;
        if (p->today_tokens > p->best_day_tokens) p->best_day_tokens = p->today_tokens;
    }
    p->lifetime_turns += r.turns;
    if (r.tokens > 0) p->eating_until = now + EAT_ANIM_S;
    if (r.turns > 0) p->cheer_until = now + CHEER_S;
    while (p->stage < STAGE_ADULT && p->xp >= THRESHOLDS[p->stage]) p->stage++;
    if (p->stage == STAGE_ADULT && before != STAGE_ADULT) p->adult_family = pet_favorite(p);
    r.hatched = before == STAGE_EGG && p->stage > STAGE_EGG;
    r.evolved = !r.hatched && p->stage != before;
    return r;
}

void pet_pat(Pet *p, int64_t now) { p->cheer_until = now + CHEER_S; }

Mood pet_mood(const Pet *p, int64_t now, int32_t tz_offset, int waiting, int running) {
    if (p->stage == STAGE_EGG) return waiting ? MOOD_ALERT : MOOD_IDLE;
    if (waiting) return MOOD_ALERT;
    if (now < p->eating_until) return MOOD_EATING;
    if (running) return MOOD_WORKING;
    if (now < p->cheer_until) return MOOD_HAPPY;
    if (pet_asleep(now, tz_offset)) return MOOD_SLEEP;
    return MOOD_IDLE;
}

/* ---- persistence: plain key=value text so it survives format tweaks --------------------- */

bool pet_save(const Pet *p, const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "name=%s\nborn=%lld\nxp=%lld\nstage=%d\nadult_family=%d\n", p->name, (long long)p->born,
            (long long)p->xp, p->stage, p->adult_family);
    fprintf(f, "diet=%lld %lld %lld %lld\n", (long long)p->diet[0], (long long)p->diet[1], (long long)p->diet[2],
            (long long)p->diet[3]);
    fprintf(f, "lifetime_turns=%lld\nday=%lld\ntoday_tokens=%lld\ntoday_turns=%lld\nbest_day_tokens=%lld\n",
            (long long)p->lifetime_turns, (long long)p->day, (long long)p->today_tokens, (long long)p->today_turns,
            (long long)p->best_day_tokens);
    fprintf(f, "cheer_until=%lld\n", (long long)p->cheer_until);
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *s = &p->seen[i];
        if (!s->ledger_id[0]) continue;
        fprintf(f, "seen=%s %lld %lld %lld %lld %lld %lld %lld\n", s->ledger_id, (long long)s->fed[0],
                (long long)s->fed[1], (long long)s->fed[2], (long long)s->fed[3], (long long)s->turns,
                (long long)s->errors, (long long)s->last_seen);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

bool pet_load(Pet *p, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    memset(p, 0, sizeof *p);
    p->adult_family = FAM_OTHER;
    char line[256];
    int n_seen = 0;
    long long a, b, c, d, e, g, h;
    char id[16];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "name")) snprintf(p->name, sizeof p->name, "%s", v);
        else if (!strcmp(k, "born")) p->born = atoll(v);
        else if (!strcmp(k, "xp")) p->xp = atoll(v);
        else if (!strcmp(k, "stage")) p->stage = (Stage)(atoi(v) % N_STAGES);
        else if (!strcmp(k, "adult_family")) p->adult_family = (Family)(atoi(v) % N_FAMILIES);
        else if (!strcmp(k, "lifetime_turns")) p->lifetime_turns = atoll(v);
        else if (!strcmp(k, "day")) p->day = atoll(v);
        else if (!strcmp(k, "today_tokens")) p->today_tokens = atoll(v);
        else if (!strcmp(k, "today_turns")) p->today_turns = atoll(v);
        else if (!strcmp(k, "best_day_tokens")) p->best_day_tokens = atoll(v);
        else if (!strcmp(k, "cheer_until")) p->cheer_until = atoll(v);
        else if (!strcmp(k, "diet") && sscanf(v, "%lld %lld %lld %lld", &a, &b, &c, &d) == 4) {
            p->diet[0] = a, p->diet[1] = b, p->diet[2] = c, p->diet[3] = d;
        } else if (!strcmp(k, "seen") && n_seen < PET_LEDGERS &&
                   sscanf(v, "%15s %lld %lld %lld %lld %lld %lld %lld", id, &a, &b, &c, &d, &e, &g, &h) == 8) {
            LedgerSeen *s = &p->seen[n_seen++];
            snprintf(s->ledger_id, sizeof s->ledger_id, "%s", id);
            s->fed[0] = a, s->fed[1] = b, s->fed[2] = c, s->fed[3] = d;
            s->turns = e, s->errors = g, s->last_seen = h;
        }
    }
    fclose(f);
    return true;
}
