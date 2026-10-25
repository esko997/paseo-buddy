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

/* Unlock ids are persisted (bit i of Pet.unlocked): append only, never renumber. Max 64. */
enum {
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
    N_UNLOCKS
};

#define PET_LEDGERS 8

typedef struct {
    char ledger_id[16];
    int64_t fed[N_FAMILIES];
    int64_t turns, errors;
    int64_t last_seen;
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
    /* Cosmetics: bit i of `unlocked` = unlock id i (U_*), kept forever. `equipped[WearSlot]` is an
     * unlock id or -1. Names, hints and milestones live in pet.c; art in cosmetics.h. */
    uint64_t unlocked;
    int8_t equipped[8];
} Pet;

typedef struct {
    int64_t tokens;
    int64_t turns;
    int64_t errors;
    bool hatched;
    bool evolved;
    uint64_t unlocked; /* ids newly earned by this feed (bitmask) */
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
/* Grants unlocks earned by date/age (seasonal, birthday, days alive) and any stat milestone already
 * reached (so existing pets catch up). Returns newly earned ids. Deterministic in (pet, now, tz). */
uint64_t pet_check_unlocks(Pet *p, int64_t now, int32_t tz_offset);
