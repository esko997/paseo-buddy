#pragma once
/* A no-guilt pet: it only ever gains. It eats output tokens, cheers when agents finish, and naps
 * at night. Ignoring it costs nothing; there is no hunger, decay, or sickness. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feed.h"

typedef enum { STAGE_EGG, STAGE_BABY, STAGE_KID, STAGE_TEEN, STAGE_ADULT, N_STAGES } Stage;

typedef enum { MOOD_IDLE, MOOD_HAPPY, MOOD_WORKING, MOOD_ALERT, MOOD_EATING, MOOD_SLEEP } Mood;

/* Cosmetic slots; Pet.equipped[slot]. DESK/ROOM items show in the den, SHINY recolors the pet. */
typedef enum { WEAR_HAT, WEAR_FACE, WEAR_NECK, WEAR_HELD, WEAR_DESK, WEAR_ROOM, WEAR_SHINY, N_WEAR } WearSlot;

/* A set of unlock ids 0..255: bit (id & 63) of words[id >> 6]. */
#define ITEMSET_BITS 256
typedef struct {
    uint64_t words[ITEMSET_BITS / 64];
} ItemSet;

static inline bool itemset_has(ItemSet s, int id) {
    return id >= 0 && id < ITEMSET_BITS && ((s.words[id >> 6] >> (id & 63)) & 1);
}

static inline bool itemset_empty(ItemSet s) {
    return !(s.words[0] | s.words[1] | s.words[2] | s.words[3]);
}

/* Unlock ids are persisted (bit i of Pet.unlocked): append only, never renumber. Max 256. */
enum {
    /* Milestones and dates. */
    U_SPROUT = 0,
    U_PARTY_HAT = 1,
    U_WITCH_HAT = 2,
    U_HOLIDAY_HAT = 3,
    U_CROWN = 4,
    U_GLASSES = 5,
    U_SHADES = 6,
    U_STAR_SPECS = 7,
    U_MONOCLE = 8,
    U_BOWTIE = 9,
    U_SCARF = 10,
    U_MEDAL = 11,
    U_LOLLIPOP = 12,
    U_COCOA = 13,
    U_WAND = 14,
    U_TROPHY = 15,
    U_LAVA_LAMP = 16,
    U_BONSAI = 17,
    U_PUMPKIN = 18,
    U_HEART_RUG = 19,
    U_STAR_RUG = 20,
    U_SHINY = 21,
    /* Finds (22..53): drawn one at a time from activity, each belongs to one collection set. */
    U_BERET = 22,
    U_BEANIE = 23,
    U_FLOWER_HAT = 24,
    U_MUSHROOM_HAT = 25,
    U_SPACE_HELMET = 26,
    U_ANTENNA_HAT = 27,
    U_PIRATE_HAT = 28,
    U_SUN_HAT = 29,
    U_HEART_GLASSES = 30,
    U_GOGGLES = 31,
    U_PIXEL_VISOR = 32,
    U_FLOWER_GLASSES = 33,
    U_SLEEP_MASK = 34,
    U_LEAF_SCARF = 35,
    U_SAILOR_TIE = 36,
    U_RAINBOW_SCARF = 37,
    U_MOON_PENDANT = 38,
    U_HEADPHONES = 39,
    U_BOOK = 40,
    U_UMBRELLA = 41,
    U_FLOWER = 42,
    U_GAMEPAD = 43,
    U_ROCKET = 44,
    U_SHELL = 45,
    U_LANTERN = 46,
    U_CACTUS = 47,
    U_FISHBOWL = 48,
    U_TINY_ARCADE = 49,
    U_CRYSTAL = 50,
    U_MODEL_BOAT = 51,
    U_MOON_LAMP = 52,
    U_LEAF_RUG = 53,
    /* Set rewards (54..59): granted when every base item of their set is owned. */
    U_TEA_POT = 54,
    U_SUNFLOWER = 55,
    U_PLANET_MOBILE = 56,
    U_ARCADE_CABINET = 57,
    U_LIGHTHOUSE = 58,
    U_TREASURE_CHEST = 59,
    N_UNLOCKS
};

/* Themed collection sets 0..N_COLLECTION_SETS-1: Cozy, Garden, Space, Arcade, Seaside, Explorer. */
#define N_COLLECTION_SETS 6

/* Activity points per find: 1 per completed turn, 1 per 1000 fresh output tokens. */
#define FIND_POINTS 1000

#define PET_LEDGERS 8

typedef struct {
    char ledger_id[16];
    int64_t fed[N_FAMILIES];
    int64_t turns, errors;
    int64_t last_seen;
    /* Its counts were known while finds existed, so growth past them is fresh activity. False for
     * ledgers carried over from saves made before finds (their first delta may be pre-upgrade work). */
    bool finds_armed;
} LedgerSeen;

