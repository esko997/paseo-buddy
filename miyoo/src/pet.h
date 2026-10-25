#pragma once
/* A no-guilt pet: it only ever gains. It eats output tokens, cheers when agents finish, and naps
 * at night. Ignoring it costs nothing; there is no hunger, decay, or sickness. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feed.h"

typedef enum { STAGE_EGG, STAGE_BABY, STAGE_KID, STAGE_TEEN, STAGE_ADULT, N_STAGES } Stage;

typedef enum { MOOD_IDLE, MOOD_HAPPY, MOOD_WORKING, MOOD_ALERT, MOOD_EATING, MOOD_SLEEP } Mood;

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
    /* Cosmetics (designed in pet.c): bit i of `unlocked` = unlock id i, kept forever. `equipped[slot]`
     * is an unlock id or -1. Slot/id catalog lives in pet.c behind the functions below. */
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
/* Grants time/date-based unlocks (e.g. seasonal, days alive). Returns newly earned ids. */
uint64_t pet_check_unlocks(Pet *p, int64_t now, int32_t tz_offset);
