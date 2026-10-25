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

/* ---- cosmetics catalog: every unlock is a milestone, a date, a find or a set reward, kept forever */

_Static_assert(N_UNLOCKS <= ITEMSET_BITS, "unlock ids must fit an ItemSet");

static void itemset_add(ItemSet *s, int id) {
    if (id >= 0 && id < ITEMSET_BITS) s->words[id >> 6] |= (uint64_t)1 << (id & 63);
}

enum { SET_COZY, SET_GARDEN, SET_SPACE, SET_ARCADE, SET_SEASIDE, SET_EXPLORER };
#define COZY "Cozy"
#define GARDEN "Garden"
#define SPACE "Space"
#define ARCADE "Arcade"
#define SEASIDE "Seaside"
#define EXPLORER "Explorer"

typedef struct {
    const char *name, *hint;
    WearSlot slot;
    int8_t set; /* collection set (base item or reward), -1 for none */
    bool find;  /* drawn from activity; the base items of the sets */
} UnlockInfo;

#define MILESTONE(n, h, sl) {n, h, sl, -1, false}
#define FIND(n, sl, set, setname) {n, "Found while you work; " setname " set", sl, set, true}
#define REWARD(n, set, setname) {n, "Complete the " setname " set", WEAR_DESK, set, false}

static const UnlockInfo UNLOCKS[N_UNLOCKS] = {
    [U_SPROUT] = MILESTONE("Sprout", "Eat 100k tokens", WEAR_HAT),
    [U_PARTY_HAT] = MILESTONE("Party hat", "Be here on its birthday", WEAR_HAT),
    [U_WITCH_HAT] = MILESTONE("Witch hat", "Hang out in Halloween week", WEAR_HAT),
    [U_HOLIDAY_HAT] = MILESTONE("Holiday hat", "Hang out in December", WEAR_HAT),
    [U_CROWN] = MILESTONE("Crown", "Eat 10M tokens", WEAR_HAT),
    [U_GLASSES] = MILESTONE("Round glasses", "Finish 100 turns", WEAR_FACE),
    [U_SHADES] = MILESTONE("Shades", "Eat 1M tokens", WEAR_FACE),
    [U_STAR_SPECS] = MILESTONE("Star specs", "Hang out at New Year", WEAR_FACE),
    [U_MONOCLE] = MILESTONE("Monocle", "Finish 1000 turns", WEAR_FACE),
    [U_BOWTIE] = MILESTONE("Bowtie", "Finish 10 turns", WEAR_NECK),
    [U_SCARF] = MILESTONE("Cozy scarf", "Be friends for 30 days", WEAR_NECK),
    [U_MEDAL] = MILESTONE("Gold medal", "Eat 1M tokens in one day", WEAR_NECK),
    [U_LOLLIPOP] = MILESTONE("Lollipop", "Be friends for 7 days", WEAR_HELD),
    [U_COCOA] = MILESTONE("Cocoa mug", "Snack between 0:00 and 4:00", WEAR_HELD),
    [U_WAND] = MILESTONE("Star wand", "Finish 25 turns in one day", WEAR_HELD),
    [U_TROPHY] = MILESTONE("Trophy", "Grow up into an adult", WEAR_DESK),
    [U_LAVA_LAMP] = MILESTONE("Lava lamp", "Taste all 4 model families", WEAR_DESK),
    [U_BONSAI] = MILESTONE("Bonsai", "Be friends for 100 days", WEAR_DESK),
    [U_PUMPKIN] = MILESTONE("Jack-o'-lantern", "Hang out in Halloween week", WEAR_DESK),
    [U_HEART_RUG] = MILESTONE("Heart rug", "Hatch from the egg", WEAR_ROOM),
    [U_STAR_RUG] = MILESTONE("Star rug", "Eat 100k tokens in one day", WEAR_ROOM),
    [U_SHINY] = MILESTONE("Shiny coat", "Eat 100M tokens", WEAR_SHINY),

    [U_BERET] = FIND("Beret", WEAR_HAT, SET_COZY, COZY),
    [U_BEANIE] = FIND("Beanie", WEAR_HAT, SET_COZY, COZY),
    [U_FLOWER_HAT] = FIND("Flower hat", WEAR_HAT, SET_GARDEN, GARDEN),
    [U_MUSHROOM_HAT] = FIND("Mushroom cap", WEAR_HAT, SET_GARDEN, GARDEN),
    [U_SPACE_HELMET] = FIND("Space helmet", WEAR_HAT, SET_SPACE, SPACE),
    [U_ANTENNA_HAT] = FIND("Antenna", WEAR_HAT, SET_SPACE, SPACE),
    [U_PIRATE_HAT] = FIND("Pirate hat", WEAR_HAT, SET_EXPLORER, EXPLORER),
    [U_SUN_HAT] = FIND("Sun hat", WEAR_HAT, SET_SEASIDE, SEASIDE),
    [U_HEART_GLASSES] = FIND("Heart glasses", WEAR_FACE, SET_ARCADE, ARCADE),
    [U_GOGGLES] = FIND("Goggles", WEAR_FACE, SET_EXPLORER, EXPLORER),
    [U_PIXEL_VISOR] = FIND("Pixel visor", WEAR_FACE, SET_ARCADE, ARCADE),
    [U_FLOWER_GLASSES] = FIND("Flower glasses", WEAR_FACE, SET_GARDEN, GARDEN),
    [U_SLEEP_MASK] = FIND("Sleep mask", WEAR_FACE, SET_COZY, COZY),
    [U_LEAF_SCARF] = FIND("Leaf scarf", WEAR_NECK, SET_GARDEN, GARDEN),
    [U_SAILOR_TIE] = FIND("Sailor tie", WEAR_NECK, SET_SEASIDE, SEASIDE),
    [U_RAINBOW_SCARF] = FIND("Rainbow scarf", WEAR_NECK, SET_COZY, COZY),
    [U_MOON_PENDANT] = FIND("Moon pendant", WEAR_NECK, SET_SPACE, SPACE),
    [U_HEADPHONES] = FIND("Headphones", WEAR_NECK, SET_ARCADE, ARCADE),
    [U_BOOK] = FIND("Book", WEAR_HELD, SET_COZY, COZY),
    [U_UMBRELLA] = FIND("Umbrella", WEAR_HELD, SET_EXPLORER, EXPLORER),
    [U_FLOWER] = FIND("Flower", WEAR_HELD, SET_GARDEN, GARDEN),
    [U_GAMEPAD] = FIND("Gamepad", WEAR_HELD, SET_ARCADE, ARCADE),
    [U_ROCKET] = FIND("Toy rocket", WEAR_HELD, SET_SPACE, SPACE),
    [U_SHELL] = FIND("Seashell", WEAR_HELD, SET_SEASIDE, SEASIDE),
    [U_LANTERN] = FIND("Lantern", WEAR_HELD, SET_EXPLORER, EXPLORER),
    [U_CACTUS] = FIND("Cactus", WEAR_DESK, SET_GARDEN, GARDEN),
    [U_FISHBOWL] = FIND("Fishbowl", WEAR_DESK, SET_SEASIDE, SEASIDE),
    [U_TINY_ARCADE] = FIND("Tiny arcade", WEAR_DESK, SET_ARCADE, ARCADE),
    [U_CRYSTAL] = FIND("Crystal", WEAR_DESK, SET_SPACE, SPACE),
    [U_MODEL_BOAT] = FIND("Model boat", WEAR_DESK, SET_SEASIDE, SEASIDE),
    [U_MOON_LAMP] = FIND("Moon lamp", WEAR_DESK, SET_SPACE, SPACE),
    [U_LEAF_RUG] = FIND("Leaf rug", WEAR_ROOM, SET_GARDEN, GARDEN),

    [U_TEA_POT] = REWARD("Teapot", SET_COZY, COZY),
    [U_SUNFLOWER] = REWARD("Sunflower", SET_GARDEN, GARDEN),
    [U_PLANET_MOBILE] = REWARD("Planet mobile", SET_SPACE, SPACE),
    [U_ARCADE_CABINET] = REWARD("Arcade cabinet", SET_ARCADE, ARCADE),
    [U_LIGHTHOUSE] = REWARD("Lighthouse", SET_SEASIDE, SEASIDE),
    [U_TREASURE_CHEST] = REWARD("Treasure chest", SET_EXPLORER, EXPLORER),
};

