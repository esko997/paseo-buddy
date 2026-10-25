#pragma once
/* A no-guilt pet: it only ever gains. It eats output tokens, cheers when agents finish, and naps
 * at night. Ignoring it costs nothing; there is no hunger, decay, or sickness. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feed.h"

typedef enum { STAGE_EGG, STAGE_BABY, STAGE_KID, STAGE_TEEN, STAGE_ADULT, N_STAGES } Stage;

typedef enum { MOOD_IDLE, MOOD_HAPPY, MOOD_WORKING, MOOD_ALERT, MOOD_EATING, MOOD_SLEEP } Mood;

/* Cosmetic slots; Pet.equipped[slot]. Append only: slots are persisted by position.
 * HAT..HELD are worn, DESK/ROOM sit in the den, SHINY (shown as "Coat") recolors the pet,
 * WALL/WINDOW redecorate the den and BOT_* dress the robot workers of one model family. */
typedef enum {
    WEAR_HAT,
    WEAR_FACE,
    WEAR_NECK,
    WEAR_HELD,
    WEAR_DESK,
    WEAR_ROOM,
    WEAR_SHINY,
    WEAR_WALL,
    WEAR_WINDOW,
    WEAR_BOT_CLAUDE,
    WEAR_BOT_GPT,
    WEAR_BOT_GEMINI,
    WEAR_BOT_OTHER,
    N_WEAR
} WearSlot;

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
    /* Second collection (60..119): six themes of ten, by offset: 0 hat, 1 held, 2 desk, 3 rug,
     * 4 wallpaper, 5 window, 6-7 robot costumes, 8 coat, 9 set reward (neck). Offsets 0..8 are finds. */
    U_DIVER_HAT = 60,
    U_TRIDENT = 61,
    U_CORAL = 62,
    U_WAVE_RUG = 63,
    U_OCEAN_WALL = 64,
    U_REEF_VIEW = 65,
    U_DIVER_BOT = 66,
    U_SUB_BOT = 67,
    U_LAGOON_COAT = 68,
    U_PEARL_NECKLACE = 69,
    U_ACORN_HAT = 70,
    U_PINECONE = 71,
    U_BIRDHOUSE = 72,
    U_FERN_RUG = 73,
    U_FOREST_WALL = 74,
    U_FOREST_VIEW = 75,
    U_RANGER_BOT = 76,
    U_OWL_BOT = 77,
    U_MOSS_COAT = 78,
    U_ACORN_CHARM = 79,
    U_COMET_HAT = 80,
    U_TELESCOPE = 81,
    U_ORRERY = 82,
    U_ORBIT_RUG = 83,
    U_STAR_WALL = 84,
    U_NEBULA_VIEW = 85,
    U_ASTRONAUT_BOT = 86,
    U_SATELLITE_BOT = 87,
    U_NEBULA_COAT = 88,
    U_CONSTELLATION = 89,
    U_HARD_HAT = 90,
    U_WRENCH = 91,
    U_TOOLBOX = 92,
    U_GEAR_RUG = 93,
    U_BLUEPRINT_WALL = 94,
    U_CITY_VIEW = 95,
    U_MECHANIC_BOT = 96,
    U_CLOCKWORK_BOT = 97,
    U_COPPER_COAT = 98,
    U_GEAR_PENDANT = 99,
    U_CUPCAKE_HAT = 100,
    U_ICE_CREAM = 101,
    U_GUMBALL = 102,
    U_CANDY_RUG = 103,
    U_CANDY_WALL = 104,
    U_CANDY_VIEW = 105,
    U_PATISSIER_BOT = 106,
    U_SODA_BOT = 107,
    U_SORBET_COAT = 108,
    U_CANDY_BOW = 109,
    U_SNOW_HAT = 110,
    U_SNOWGLOBE = 111,
    U_IGLOO = 112,
    U_SNOW_RUG = 113,
    U_FROST_WALL = 114,
    U_AURORA_VIEW = 115,
    U_PENGUIN_BOT = 116,
    U_SNOW_BOT = 117,
    U_FROST_COAT = 118,
    U_SNOW_SCARF = 119,
    N_UNLOCKS
};

/* Themed collection sets 0..N_COLLECTION_SETS-1: Cozy, Garden, Space, Arcade, Seaside, Explorer,
 * Ocean, Woodland, Celestial, Workshop, Candy, Winter. Append only. */
#define N_COLLECTION_SETS 12

/* Activity points per find: 1 per completed turn, 1 per 1000 fresh output tokens. */
#define FIND_POINTS 1000

