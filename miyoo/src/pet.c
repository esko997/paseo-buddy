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
_Static_assert(N_UNLOCKS == 120, "every id needs a catalog entry below");

static void itemset_add(ItemSet *s, int id) {
    if (id >= 0 && id < ITEMSET_BITS) s->words[id >> 6] |= (uint64_t)1 << (id & 63);
}

enum {
    SET_COZY,
    SET_GARDEN,
    SET_SPACE,
    SET_ARCADE,
    SET_SEASIDE,
    SET_EXPLORER,
    SET_OCEAN,
    SET_WOODLAND,
    SET_CELESTIAL,
    SET_WORKSHOP,
    SET_CANDY,
    SET_WINTER,
    N_SETS
};
_Static_assert(N_SETS == N_COLLECTION_SETS, "every set needs a name and reward below");
#define COZY "Cozy"
#define GARDEN "Garden"
#define SPACE "Space"
#define ARCADE "Arcade"
#define SEASIDE "Seaside"
#define EXPLORER "Explorer"
#define OCEAN "Ocean"
#define WOODLAND "Woodland"
#define CELESTIAL "Celestial"
#define WORKSHOP "Workshop"
#define CANDY "Candy"
#define WINTER "Winter"

typedef struct {
    const char *name, *hint;
    WearSlot slot;
    int8_t set; /* collection set (base item or reward), -1 for none */
    bool find;  /* drawn from activity; the base items of the sets */
} UnlockInfo;