typedef struct {
    const char *name;
    int reward;
} CollectionSet;

static const CollectionSet SETS[N_COLLECTION_SETS] = {
    [SET_COZY] = {COZY, U_TEA_POT},
    [SET_GARDEN] = {GARDEN, U_SUNFLOWER},
    [SET_SPACE] = {SPACE, U_PLANET_MOBILE},
    [SET_ARCADE] = {ARCADE, U_ARCADE_CABINET},
    [SET_SEASIDE] = {SEASIDE, U_LIGHTHOUSE},
    [SET_EXPLORER] = {EXPLORER, U_TREASURE_CHEST},
};

static const char *SLOT_NAMES[N_WEAR] = {"Hat", "Face", "Neck", "Held", "Desk", "Room", "Shiny"};

const char *pet_unlock_name(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].name : "?"; }
const char *pet_unlock_hint(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].hint : ""; }
int pet_unlock_slot(int id) { return id >= 0 && id < N_UNLOCKS ? (int)UNLOCKS[id].slot : -1; }
const char *pet_slot_name(int slot) { return slot >= 0 && slot < N_WEAR ? SLOT_NAMES[slot] : "?"; }
bool pet_has(const Pet *p, int id) { return id >= 0 && id < N_UNLOCKS && itemset_has(p->unlocked, id); }

