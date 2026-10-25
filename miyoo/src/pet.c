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

static int local_hour(int64_t now, int32_t tz_offset) {
    int64_t local = now + tz_offset;
    return (int)(((local % 86400) + 86400) % 86400 / 3600);
}

/* ---- cosmetics catalog: every unlock is a milestone or a date, and is kept forever ---------- */

typedef struct {
    const char *name, *hint;
    WearSlot slot;
} UnlockInfo;

static const UnlockInfo UNLOCKS[N_UNLOCKS] = {
    [U_SPROUT] = {"Sprout", "Eat 100k tokens", WEAR_HAT},
    [U_PARTY_HAT] = {"Party hat", "Be here on its birthday", WEAR_HAT},
    [U_WITCH_HAT] = {"Witch hat", "Hang out in Halloween week", WEAR_HAT},
    [U_HOLIDAY_HAT] = {"Holiday hat", "Hang out in December", WEAR_HAT},
    [U_CROWN] = {"Crown", "Eat 10M tokens", WEAR_HAT},
    [U_GLASSES] = {"Round glasses", "Finish 100 turns", WEAR_FACE},
    [U_SHADES] = {"Shades", "Eat 1M tokens", WEAR_FACE},
    [U_STAR_SPECS] = {"Star specs", "Hang out at New Year", WEAR_FACE},
    [U_MONOCLE] = {"Monocle", "Finish 1000 turns", WEAR_FACE},
    [U_BOWTIE] = {"Bowtie", "Finish 10 turns", WEAR_NECK},
    [U_SCARF] = {"Cozy scarf", "Be friends for 30 days", WEAR_NECK},
    [U_MEDAL] = {"Gold medal", "Eat 1M tokens in one day", WEAR_NECK},
    [U_LOLLIPOP] = {"Lollipop", "Be friends for 7 days", WEAR_HELD},
    [U_COCOA] = {"Cocoa mug", "Snack between 0:00 and 4:00", WEAR_HELD},
    [U_WAND] = {"Star wand", "Finish 25 turns in one day", WEAR_HELD},
    [U_TROPHY] = {"Trophy", "Grow up into an adult", WEAR_DESK},
    [U_LAVA_LAMP] = {"Lava lamp", "Taste all 4 model families", WEAR_DESK},
    [U_BONSAI] = {"Bonsai", "Be friends for 100 days", WEAR_DESK},
    [U_PUMPKIN] = {"Jack-o'-lantern", "Hang out in Halloween week", WEAR_DESK},
    [U_HEART_RUG] = {"Heart rug", "Hatch from the egg", WEAR_ROOM},
    [U_STAR_RUG] = {"Star rug", "Eat 100k tokens in one day", WEAR_ROOM},
    [U_SHINY] = {"Shiny coat", "Eat 100M tokens", WEAR_SHINY},
};

static const char *SLOT_NAMES[N_WEAR] = {"Hat", "Face", "Neck", "Held", "Desk", "Room", "Shiny"};

#define BIT(id) ((uint64_t)1 << (id))

const char *pet_unlock_name(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].name : "?"; }
const char *pet_unlock_hint(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].hint : ""; }
int pet_unlock_slot(int id) { return id >= 0 && id < N_UNLOCKS ? (int)UNLOCKS[id].slot : -1; }
const char *pet_slot_name(int slot) { return slot >= 0 && slot < N_WEAR ? SLOT_NAMES[slot] : "?"; }
bool pet_has(const Pet *p, int id) { return id >= 0 && id < N_UNLOCKS && (p->unlocked & BIT(id)); }

int pet_worn(const Pet *p, int slot) {
    if (slot < 0 || slot >= N_WEAR) return -1;
    int id = p->equipped[slot];
    return pet_has(p, id) && pet_unlock_slot(id) == slot ? id : -1;
}

bool pet_toggle_equip(Pet *p, int id) {
    if (!pet_has(p, id)) return false;
    int slot = pet_unlock_slot(id);
    p->equipped[slot] = (int8_t)(p->equipped[slot] == id ? -1 : id);
    return true;
}