#define MILESTONE(n, h, sl) {n, h, sl, -1, false}
#define FIND(n, sl, set, setname) {n, "Found while you work; " setname " set", sl, set, true}
#define REWARD(n, sl, set, setname) {n, "Complete the " setname " set", sl, set, false}

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

    [U_TEA_POT] = REWARD("Teapot", WEAR_DESK, SET_COZY, COZY),
    [U_SUNFLOWER] = REWARD("Sunflower", WEAR_DESK, SET_GARDEN, GARDEN),
    [U_PLANET_MOBILE] = REWARD("Planet mobile", WEAR_DESK, SET_SPACE, SPACE),
    [U_ARCADE_CABINET] = REWARD("Arcade cabinet", WEAR_DESK, SET_ARCADE, ARCADE),
    [U_LIGHTHOUSE] = REWARD("Lighthouse", WEAR_DESK, SET_SEASIDE, SEASIDE),
    [U_TREASURE_CHEST] = REWARD("Treasure chest", WEAR_DESK, SET_EXPLORER, EXPLORER),

    [U_DIVER_HAT] = FIND("Diver hood", WEAR_HAT, SET_OCEAN, OCEAN),
    [U_TRIDENT] = FIND("Trident", WEAR_HELD, SET_OCEAN, OCEAN),
    [U_CORAL] = FIND("Coral garden", WEAR_DESK, SET_OCEAN, OCEAN),
    [U_WAVE_RUG] = FIND("Wave rug", WEAR_ROOM, SET_OCEAN, OCEAN),
    [U_OCEAN_WALL] = FIND("Ocean tiles", WEAR_WALL, SET_OCEAN, OCEAN),
    [U_REEF_VIEW] = FIND("Coral reef", WEAR_WINDOW, SET_OCEAN, OCEAN),
    [U_DIVER_BOT] = FIND("Diver bot", WEAR_BOT_CLAUDE, SET_OCEAN, OCEAN),
    [U_SUB_BOT] = FIND("Submarine bot", WEAR_BOT_GPT, SET_OCEAN, OCEAN),
    [U_LAGOON_COAT] = FIND("Lagoon coat", WEAR_SHINY, SET_OCEAN, OCEAN),
    [U_PEARL_NECKLACE] = REWARD("Pearl necklace", WEAR_NECK, SET_OCEAN, OCEAN),

    [U_ACORN_HAT] = FIND("Acorn cap", WEAR_HAT, SET_WOODLAND, WOODLAND),
    [U_PINECONE] = FIND("Pinecone", WEAR_HELD, SET_WOODLAND, WOODLAND),
    [U_BIRDHOUSE] = FIND("Birdhouse", WEAR_DESK, SET_WOODLAND, WOODLAND),
    [U_FERN_RUG] = FIND("Fern rug", WEAR_ROOM, SET_WOODLAND, WOODLAND),
    [U_FOREST_WALL] = FIND("Fern wallpaper", WEAR_WALL, SET_WOODLAND, WOODLAND),
    [U_FOREST_VIEW] = FIND("Forest trail", WEAR_WINDOW, SET_WOODLAND, WOODLAND),
    [U_RANGER_BOT] = FIND("Ranger bot", WEAR_BOT_GEMINI, SET_WOODLAND, WOODLAND),
    [U_OWL_BOT] = FIND("Owl bot", WEAR_BOT_OTHER, SET_WOODLAND, WOODLAND),
    [U_MOSS_COAT] = FIND("Moss coat", WEAR_SHINY, SET_WOODLAND, WOODLAND),
    [U_ACORN_CHARM] = REWARD("Acorn charm", WEAR_NECK, SET_WOODLAND, WOODLAND),

    [U_COMET_HAT] = FIND("Comet crown", WEAR_HAT, SET_CELESTIAL, CELESTIAL),
    [U_TELESCOPE] = FIND("Telescope", WEAR_HELD, SET_CELESTIAL, CELESTIAL),
    [U_ORRERY] = FIND("Orrery", WEAR_DESK, SET_CELESTIAL, CELESTIAL),
    [U_ORBIT_RUG] = FIND("Orbit rug", WEAR_ROOM, SET_CELESTIAL, CELESTIAL),
    [U_STAR_WALL] = FIND("Star wallpaper", WEAR_WALL, SET_CELESTIAL, CELESTIAL),
    [U_NEBULA_VIEW] = FIND("Nebula", WEAR_WINDOW, SET_CELESTIAL, CELESTIAL),
    [U_ASTRONAUT_BOT] = FIND("Astronaut bot", WEAR_BOT_CLAUDE, SET_CELESTIAL, CELESTIAL),
    [U_SATELLITE_BOT] = FIND("Satellite bot", WEAR_BOT_GEMINI, SET_CELESTIAL, CELESTIAL),
    [U_NEBULA_COAT] = FIND("Nebula coat", WEAR_SHINY, SET_CELESTIAL, CELESTIAL),
    [U_CONSTELLATION] = REWARD("Star collar", WEAR_NECK, SET_CELESTIAL, CELESTIAL),

    [U_HARD_HAT] = FIND("Hard hat", WEAR_HAT, SET_WORKSHOP, WORKSHOP),
    [U_WRENCH] = FIND("Wrench", WEAR_HELD, SET_WORKSHOP, WORKSHOP),
    [U_TOOLBOX] = FIND("Toolbox", WEAR_DESK, SET_WORKSHOP, WORKSHOP),
    [U_GEAR_RUG] = FIND("Gear rug", WEAR_ROOM, SET_WORKSHOP, WORKSHOP),
    [U_BLUEPRINT_WALL] = FIND("Blueprint wall", WEAR_WALL, SET_WORKSHOP, WORKSHOP),
    [U_CITY_VIEW] = FIND("City skyline", WEAR_WINDOW, SET_WORKSHOP, WORKSHOP),
    [U_MECHANIC_BOT] = FIND("Mechanic bot", WEAR_BOT_GPT, SET_WORKSHOP, WORKSHOP),
    [U_CLOCKWORK_BOT] = FIND("Clockwork bot", WEAR_BOT_OTHER, SET_WORKSHOP, WORKSHOP),
    [U_COPPER_COAT] = FIND("Copper coat", WEAR_SHINY, SET_WORKSHOP, WORKSHOP),
    [U_GEAR_PENDANT] = REWARD("Gear pendant", WEAR_NECK, SET_WORKSHOP, WORKSHOP),

    [U_CUPCAKE_HAT] = FIND("Cupcake hat", WEAR_HAT, SET_CANDY, CANDY),
    [U_ICE_CREAM] = FIND("Ice cream", WEAR_HELD, SET_CANDY, CANDY),
    [U_GUMBALL] = FIND("Gumball jar", WEAR_DESK, SET_CANDY, CANDY),
    [U_CANDY_RUG] = FIND("Candy rug", WEAR_ROOM, SET_CANDY, CANDY),
    [U_CANDY_WALL] = FIND("Candy stripes", WEAR_WALL, SET_CANDY, CANDY),
    [U_CANDY_VIEW] = FIND("Candy hills", WEAR_WINDOW, SET_CANDY, CANDY),
    [U_PATISSIER_BOT] = FIND("Pastry chef", WEAR_BOT_CLAUDE, SET_CANDY, CANDY),
    [U_SODA_BOT] = FIND("Soda bot", WEAR_BOT_OTHER, SET_CANDY, CANDY),
    [U_SORBET_COAT] = FIND("Sorbet coat", WEAR_SHINY, SET_CANDY, CANDY),
    [U_CANDY_BOW] = REWARD("Candy bow", WEAR_NECK, SET_CANDY, CANDY),

    [U_SNOW_HAT] = FIND("Snow cap", WEAR_HAT, SET_WINTER, WINTER),
    [U_SNOWGLOBE] = FIND("Snow globe", WEAR_HELD, SET_WINTER, WINTER),
    [U_IGLOO] = FIND("Igloo", WEAR_DESK, SET_WINTER, WINTER),
    [U_SNOW_RUG] = FIND("Snowflake rug", WEAR_ROOM, SET_WINTER, WINTER),
    [U_FROST_WALL] = FIND("Frost wallpaper", WEAR_WALL, SET_WINTER, WINTER),
    [U_AURORA_VIEW] = FIND("Aurora", WEAR_WINDOW, SET_WINTER, WINTER),
    [U_PENGUIN_BOT] = FIND("Penguin bot", WEAR_BOT_GPT, SET_WINTER, WINTER),
    [U_SNOW_BOT] = FIND("Snow bot", WEAR_BOT_GEMINI, SET_WINTER, WINTER),
    [U_FROST_COAT] = FIND("Frost coat", WEAR_SHINY, SET_WINTER, WINTER),
    [U_SNOW_SCARF] = REWARD("Snowflake scarf", WEAR_NECK, SET_WINTER, WINTER),
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
    [SET_OCEAN] = {OCEAN, U_PEARL_NECKLACE},
    [SET_WOODLAND] = {WOODLAND, U_ACORN_CHARM},
    [SET_CELESTIAL] = {CELESTIAL, U_CONSTELLATION},
    [SET_WORKSHOP] = {WORKSHOP, U_GEAR_PENDANT},
    [SET_CANDY] = {CANDY, U_CANDY_BOW},
    [SET_WINTER] = {WINTER, U_SNOW_SCARF},
};