const char *pet_set_name(int set) { return set >= 0 && set < N_COLLECTION_SETS ? SETS[set].name : "?"; }
int pet_unlock_set(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].set : -1; }
int pet_set_reward(int set) { return set >= 0 && set < N_COLLECTION_SETS ? SETS[set].reward : -1; }
bool pet_unlock_is_find(int id) { return id >= 0 && id < N_UNLOCKS && UNLOCKS[id].find; }

void pet_set_progress(const Pet *p, int set, int *have, int *total) {
    *have = *total = 0;
    for (int id = 0; id < N_UNLOCKS; id++) {
        if (!UNLOCKS[id].find || UNLOCKS[id].set != set) continue;
        ++*total;
        *have += pet_has(p, id);
    }
}

int pet_finds_left(const Pet *p) {
    int n = 0;
    for (int id = 0; id < N_UNLOCKS; id++) n += UNLOCKS[id].find && !pet_has(p, id);
    return n;
}

int64_t pet_find_progress(const Pet *p) {
    if (!pet_finds_left(p)) return 0;
    return p->find_points < FIND_POINTS - 1 ? p->find_points : FIND_POINTS - 1;
}

bool pet_find_waiting(const Pet *p, int64_t now, int32_t tz_offset) {
    return p->find_points >= FIND_POINTS && p->find_day >= 0 && local_day(now, tz_offset) <= p->find_day &&
           pet_finds_left(p) > 0;
}

int pet_worn(const Pet *p, int slot) {
    if (slot < 0 || slot >= N_WEAR) return -1;
    int id = p->equipped[slot];
    return pet_has(p, id) && pet_unlock_slot(id) == slot ? id : -1;
}