/* Adult forms: form 0 is the original look; forms 1..15 are generated from the current life's DNA
 * and adult family. Ids are stable and never reroll within a life; every reached form stays
 * switchable and choosing one never consumes anything. A DNA rewrite starts a new set of forms. */
#define PET_FORMS 16

/* Each trait is 0..2. body round/square/pear; ears rounded/pointed/drooping; tail curl/fin/tuft;
 * crown antenna/horns/sprout; wings small/feather/fin; markings freckles/belly/stripes;
 * motion bounce/float/waddle. Only meaningful for forms 1..15 (form 0 is the existing art).
 * Trait indices 0..PET_TRAITS-1 (Pet.inherited_trait, pet_rewrite_preview) follow field order. */
#define PET_TRAITS 7
typedef struct {
    uint8_t body, ears, tail, crown, wings, markings, motion;
} PetTraits;

#define PET_LEDGERS 8

typedef struct {
    char ledger_id[16];
    int64_t fed[N_FAMILIES];
    int64_t turns, errors;
    int64_t last_seen;
    /* Its counts were known while finds existed, so growth past them is fresh activity. False for
     * ledgers carried over from saves made before finds (their first delta may be pre-upgrade work). */
    bool finds_armed;
    /* Its counts were known during the current life, so growth past them feeds this life. Cleared
     * by a DNA rewrite: each bridge's first snapshot afterwards is only a baseline. Generation 1
     * ignores it (all food, backlog included, feeds the original life). */
    bool life_armed;
} LedgerSeen;

typedef struct {
    char name[16];
    /* Lifetime totals, kept across DNA rewrites: cosmetic milestones, age records, stats. */
    int64_t born;
    int64_t xp; /* lifetime output tokens eaten */
    int64_t diet[N_FAMILIES];
    /* Current life (since the last DNA rewrite; the whole lifetime for generation 1): drives stage,
     * forms, favorite family and the adult family lock. */
    int64_t life_born;
    int64_t life_xp;
    int64_t life_diet[N_FAMILIES];
    uint32_t generation;     /* 1 = original life, +1 per DNA rewrite */
    int8_t inherited_trait;  /* trait index kept from the previous life's final form, -1 = none */
    uint8_t inherited_value; /* its value 0..2 (meaningful only with inherited_trait >= 0) */
    Stage stage;
    Family adult_family; /* locked in when this life becomes an adult */
    int64_t lifetime_turns;
    int64_t day;          /* bridge-local day number the today_* counters belong to */
    int64_t today_tokens; /* resets at local midnight; a stat, never a requirement */
    int64_t today_turns;
    int64_t best_day_tokens;
    int64_t cheer_until;  /* unix seconds: happy after a finish or a pat */
    int64_t eating_until; /* unix seconds; not persisted */
    LedgerSeen seen[PET_LEDGERS];
    /* Cosmetics: `unlocked` holds unlock ids (U_*), kept forever; unknown future ids are preserved.
     * `equipped[WearSlot]` is an unlock id or -1. Names, hints, milestones and sets live in pet.c;
     * art in cosmetics.h and appearance.c. */
    ItemSet unlocked;
    int16_t equipped[N_WEAR];
    /* Finds: fresh activity banks points (capped at what the unowned finds need); each FIND_POINTS
     * draws one unowned find, at most one per bridge-local day. */
    int64_t find_points;
    int64_t find_tokens; /* fresh output tokens not yet worth a point, 0..999 */
    int64_t find_day;    /* bridge-local day (as `day`) of the last drawn find; -1 = welcome find pending */
    uint64_t find_rng;   /* pet-specific draw sequence state */
    /* Evolution: `dna` is fixed for each life (at birth, re-drawn by a DNA rewrite; 0 = not stored
     * yet, see pet_dna()), `form` is the worn form 0..PET_FORMS-1 and `forms_seen` the highest
     * ready form already browsed (NEW indicator). */
    uint64_t dna;
    uint8_t form;
    uint8_t forms_seen;
} Pet;

typedef struct {
    int64_t tokens;
    int64_t turns;
    int64_t errors;
    bool hatched;
    bool evolved;
    ItemSet unlocked; /* ids newly earned by this feed */
} FeedResult;

/* A new original life: generation 1, life_born = born, no inherited trait. */
void pet_init(Pet *p, const char *name, int64_t now);
/* Lax key=value reader. Files from before DNA rewrites load as generation 1 with the current life
 * equal to the lifetime (life_xp = xp, life_diet = diet, life_born = born), so they keep their
 * exact DNA, forms and look. */