/* Proleptic Gregorian date of a day number (days since 1970-01-01); H. Hinnant's civil_from_days. */
static void civil(int64_t days, int *year, int *month, int *mday) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(yoe + era * 400 + (*month <= 2));
}

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* Everything the pet qualifies for right now, from its stats and the bridge-local date. */
static uint64_t qualifies(const Pet *p, int64_t now, int32_t tz_offset) {
    uint64_t m = 0;
    if (p->xp >= 100000) m |= BIT(U_SPROUT);
    if (p->xp >= 1000000) m |= BIT(U_SHADES);
    if (p->xp >= 10000000) m |= BIT(U_CROWN);
    if (p->xp >= 100000000) m |= BIT(U_SHINY);
    if (p->lifetime_turns >= 10) m |= BIT(U_BOWTIE);
    if (p->lifetime_turns >= 100) m |= BIT(U_GLASSES);
    if (p->lifetime_turns >= 1000) m |= BIT(U_MONOCLE);
    if (p->best_day_tokens >= 100000) m |= BIT(U_STAR_RUG);
    if (p->best_day_tokens >= 1000000) m |= BIT(U_MEDAL);
    if (p->today_turns >= 25 && p->day == local_day(now, tz_offset)) m |= BIT(U_WAND);
    if (p->stage > STAGE_EGG) m |= BIT(U_HEART_RUG);
    if (p->stage == STAGE_ADULT) m |= BIT(U_TROPHY);
    bool all = true;
    for (int i = 0; i < N_FAMILIES; i++) all &= p->diet[i] > 0;
    if (all) m |= BIT(U_LAVA_LAMP);

    int64_t age = now - p->born;
    if (age >= 7 * 86400) m |= BIT(U_LOLLIPOP);
    if (age >= 30 * 86400) m |= BIT(U_SCARF);
    if (age >= 100 * 86400) m |= BIT(U_BONSAI);

    int64_t today = local_day(now, tz_offset), born_day = local_day(p->born, tz_offset);
    int y, mo, d, by, bm, bd;
    civil(today, &y, &mo, &d);
    civil(born_day, &by, &bm, &bd);
    if (mo == 10 && d >= 25) m |= BIT(U_WITCH_HAT) | BIT(U_PUMPKIN);
    if (mo == 12) m |= BIT(U_HOLIDAY_HAT);
    if ((mo == 12 && d == 31) || (mo == 1 && d <= 2)) m |= BIT(U_STAR_SPECS);
    if (bm == 2 && bd == 29 && !leap(y)) bd = 28; /* leap-day pets party on the 28th */
    if (y > by && mo == bm && d == bd) m |= BIT(U_PARTY_HAT);
    return m;
}

/* Adds ids to the collection; a new item is worn right away if its slot is empty (never SHINY,
 * which recolors the pet and stays opt-in). Returns the ids that were new. */
static uint64_t grant(Pet *p, uint64_t ids) {
    uint64_t fresh = ids & ~p->unlocked;
    p->unlocked |= fresh;
    for (int id = 0; id < N_UNLOCKS; id++) {
        int slot = UNLOCKS[id].slot;
        if ((fresh & BIT(id)) && slot != WEAR_SHINY && pet_worn(p, slot) < 0) p->equipped[slot] = (int8_t)id;
    }
    return fresh;
}

uint64_t pet_check_unlocks(Pet *p, int64_t now, int32_t tz_offset) {
    return grant(p, qualifies(p, now, tz_offset));
}

void pet_init(Pet *p, const char *name, int64_t now) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->born = now;
    p->stage = STAGE_EGG;
    p->adult_family = FAM_OTHER;
    memset(p->equipped, -1, sizeof p->equipped);
}

bool pet_asleep(int64_t now, int32_t tz_offset) {
    int hour = local_hour(now, tz_offset);
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
    uint64_t earned = qualifies(p, now, tz_offset);
    if (!backlog && r.tokens > 0 && local_hour(now, tz_offset) < 4) earned |= BIT(U_COCOA); /* a night owl snack */
    r.unlocked = grant(p, earned);
    return r;
}

void pet_pat(Pet *p, int64_t now) { p->cheer_until = now + CHEER_S; }

bool pet_save_needed(const Pet *now, const Pet *saved) {
    if (now->xp != saved->xp || now->stage != saved->stage || now->adult_family != saved->adult_family ||
        now->lifetime_turns != saved->lifetime_turns || now->day != saved->day ||
        now->today_tokens != saved->today_tokens || now->today_turns != saved->today_turns ||
        now->best_day_tokens != saved->best_day_tokens || now->cheer_until != saved->cheer_until ||
        now->unlocked != saved->unlocked || strcmp(now->name, saved->name) != 0 ||
        memcmp(now->diet, saved->diet, sizeof now->diet) != 0 ||
        memcmp(now->equipped, saved->equipped, sizeof now->equipped) != 0)
        return true;
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *a = &now->seen[i], *b = &saved->seen[i];
        if (strcmp(a->ledger_id, b->ledger_id) || memcmp(a->fed, b->fed, sizeof a->fed) || a->turns != b->turns ||
            a->errors != b->errors)
            return true;
    }
    return false;
}

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
    fprintf(f, "unlocked=%llx\nequipped=", (unsigned long long)p->unlocked);
    for (int i = 0; i < 8; i++) fprintf(f, i ? " %d" : "%d", p->equipped[i]);
    fputc('\n', f);
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
    memset(p->equipped, -1, sizeof p->equipped); /* files from before cosmetics: nothing worn */
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
        else if (!strcmp(k, "unlocked")) p->unlocked = strtoull(v, NULL, 16);
        else if (!strcmp(k, "equipped")) {
            int w, n;
            for (int i = 0; i < 8 && sscanf(v, "%d%n", &w, &n) == 1; i++, v += n) p->equipped[i] = (int8_t)w;
        } else if (!strcmp(k, "diet") && sscanf(v, "%lld %lld %lld %lld", &a, &b, &c, &d) == 4) {
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
    for (int s = 0; s < 8; s++) /* only owned items, in their own slot */
        if (pet_worn(p, s) < 0) p->equipped[s] = -1;
    return true;
}