bool pet_toggle_equip(Pet *p, int id) {
    if (!pet_has(p, id)) return false;
    int slot = pet_unlock_slot(id);
    p->equipped[slot] = (int16_t)(p->equipped[slot] == id ? -1 : id);
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

/* Milestones and dates the pet qualifies for right now, from its stats and the bridge-local date. */
static ItemSet qualifies(const Pet *p, int64_t now, int32_t tz_offset) {
    ItemSet m = {{0}};
    if (p->xp >= 100000) itemset_add(&m, U_SPROUT);
    if (p->xp >= 1000000) itemset_add(&m, U_SHADES);
    if (p->xp >= 10000000) itemset_add(&m, U_CROWN);
    if (p->xp >= 100000000) itemset_add(&m, U_SHINY);
    if (p->lifetime_turns >= 10) itemset_add(&m, U_BOWTIE);
    if (p->lifetime_turns >= 100) itemset_add(&m, U_GLASSES);
    if (p->lifetime_turns >= 1000) itemset_add(&m, U_MONOCLE);
    if (p->best_day_tokens >= 100000) itemset_add(&m, U_STAR_RUG);
    if (p->best_day_tokens >= 1000000) itemset_add(&m, U_MEDAL);
    if (p->today_turns >= 25 && p->day == local_day(now, tz_offset)) itemset_add(&m, U_WAND);
    if (p->stage > STAGE_EGG) itemset_add(&m, U_HEART_RUG);
    if (p->stage == STAGE_ADULT) itemset_add(&m, U_TROPHY);
    bool all = true;
    for (int i = 0; i < N_FAMILIES; i++) all &= p->diet[i] > 0;
    if (all) itemset_add(&m, U_LAVA_LAMP);

    int64_t age = now - p->born;
    if (age >= 7 * 86400) itemset_add(&m, U_LOLLIPOP);
    if (age >= 30 * 86400) itemset_add(&m, U_SCARF);
    if (age >= 100 * 86400) itemset_add(&m, U_BONSAI);

    int64_t today = local_day(now, tz_offset), born_day = local_day(p->born, tz_offset);
    int y, mo, d, by, bm, bd;
    civil(today, &y, &mo, &d);
    civil(born_day, &by, &bm, &bd);
    if (mo == 10 && d >= 25) itemset_add(&m, U_WITCH_HAT), itemset_add(&m, U_PUMPKIN);
    if (mo == 12) itemset_add(&m, U_HOLIDAY_HAT);
    if ((mo == 12 && d == 31) || (mo == 1 && d <= 2)) itemset_add(&m, U_STAR_SPECS);
    if (bm == 2 && bd == 29 && !leap(y)) bd = 28; /* leap-day pets party on the 28th */
    if (y > by && mo == bm && d == bd) itemset_add(&m, U_PARTY_HAT);
    return m;
}

/* Adds ids to the collection plus the reward of every set that is now complete; a new item is worn
 * right away if its slot is empty (never SHINY, which recolors the pet and stays opt-in). Returns
 * the ids that were new. */
static ItemSet award(Pet *p, ItemSet ids) {
    ItemSet fresh;
    for (int w = 0; w < ITEMSET_BITS / 64; w++) {
        fresh.words[w] = ids.words[w] & ~p->unlocked.words[w];
        p->unlocked.words[w] |= fresh.words[w];
    }
    for (int set = 0; set < N_COLLECTION_SETS; set++) {
        int have, total, reward = SETS[set].reward;
        pet_set_progress(p, set, &have, &total);
        if (have == total && !pet_has(p, reward)) {
            itemset_add(&fresh, reward);
            itemset_add(&p->unlocked, reward);
        }
    }
    for (int id = 0; id < N_UNLOCKS; id++) {
        int slot = UNLOCKS[id].slot;
        if (itemset_has(fresh, id) && slot != WEAR_SHINY && pet_worn(p, slot) < 0) p->equipped[slot] = (int16_t)id;
    }
    return fresh;
}

/* ---- finds: fresh activity banks points; each FIND_POINTS draws one unowned find ------------- */

/* SplitMix64: a fixed, portable sequence (no libc rand), so reopening the app cannot reroll. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Each pet gets its own draw order, from its name and birth time (FNV-1a, then mixed). */
static uint64_t find_seed(const Pet *p) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    for (const char *c = p->name; *c; c++) h = (h ^ (uint8_t)*c) * UINT64_C(0x100000001B3);
    h ^= (uint64_t)p->born;
    return splitmix64(&h);
}

static int64_t sat_add(int64_t a, int64_t b) { return b > INT64_MAX - a ? INT64_MAX : a + b; } /* a, b >= 0 */

/* Banks fresh activity: 1 point per completed turn, 1 per 1000 output tokens (remainder kept). */
static void add_activity(Pet *p, int64_t tokens, int64_t turns) {
    int64_t rem = p->find_tokens + tokens % 1000;
    p->find_tokens = rem % 1000;
    int64_t points = sat_add(tokens / 1000, rem / 1000);
    p->find_points = sat_add(sat_add(p->find_points, points), turns);
}