bool pet_load(Pet *p, const char *path);
/* Writes path.tmp, fsyncs it and renames it over path; the rename is the last fallible step.
 * False (path untouched) if the temporary path would not fit 768 bytes or any write fails. */
bool pet_save(const Pet *p, const char *path);

/* Eats token growth reported by one bridge (deltas against what this ledger reported before).
 * Lifetime totals always grow. Generation 1 credits all of it (backlog included) to the current
 * life too; later lives grow only from ledgers already known and life_armed before this feed, so
 * neither a new bridge's history nor the first snapshot after a rewrite feeds them. Every feed
 * arms its ledger for the current life. */
FeedResult pet_feed(Pet *p, const Feed *f, int64_t now, int32_t tz_offset);
void pet_pat(Pet *p, int64_t now);

bool pet_asleep(int64_t now, int32_t tz_offset);
Mood pet_mood(const Pet *p, int64_t now, int32_t tz_offset, int waiting, int running);
const char *stage_name(Stage s);
/* Current-life tokens needed to reach the next stage (0 for adults). */
int64_t stage_threshold(Stage s);
/* Family that has fed the current life the most so far; FAM_OTHER with no food this life. */
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
/* WEAR_BOT_* slot dressing the robots of a model family; unknown families use WEAR_BOT_OTHER. */
int pet_robot_slot(Family family);
bool pet_has(const Pet *p, int id);
/* Id worn in a slot, or -1. */
int pet_worn(const Pet *p, int slot);
/* Wears an owned id in its slot, or takes it off if it is already worn. False if not owned. */
bool pet_toggle_equip(Pet *p, int id);
/* Grants unlocks earned by date/age (seasonal, birthday, days alive), any stat milestone already
 * reached (so existing pets catch up), a find from banked points on a new day, and completed set
 * rewards. Milestones and ages use lifetime totals (xp, diet, born), never the current life.
 * Returns newly earned ids. Deterministic in (pet, now, tz). */
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

/* Evolution. Stored DNA, or the deterministic fallback from name and birth time; never depends on
 * the mutable find sequence. */
uint64_t pet_dna(const Pet *p);
/* Current-life tokens that unlock a form: 0 for the original (and invalid forms),
 * 2000000 + (form - 1) * form * 500000 for forms 1..15. */
int64_t pet_form_threshold(int form);
/* Forms ready to wear including the original: 1 before adulthood, then 1 + generated forms whose
 * threshold the current life's XP has reached (at most PET_FORMS). Ready forms are always 0..n-1. */
int pet_forms_available(const Pet *p);
/* Traits of generated form 1..15 (all zero for 0 or invalid). Form 1 comes from the DNA mixed with
 * the adult family (the current favorite, as a preview, before adulthood); each later form bumps
 * one trait by 1 mod 3, cycling through the seven fields, so all fifteen are distinct and
 * neighbours share six traits. With an inherited trait every form wears inherited_value for it and
 * the cycle runs over the other six fields only (still fifteen distinct forms, one change each). */
PetTraits pet_form_traits(const Pet *p, int form);
/* Wears a ready form. False (and no change) for an invalid, locked or already worn form. Never
 * touches equipment, XP or unlocks. */
bool pet_choose_form(Pet *p, int form);
/* Marks every currently ready form as seen. */
void pet_seen_forms(Pet *p);

/* DNA rewrite: an adult whose current life has every form (final form PET_FORMS-1) may start a new
 * life. True when that is the case and the generation can still advance. */
bool pet_rewrite_ready(const Pet *p);
/* The pet a rewrite would produce, written to *out (untouched on false); never modifies *p. False
 * unless pet_rewrite_ready(p), 0 <= trait < PET_TRAITS and now > 0 and now >= p->life_born. Keeps
 * name, lifetime totals, collection, equipment, finds, day counters and ledgers; starts life
 * generation + 1 at now as a Baby with no life XP/diet, adult family FAM_OTHER, original form,
 * nothing browsed, no cheer/eating, every ledger disarmed for the new life. Its DNA is fresh,
 * nonzero and different, derived only from the old DNA and generation (never trait, time or finds),
 * so cancelling and reopening shows the same candidate. inherited_trait = trait and inherited_value
 * = that trait's value in the OLD life's final form PET_FORMS-1 (not the worn form). Persisting it
 * (with the archived previous life) is pet_rewrite_commit() in rewrite.h. */
bool pet_rewrite_preview(const Pet *p, int trait, int64_t now, Pet *out);