typedef struct {
    char name[16];
    int64_t born;
    int64_t xp; /* lifetime output tokens eaten */
    int64_t diet[N_FAMILIES];
    Stage stage;
    Family adult_family; /* locked in when it becomes an adult */
    int64_t lifetime_turns;
    int64_t day;          /* bridge-local day number the today_* counters belong to */
    int64_t today_tokens; /* resets at local midnight; a stat, never a requirement */
    int64_t today_turns;
    int64_t best_day_tokens;
    int64_t cheer_until;  /* unix seconds: happy after a finish or a pat */
    int64_t eating_until; /* unix seconds; not persisted */
    LedgerSeen seen[PET_LEDGERS];
    /* Cosmetics: `unlocked` holds unlock ids (U_*), kept forever; unknown future ids are preserved.
     * `equipped[WearSlot]` is an unlock id or -1 (index 7 is unused, always -1). Names, hints,
     * milestones and sets live in pet.c; art in cosmetics.h. */
    ItemSet unlocked;
    int16_t equipped[8];
    /* Finds: fresh activity banks points (capped at what the unowned finds need); each FIND_POINTS
     * draws one unowned find, at most one per bridge-local day. */
    int64_t find_points;
    int64_t find_tokens; /* fresh output tokens not yet worth a point, 0..999 */
    int64_t find_day;    /* bridge-local day (as `day`) of the last drawn find; -1 = welcome find pending */
    uint64_t find_rng;   /* pet-specific draw sequence state */
} Pet;

typedef struct {
    int64_t tokens;
    int64_t turns;
    int64_t errors;
    bool hatched;
    bool evolved;
    ItemSet unlocked; /* ids newly earned by this feed */
} FeedResult;

void pet_init(Pet *p, const char *name, int64_t now);
bool pet_load(Pet *p, const char *path);
bool pet_save(const Pet *p, const char *path);

/* Eats token growth reported by one bridge (deltas against what this ledger reported before). */
FeedResult pet_feed(Pet *p, const Feed *f, int64_t now, int32_t tz_offset);
void pet_pat(Pet *p, int64_t now);

bool pet_asleep(int64_t now, int32_t tz_offset);
Mood pet_mood(const Pet *p, int64_t now, int32_t tz_offset, int waiting, int running);
const char *stage_name(Stage s);
/* Lifetime tokens needed to reach the next stage (0 for adults). */
int64_t stage_threshold(Stage s);
/* Family that has fed it the most so far; FAM_OTHER with no food at all. */
Family pet_favorite(const Pet *p);
/* Today's counters, reset if the bridge-local day changed. */
void pet_roll_day(Pet *p, int64_t now, int32_t tz_offset);
/* True if anything persisted differs (ignores bookkeeping like LedgerSeen.last_seen and eating_until). */
bool pet_save_needed(const Pet *now, const Pet *saved);
/* Display name of an unlock id, e.g. "Party hat". */
const char *pet_unlock_name(int id);
/* How to earn it, e.g. "Eat 1M tokens". */
const char *pet_unlock_hint(int id);
/* WearSlot of an unlock id, -1 if the id is unknown. */
int pet_unlock_slot(int id);
const char *pet_slot_name(int slot);
bool pet_has(const Pet *p, int id);
/* Id worn in a slot, or -1. */
int pet_worn(const Pet *p, int slot);
/* Wears an owned id in its slot, or takes it off if it is already worn. False if not owned. */
bool pet_toggle_equip(Pet *p, int id);
/* Grants unlocks earned by date/age (seasonal, birthday, days alive), any stat milestone already
 * reached (so existing pets catch up), a find from banked points on a new day, and completed set
 * rewards. Returns newly earned ids. Deterministic in (pet, now, tz). */
ItemSet pet_check_unlocks(Pet *p, int64_t now, int32_t tz_offset);

/* Collection sets. */
const char *pet_set_name(int set);
/* Set an id belongs to (base item or reward), -1 if none. */
int pet_unlock_set(int id);
/* Reward id granted for completing a set, -1 if the set is unknown. */
int pet_set_reward(int set);
/* Owned and total base items of a set (the reward is not counted). */
void pet_set_progress(const Pet *p, int set, int *have, int *total);

/* True for ids drawn from activity (the base items of the sets). */
bool pet_unlock_is_find(int id);
/* Unowned finds left to draw. */
int pet_finds_left(const Pet *p);
/* Banked points toward the next find, 0..FIND_POINTS-1 (999 once a find is ready but today's
 * draw is used); 0 when every find is owned. */
int64_t pet_find_progress(const Pet *p);
/* A find is ready but waits for a later bridge-local day (today's draw is used). */
bool pet_find_waiting(const Pet *p, int64_t now, int32_t tz_offset);