/* Never bank more than the unowned finds can use. */
static void cap_points(Pet *p) {
    int64_t cap = (int64_t)pet_finds_left(p) * FIND_POINTS;
    if (p->find_points > cap) p->find_points = cap;
}

/* Draws at most one unowned find per bridge-local day: the welcome find on the first fresh activity,
 * later one per FIND_POINTS banked. A day at or before the last draw (a clock or zone moving back)
 * never draws. Returns the id, or -1. The caller grants it. */
static int draw_find(Pet *p, int64_t today, bool welcome_ok) {
    int left = pet_finds_left(p);
    if (!left || today < 0) return -1;
    if (p->find_day < 0) {
        if (!welcome_ok) return -1;
    } else {
        if (today <= p->find_day || p->find_points < FIND_POINTS) return -1;
        p->find_points -= FIND_POINTS;
    }
    p->find_day = today;
    uint64_t k = splitmix64(&p->find_rng) % (uint64_t)left;
    for (int id = 0; id < N_UNLOCKS; id++)
        if (UNLOCKS[id].find && !pet_has(p, id) && k-- == 0) return id;
    return -1;
}

ItemSet pet_check_unlocks(Pet *p, int64_t now, int32_t tz_offset) {
    ItemSet earned = qualifies(p, now, tz_offset);
    itemset_add(&earned, draw_find(p, local_day(now, tz_offset), false));
    ItemSet fresh = award(p, earned);
    cap_points(p);
    return fresh;
}

void pet_init(Pet *p, const char *name, int64_t now) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->born = now;
    p->stage = STAGE_EGG;
    p->adult_family = FAM_OTHER;
    memset(p->equipped, -1, sizeof p->equipped);
    p->find_day = -1;
    p->find_rng = find_seed(p);
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
    oldest->finds_armed = true; /* its history is backlog; growth from here on is fresh */
    *created = true;
    return oldest;
}

FeedResult pet_feed(Pet *p, const Feed *f, int64_t now, int32_t tz_offset) {
    FeedResult r = {0};
    bool backlog; /* first meal from this ledger: its whole history, not today's work */
    LedgerSeen *s = seen_for(p, f->ledger_id, now, &backlog);
    /* Finds count only growth past counts known since finds existed: never a new bridge's history
     * nor what a ledger carried over from an older save did before the upgrade. */
    bool fresh = !backlog && s->finds_armed;
    s->finds_armed = true;
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
    ItemSet earned = qualifies(p, now, tz_offset);
    if (!backlog && r.tokens > 0 && local_hour(now, tz_offset) < 4) itemset_add(&earned, U_COCOA); /* a night owl snack */
    if (fresh) add_activity(p, r.tokens, r.turns);
    itemset_add(&earned, draw_find(p, local_day(now, tz_offset), fresh));
    r.unlocked = award(p, earned);
    cap_points(p);
    return r;
}

void pet_pat(Pet *p, int64_t now) { p->cheer_until = now + CHEER_S; }