/* Slot 6 keeps its WEAR_SHINY id but now holds every coat, so it shows as "Coat". */
static const char *SLOT_NAMES[N_WEAR] = {"Hat",  "Face",   "Neck",       "Held",    "Desk",       "Room",     "Coat",
                                         "Wall", "Window", "Claude bot", "GPT bot", "Gemini bot", "Other bot"};

const char *pet_unlock_name(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].name : "?"; }
const char *pet_unlock_hint(int id) { return id >= 0 && id < N_UNLOCKS ? UNLOCKS[id].hint : ""; }
int pet_unlock_slot(int id) { return id >= 0 && id < N_UNLOCKS ? (int)UNLOCKS[id].slot : -1; }
const char *pet_slot_name(int slot) { return slot >= 0 && slot < N_WEAR ? SLOT_NAMES[slot] : "?"; }
bool pet_has(const Pet *p, int id) { return id >= 0 && id < N_UNLOCKS && itemset_has(p->unlocked, id); }

int pet_robot_slot(Family family) {
    switch (family) {
    case FAM_CLAUDE: return WEAR_BOT_CLAUDE;
    case FAM_GPT: return WEAR_BOT_GPT;
    case FAM_GEMINI: return WEAR_BOT_GEMINI;
    default: return WEAR_BOT_OTHER;
    }
}

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

/* New items wear themselves into an empty slot only for the pet's own wearables and den decor
 * (HAT..ROOM). Coats, wallpaper, windows and robot costumes change how the whole screen looks, so
 * they stay opt-in. */
static bool auto_wears(int slot) { return slot >= WEAR_HAT && slot < WEAR_SHINY; }

/* Adds ids to the collection plus the reward of every set that is now complete; a new item is worn
 * right away if auto_wears() its slot and the slot is empty. Returns the ids that were new. */
static ItemSet award(Pet *p, ItemSet ids) {
    ItemSet fresh;
    for (int w = 0; w < ITEMSET_BITS / 64; w++) {
        fresh.words[w] = ids.words[w] & ~p->unlocked.words[w];
        p->unlocked.words[w] |= fresh.words[w];
    }
    for (int set = 0; set < N_COLLECTION_SETS; set++) {
        int have, total, reward = SETS[set].reward;
        pet_set_progress(p, set, &have, &total);
        if (total > 0 && have == total && !pet_has(p, reward)) {
            itemset_add(&fresh, reward);
            itemset_add(&p->unlocked, reward);
        }
    }
    for (int id = 0; id < N_UNLOCKS; id++) {
        int slot = UNLOCKS[id].slot;
        if (itemset_has(fresh, id) && auto_wears(slot) && pet_worn(p, slot) < 0) p->equipped[slot] = (int16_t)id;
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

/* ---- evolution: generated adult forms, unlocked by current-life XP and switched freely -------- */

#define FIRST_FORM_XP 2000000 /* stage_threshold(STAGE_TEEN): form 1 is ready on reaching adulthood */
#define FORM_STEP_XP 500000
#define N_TRAITS PET_TRAITS

/* Fixed at birth from name and birth time; its own hash, so it never follows the find sequence. */
static uint64_t dna_seed(const Pet *p) {
    uint64_t h = UINT64_C(0x6A09E667F3BCC908);
    for (const char *c = p->name; *c; c++) h = (h ^ (uint8_t)*c) * UINT64_C(0x100000001B3);
    h ^= (uint64_t)p->born * UINT64_C(0xD6E8FEB86659FD93);
    return splitmix64(&h);
}

uint64_t pet_dna(const Pet *p) { return p->dna ? p->dna : dna_seed(p); }

int64_t pet_form_threshold(int form) {
    if (form < 1 || form >= PET_FORMS) return 0;
    return FIRST_FORM_XP + (int64_t)(form - 1) * form * FORM_STEP_XP;
}

int pet_forms_available(const Pet *p) {
    if (p->stage < STAGE_ADULT) return 1;
    int n = 1;
    while (n < PET_FORMS && p->life_xp >= pet_form_threshold(n)) n++;
    return n;
}

/* The trait every form of this life keeps from the previous life's final form, or -1. */
static int kept_trait(const Pet *p) {
    return p->inherited_trait >= 0 && p->inherited_trait < N_TRAITS && p->inherited_value < 3 ? p->inherited_trait : -1;
}

PetTraits pet_form_traits(const Pet *p, int form) {
    PetTraits t = {0};
    if (form < 1 || form >= PET_FORMS) return t;
    Family family = p->stage == STAGE_ADULT ? p->adult_family : pet_favorite(p);
    uint64_t h = pet_dna(p) ^ (UINT64_C(0x9E3779B97F4A7C15) * (uint64_t)(family + 1));
    uint64_t x = splitmix64(&h);
    uint8_t v[N_TRAITS];
    for (int i = 0; i < N_TRAITS; i++, x /= 3) v[i] = (uint8_t)(x % 3);
    /* Fields that change from form to form: all seven, or the six besides an inherited trait.
     * Without one this is exactly the original sequence (order is the identity). */
    int kept = kept_trait(p), order[N_TRAITS], n = 0;
    for (int i = 0; i < N_TRAITS; i++)
        if (i != kept) order[n++] = i;
    if (kept >= 0) v[kept] = p->inherited_value;
    int first = (int)(x % (uint64_t)n); /* which field changes first, then cyclically */
    for (int k = 2; k <= form; k++) {
        int i = order[(first + k - 2) % n];
        v[i] = (uint8_t)((v[i] + 1) % 3);
    }
    t.body = v[0], t.ears = v[1], t.tail = v[2], t.crown = v[3];
    t.wings = v[4], t.markings = v[5], t.motion = v[6];
    return t;
}

static uint8_t trait_value(PetTraits t, int trait) {
    const uint8_t v[N_TRAITS] = {t.body, t.ears, t.tail, t.crown, t.wings, t.markings, t.motion};
    return v[trait];
}

bool pet_choose_form(Pet *p, int form) {
    if (form < 0 || form >= pet_forms_available(p) || form == p->form) return false;
    p->form = (uint8_t)form;
    return true;
}

void pet_seen_forms(Pet *p) {
    int top = pet_forms_available(p) - 1;
    if (top > p->forms_seen) p->forms_seen = (uint8_t)top;
}

bool pet_rewrite_ready(const Pet *p) {
    return p->generation >= 1 && p->generation < UINT32_MAX && pet_forms_available(p) == PET_FORMS;
}

/* The next life's DNA: from the old DNA and the new generation only, never 0 nor the old DNA. */
static uint64_t rewrite_dna(const Pet *p) {
    uint64_t old = pet_dna(p), dna;
    uint64_t h = old ^ (UINT64_C(0xBB67AE8584CAA73B) * ((uint64_t)p->generation + 1));
    do dna = splitmix64(&h);
    while (!dna || dna == old);
    return dna;
}

bool pet_rewrite_preview(const Pet *p, int trait, int64_t now, Pet *out) {
    if (!pet_rewrite_ready(p) || trait < 0 || trait >= N_TRAITS || now <= 0 || now < p->life_born) return false;
    Pet n = *p; /* lifetime totals, collection, equipment, finds, day counters and ledgers */
    n.generation = p->generation + 1;
    n.life_born = now;
    n.life_xp = 0;
    memset(n.life_diet, 0, sizeof n.life_diet);
    n.stage = STAGE_BABY;
    n.adult_family = FAM_OTHER;
    n.form = 0;
    n.forms_seen = 0;
    n.cheer_until = 0;
    n.eating_until = 0;
    for (int i = 0; i < PET_LEDGERS; i++) n.seen[i].life_armed = false;
    n.dna = rewrite_dna(p);
    n.inherited_trait = (int8_t)trait;
    n.inherited_value = trait_value(pet_form_traits(p, PET_FORMS - 1), trait);
    *out = n;
    return true;
}

void pet_init(Pet *p, const char *name, int64_t now) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->born = p->life_born = now;
    p->generation = 1;
    p->inherited_trait = -1;
    p->stage = STAGE_EGG;
    p->adult_family = FAM_OTHER;
    memset(p->equipped, -1, sizeof p->equipped);
    p->find_day = -1;
    p->find_rng = find_seed(p);
    p->dna = dna_seed(p);
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
        if (p->life_diet[i] > most) {
            most = p->life_diet[i];
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
    /* The original life eats everything; later lives only growth past counts known this life. */
    bool life = p->generation <= 1 || (!backlog && s->life_armed);
    s->finds_armed = true;
    s->life_armed = true;
    s->last_seen = now;
    int64_t life_tokens = 0;
    for (int i = 0; i < N_FAMILIES; i++) {
        int64_t d = f->fed[i] - s->fed[i];
        if (d < 0) d = 0; /* ledger shrank: just rebaseline */
        p->diet[i] += d;
        r.tokens += d;
        if (life) p->life_diet[i] += d, life_tokens += d;
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
    p->life_xp += life_tokens;
    if (!backlog) {
        p->today_tokens += r.tokens;
        p->today_turns += r.turns;
        if (p->today_tokens > p->best_day_tokens) p->best_day_tokens = p->today_tokens;
    }
    p->lifetime_turns += r.turns;
    if (life_tokens > 0) p->eating_until = now + EAT_ANIM_S;
    if (r.turns > 0) p->cheer_until = now + CHEER_S;
    while (p->stage < STAGE_ADULT && p->life_xp >= THRESHOLDS[p->stage]) p->stage++;
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
        now->find_tokens != saved->find_tokens || now->find_day != saved->find_day || now->find_rng != saved->find_rng ||
        now->dna != saved->dna || now->form != saved->form || now->forms_seen != saved->forms_seen ||
        now->life_xp != saved->life_xp || now->life_born != saved->life_born ||
        memcmp(now->life_diet, saved->life_diet, sizeof now->life_diet) != 0 || now->generation != saved->generation ||
        now->inherited_trait != saved->inherited_trait || now->inherited_value != saved->inherited_value)
        return true;
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *a = &now->seen[i], *b = &saved->seen[i];
        if (strcmp(a->ledger_id, b->ledger_id) || memcmp(a->fed, b->fed, sizeof a->fed) || a->turns != b->turns ||
            a->errors != b->errors || a->finds_armed != b->finds_armed || a->life_armed != b->life_armed)
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

/* `equipped=` value count written by builds from before the collection (7 slots + 1 padding). */
#define LEGACY_EQUIPPED 8
_Static_assert(LEGACY_EQUIPPED != N_WEAR && WEAR_SHINY + 1 < LEGACY_EQUIPPED, "legacy layout must stay detectable");

bool pet_save(const Pet *p, const char *path) {
    char tmp[768];
    int len = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (len < 0 || (size_t)len >= sizeof tmp) return false; /* never write beside a truncated name */
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
    fprintf(f, "dna=%llx\nform=%d\nforms_seen=%d\n", (unsigned long long)p->dna, p->form, p->forms_seen);
    fprintf(f, "generation=%lu\nlife_born=%lld\nlife_xp=%lld\n", (unsigned long)p->generation, (long long)p->life_born,
            (long long)p->life_xp);
    fprintf(f, "life_diet=%lld %lld %lld %lld\n", (long long)p->life_diet[0], (long long)p->life_diet[1],
            (long long)p->life_diet[2], (long long)p->life_diet[3]);
    fprintf(f, "inherited_trait=%d\ninherited_value=%d\n", p->inherited_trait, p->inherited_value);
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *s = &p->seen[i];
        if (!s->ledger_id[0]) continue;
        fprintf(f, "seen=%s %lld %lld %lld %lld %lld %lld %lld %d %d\n", s->ledger_id, (long long)s->fed[0],
                (long long)s->fed[1], (long long)s->fed[2], (long long)s->fed[3], (long long)s->turns,
                (long long)s->errors, (long long)s->last_seen, s->finds_armed, s->life_armed);
    }
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
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
    int form = 0, forms_seen = 0; /* files from before forms: the original look, nothing browsed */
    /* Files from before DNA rewrites: the original life, which is the whole lifetime. */
    bool have_life_born = false, have_life_xp = false, have_life_diet = false;
    unsigned long long generation = 1;
    int inherited_trait = -1, inherited_value = 0;
    char line[256];
    int n_seen = 0;
    long long a, b, c, d, e, g, h;
    int armed, life_armed;
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
        else if (!strcmp(k, "dna")) p->dna = strtoull(v, NULL, 16);
        else if (!strcmp(k, "form")) form = atoi(v);
        else if (!strcmp(k, "forms_seen")) forms_seen = atoi(v);
        else if (!strcmp(k, "generation")) generation = strtoull(v, NULL, 10);
        else if (!strcmp(k, "life_born")) p->life_born = atoll(v), have_life_born = true;
        else if (!strcmp(k, "life_xp")) p->life_xp = atoll(v), have_life_xp = true;
        else if (!strcmp(k, "inherited_trait")) inherited_trait = atoi(v);
        else if (!strcmp(k, "inherited_value")) inherited_value = atoi(v);
        else if (!strcmp(k, "life_diet") && sscanf(v, "%lld %lld %lld %lld", &a, &b, &c, &d) == 4) {
            p->life_diet[0] = a, p->life_diet[1] = b, p->life_diet[2] = c, p->life_diet[3] = d;
            have_life_diet = true;
        } else if (!strcmp(k, "unlocked")) { /* one hex word per 64 ids, lowest first; older files have one */
            char *end;
            for (int w = 0; w < ITEMSET_BITS / 64; w++, v = end) {
                unsigned long long bits = strtoull(v, &end, 16);
                if (end == v) break;
                p->unlocked.words[w] = bits;
            }
        } else if (!strcmp(k, "equipped")) {
            /* One value per slot. Files from before the collection wrote eight: slots 0..6 plus an
             * unused padding value that must not become slot 7 (the wallpaper). The first collection
             * wrote seven; now N_WEAR. Values past N_WEAR (a newer build) are ignored. */
            int16_t vals[N_WEAR];
            int count = 0, w, n;
            for (; sscanf(v, "%d%n", &w, &n) == 1; v += n, count++)
                if (count < N_WEAR) vals[count] = (int16_t)(w >= 0 && w < ITEMSET_BITS ? w : -1);
            int use = count == LEGACY_EQUIPPED ? WEAR_SHINY + 1 : count < N_WEAR ? count : N_WEAR;
            memcpy(p->equipped, vals, (size_t)use * sizeof *vals);
        } else if (!strcmp(k, "diet") && sscanf(v, "%lld %lld %lld %lld", &a, &b, &c, &d) == 4) {
            p->diet[0] = a, p->diet[1] = b, p->diet[2] = c, p->diet[3] = d;
        } else if (!strcmp(k, "seen") && n_seen < PET_LEDGERS) {
            int got = sscanf(v, "%15s %lld %lld %lld %lld %lld %lld %lld %d %d", id, &a, &b, &c, &d, &e, &g, &h, &armed,
                             &life_armed);
            if (got < 8) continue;
            LedgerSeen *s = &p->seen[n_seen++];
            snprintf(s->ledger_id, sizeof s->ledger_id, "%s", id);
            s->fed[0] = a, s->fed[1] = b, s->fed[2] = c, s->fed[3] = d;
            s->turns = e, s->errors = g, s->last_seen = h;
            s->finds_armed = got >= 9 && armed; /* older files: counts may include pre-upgrade work */
            s->life_armed = got == 10 && life_armed; /* older files: the original life ignores it */
        }
    }
    fclose(f);
    if (!have_life_born) p->life_born = p->born;
    if (!have_life_xp) p->life_xp = p->xp;
    if (!have_life_diet) memcpy(p->life_diet, p->diet, sizeof p->life_diet);
    p->generation = generation < 1 ? 1 : generation > UINT32_MAX ? UINT32_MAX : (uint32_t)generation;
    /* The original life inherits nothing; a bad record falls back to the plain form sequence. */
    bool inherits = p->generation > 1 && inherited_trait >= 0 && inherited_trait < N_TRAITS && inherited_value >= 0 &&
                    inherited_value < 3;
    p->inherited_trait = (int8_t)(inherits ? inherited_trait : -1);
    p->inherited_value = (uint8_t)(inherits ? inherited_value : 0);
    for (int s = 0; s < N_WEAR; s++) /* only owned items, in their own slot */
        if (pet_worn(p, s) < 0) p->equipped[s] = -1;
    if (!have_rng) p->find_rng = find_seed(p); /* first load since finds: no backlog, fresh sequence */
    if (p->find_points < 0) p->find_points = 0;
    if (p->find_tokens < 0 || p->find_tokens >= 1000) p->find_tokens = 0;
    if (p->find_day < -1) p->find_day = -1;
    cap_points(p);
    if (!p->dna) p->dna = dna_seed(p); /* first load since forms: the DNA it was born with */
    int ready = pet_forms_available(p); /* only reached forms; anything else falls back to the original */
    p->form = (uint8_t)(form > 0 && form < ready ? form : 0);
    p->forms_seen = (uint8_t)(forms_seen > 0 ? (forms_seen < ready ? forms_seen : ready - 1) : 0);
    return true;
}