bool pet_save_needed(const Pet *now, const Pet *saved) {
    if (now->xp != saved->xp || now->stage != saved->stage || now->adult_family != saved->adult_family ||
        now->lifetime_turns != saved->lifetime_turns || now->day != saved->day ||
        now->today_tokens != saved->today_tokens || now->today_turns != saved->today_turns ||
        now->best_day_tokens != saved->best_day_tokens || now->cheer_until != saved->cheer_until ||
        memcmp(&now->unlocked, &saved->unlocked, sizeof now->unlocked) != 0 || strcmp(now->name, saved->name) != 0 ||
        memcmp(now->diet, saved->diet, sizeof now->diet) != 0 ||
        memcmp(now->equipped, saved->equipped, sizeof now->equipped) != 0 || now->find_points != saved->find_points ||
        now->find_tokens != saved->find_tokens || now->find_day != saved->find_day || now->find_rng != saved->find_rng)
        return true;
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *a = &now->seen[i], *b = &saved->seen[i];
        if (strcmp(a->ledger_id, b->ledger_id) || memcmp(a->fed, b->fed, sizeof a->fed) || a->turns != b->turns ||
            a->errors != b->errors || a->finds_armed != b->finds_armed)
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
    /* Lowest word first: older builds read only the first word, i.e. ids 0..63. */
    fputs("unlocked=", f);
    for (int w = 0; w < ITEMSET_BITS / 64; w++) fprintf(f, w ? " %llx" : "%llx", (unsigned long long)p->unlocked.words[w]);
    fputs("\nequipped=", f);
    for (int i = 0; i < N_WEAR; i++) fprintf(f, i ? " %d" : "%d", p->equipped[i]);
    fprintf(f, "\nfind_points=%lld\nfind_tokens=%lld\nfind_day=%lld\nfind_rng=%llx\n", (long long)p->find_points,
            (long long)p->find_tokens, (long long)p->find_day, (unsigned long long)p->find_rng);
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *s = &p->seen[i];
        if (!s->ledger_id[0]) continue;
        fprintf(f, "seen=%s %lld %lld %lld %lld %lld %lld %lld %d\n", s->ledger_id, (long long)s->fed[0],
                (long long)s->fed[1], (long long)s->fed[2], (long long)s->fed[3], (long long)s->turns,
                (long long)s->errors, (long long)s->last_seen, s->finds_armed);
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
    p->find_day = -1;                            /* files from before finds: the welcome find is pending */
    bool have_rng = false;
    char line[256];
    int n_seen = 0;
    long long a, b, c, d, e, g, h;
    int armed;
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
        else if (!strcmp(k, "find_points")) p->find_points = atoll(v);
        else if (!strcmp(k, "find_tokens")) p->find_tokens = atoll(v);
        else if (!strcmp(k, "find_day")) p->find_day = atoll(v);
        else if (!strcmp(k, "find_rng")) p->find_rng = strtoull(v, NULL, 16), have_rng = true;
        else if (!strcmp(k, "unlocked")) { /* one hex word per 64 ids, lowest first; older files have one */
            char *end;
            for (int w = 0; w < ITEMSET_BITS / 64; w++, v = end) {
                unsigned long long bits = strtoull(v, &end, 16);
                if (end == v) break;
                p->unlocked.words[w] = bits;
            }
        } else if (!strcmp(k, "equipped")) { /* older files carry an unused eighth value: ignored */
            int w, n;
            for (int i = 0; i < N_WEAR && sscanf(v, "%d%n", &w, &n) == 1; i++, v += n)
                p->equipped[i] = (int16_t)(w >= 0 && w < ITEMSET_BITS ? w : -1);
        } else if (!strcmp(k, "diet") && sscanf(v, "%lld %lld %lld %lld", &a, &b, &c, &d) == 4) {
            p->diet[0] = a, p->diet[1] = b, p->diet[2] = c, p->diet[3] = d;
        } else if (!strcmp(k, "seen") && n_seen < PET_LEDGERS) {
            int got = sscanf(v, "%15s %lld %lld %lld %lld %lld %lld %lld %d", id, &a, &b, &c, &d, &e, &g, &h, &armed);
            if (got < 8) continue;
            LedgerSeen *s = &p->seen[n_seen++];
            snprintf(s->ledger_id, sizeof s->ledger_id, "%s", id);
            s->fed[0] = a, s->fed[1] = b, s->fed[2] = c, s->fed[3] = d;
            s->turns = e, s->errors = g, s->last_seen = h;
            s->finds_armed = got == 9 && armed; /* older files: counts may include pre-upgrade work */
        }
    }
    fclose(f);
    for (int s = 0; s < N_WEAR; s++) /* only owned items, in their own slot */
        if (pet_worn(p, s) < 0) p->equipped[s] = -1;
    if (!have_rng) p->find_rng = find_seed(p); /* first load since finds: no backlog, fresh sequence */
    if (p->find_points < 0) p->find_points = 0;
    if (p->find_tokens < 0 || p->find_tokens >= 1000) p->find_tokens = 0;
    if (p->find_day < -1) p->find_day = -1;
    cap_points(p);
    return true;
}
