#include "pet.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const int64_t NOW = 1792929600; /* fixed bridge time, not the device clock */
static char path[128];

static int finds(const Pet *p) {
    int n = 0;
    for (int i = 0; i < N_UNLOCKS; i++) n += pet_unlock_is_find(i) && pet_has(p, i);
    return n;
}

static void migration(void) {
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "name=Legacy\nborn=%lld\nxp=2500000\nstage=4\nadult_family=2\n"
               "unlocked=200021\nequipped=0 5 -1 -1 -1 -1 21 127\n"
               "seen=ledger 1000000 0 0 0 50 0 %lld\n", (long long)(NOW - 86400), (long long)NOW);
    assert(fclose(f) == 0);
    Pet p, q;
    assert(pet_load(&p, path));
    assert(p.xp == 2500000 && p.stage == STAGE_ADULT && p.adult_family == FAM_GEMINI);
    assert(pet_has(&p, U_SPROUT) && pet_has(&p, U_GLASSES) && pet_has(&p, U_SHINY));
    assert(pet_worn(&p, WEAR_HAT) == U_SPROUT && pet_worn(&p, WEAR_FACE) == U_GLASSES);
    assert(pet_worn(&p, WEAR_SHINY) == U_SHINY && p.equipped[7] == -1);
    /* Unknown future bits must survive a load/save, including beyond the old signed equipment range. */
    p.unlocked.words[2] |= UINT64_C(1) << 22;
    p.unlocked.words[3] |= UINT64_C(1) << 63;
    assert(itemset_has(p.unlocked, 150) && itemset_has(p.unlocked, 255));
    assert(!itemset_has(p.unlocked, -1) && !itemset_has(p.unlocked, 256));
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(memcmp(&p.unlocked, &q.unlocked, sizeof p.unlocked) == 0);
    assert(!pet_save_needed(&p, &q));
    Feed old_ledger = {0};
    strcpy(old_ledger.ledger_id, "ledger");
    old_ledger.fed[0] = 2000000;
    old_ledger.turns = 100;
    pet_feed(&q, &old_ledger, NOW, 0);
    assert(finds(&q) == 0 && pet_find_progress(&q) == 0);
    old_ledger.fed[0]++;
    pet_feed(&q, &old_ledger, NOW + 1, 0);
    assert(finds(&q) == 1);
    puts("PASS legacy ownership/equipment and 256-bit round trip");
}

static void batch_one_migration(void) {
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "name=Collector\nborn=%lld\nxp=2500000\nstage=4\nadult_family=2\n"
               "unlocked=fffffffffffffff 0 0 0\nequipped=25 30 38 46 59 53 21\n"
               "find_points=345\nfind_tokens=789\nfind_day=%lld\nfind_rng=123456789abcdef\n"
               "seen=ledger 2500000 0 0 0 200 0 %lld 1\n",
               (long long)NOW, (long long)(NOW / 86400), (long long)NOW);
    assert(fclose(f) == 0);
    Pet p, q;
    assert(pet_load(&p, path));
    assert(pet_worn(&p, WEAR_HAT) == U_MUSHROOM_HAT);
    assert(pet_worn(&p, WEAR_DESK) == U_TREASURE_CHEST);
    assert(p.find_points == 345 && p.find_tokens == 789 && p.find_day == NOW / 86400);
    assert(p.find_rng == UINT64_C(0x123456789abcdef));
    for (int slot = WEAR_WALL; slot < N_WEAR; slot++) assert(pet_worn(&p, slot) == -1);
    Feed fresh = {0};
    strcpy(fresh.ledger_id, "ledger");
    fresh.fed[0] = 2500211;
    fresh.turns = 201;
    pet_feed(&p, &fresh, NOW + 1, 0);
    assert(p.find_points == 347 && p.find_tokens == 0 && finds(&p) == 32);
    assert(p.find_rng == UINT64_C(0x123456789abcdef)); /* upgrade never draws another welcome */

    /* Family outfits are independent, and items across the word boundary equip and persist. */
    const int ids[] = {U_OCEAN_WALL, U_AURORA_VIEW, U_LAGOON_COAT,
                      U_DIVER_BOT, U_PENGUIN_BOT, U_SNOW_BOT, U_OWL_BOT};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        int id = ids[i];
        p.unlocked.words[id / 64] |= UINT64_C(1) << (id % 64);
        assert(pet_toggle_equip(&p, id));
        assert(pet_worn(&p, pet_unlock_slot(id)) == id);
    }
    assert(pet_worn(&p, pet_robot_slot(FAM_CLAUDE)) == U_DIVER_BOT);
    assert(pet_worn(&p, pet_robot_slot(FAM_GPT)) == U_PENGUIN_BOT);
    assert(pet_worn(&p, pet_robot_slot(FAM_GEMINI)) == U_SNOW_BOT);
    assert(pet_worn(&p, pet_robot_slot(FAM_OTHER)) == U_OWL_BOT);
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(!pet_save_needed(&p, &q));
    assert(pet_toggle_equip(&q, U_DIVER_BOT));
    assert(pet_worn(&q, pet_robot_slot(FAM_CLAUDE)) == -1);
    assert(pet_worn(&q, pet_robot_slot(FAM_GPT)) == U_PENGUIN_BOT);
    assert(!pet_toggle_equip(&q, U_SODA_BOT)); /* locked outfits cannot be worn */

    /* Legacy eight-field saves had an unused last field, not an equipped wallpaper. */
    f = fopen(path, "w");
    assert(f);
    fprintf(f, "unlocked=0 1 0 0\nequipped=-1 -1 -1 -1 -1 -1 -1 64\n");
    assert(fclose(f) == 0 && pet_load(&q, path));
    assert(pet_has(&q, U_OCEAN_WALL) && pet_worn(&q, WEAR_WALL) == -1);
    puts("PASS batch-one progress migration, independent robot outfits and expanded equipment saves");
}

static void find_deck(void) {
    Pet p, restored;
    pet_init(&p, "Collector", NOW);
    int expected = pet_finds_left(&p);
    Feed f = {0};
    strcpy(f.ledger_id, "ledger");
    f.fed[0] = 50000000;
    f.turns = 10000;
    pet_feed(&p, &f, NOW, 0);
    assert(finds(&p) == 0 && pet_find_progress(&p) == 0); /* no first-snapshot jackpot */
    f.turns++;
    pet_feed(&p, &f, NOW + 1, 0);
    assert(finds(&p) == 1); /* one welcome present */
    assert(pet_save(&p, path) && pet_load(&restored, path));
    for (int day = 0; day < expected + 3; day++) {
        int64_t now = NOW + day * 86400 + 2;
        int before = finds(&p);
        f.fed[0] += 1000000;
        f.turns++;
        pet_feed(&p, &f, now, 0);
        pet_feed(&restored, &f, now, 0);
        pet_check_unlocks(&p, now, 0);
        pet_check_unlocks(&restored, now, 0);
        int after = finds(&p);
        assert(after >= before && after <= before + 1);
        assert(memcmp(&p.unlocked, &restored.unlocked, sizeof p.unlocked) == 0);
        /* Same-day extra work, unchanged feed, and restart cannot reroll or bypass the day gate. */
        f.turns += 3000;
        pet_feed(&p, &f, now + 10, 0);
        pet_feed(&restored, &f, now + 10, 0);
        pet_check_unlocks(&p, now + 20, 0);
        assert(finds(&p) == after);
        assert(pet_save(&p, path) && pet_load(&restored, path));
        assert(!pet_save_needed(&p, &restored));
    }
    assert(finds(&p) == expected && pet_finds_left(&p) == 0);
    for (int set = 0; set < N_COLLECTION_SETS; set++) {
        int have, total;
        pet_set_progress(&p, set, &have, &total);
        assert(have == total && total > 0);
        assert(pet_has(&p, pet_set_reward(set)));
    }
    for (int slot = WEAR_WALL; slot < N_WEAR; slot++) assert(pet_worn(&p, slot) == -1);
    assert(pet_worn(&p, WEAR_SHINY) == -1);
    puts("PASS full duplicate-free deck, daily cap, restart determinism, set rewards and opt-in appearances");
}

static void fractional_activity(void) {
    Pet p;
    pet_init(&p, "Fraction", NOW);
    Feed f = {0};
    strcpy(f.ledger_id, "fraction");
    pet_feed(&p, &f, NOW, 0);
    f.turns = 1;
    pet_feed(&p, &f, NOW + 1, 0); /* welcome find */
    int64_t before = pet_find_progress(&p);
    f.fed[0] = 499;
    pet_feed(&p, &f, NOW + 2, 0);
    assert(pet_find_progress(&p) == before);
    assert(pet_save(&p, path));
    Pet q;
    assert(pet_load(&q, path));
    f.fed[0] = 1000;
    pet_feed(&q, &f, NOW + 3, 0);
    assert(pet_find_progress(&q) == before + 1);
    Feed second = {0};
    strcpy(second.ledger_id, "second");
    second.fed[0] = 100000000;
    second.turns = 10000;
    pet_feed(&q, &second, NOW + 4, 0);
    assert(pet_find_progress(&q) == before + 1 && finds(&q) == 1);
    /* A backwards clock cannot grant a second item from the same activity. */
    pet_check_unlocks(&q, NOW - 86400, 0);
    assert(finds(&q) == 1);
    f.turns += 1000;
    pet_feed(&q, &f, NOW + 5, 0);
    assert(finds(&q) == 1 && pet_find_waiting(&q, NOW + 5, 0));
    pet_check_unlocks(&q, NOW + 86400, 0);
    assert(finds(&q) == 2); /* banked work delivers tomorrow, even without another feed */
    pet_check_unlocks(&q, NOW, 0);
    assert(finds(&q) == 2);
    puts("PASS fractional activity across save/reload and new-bridge backlog exclusion");
}

int main(void) {
    snprintf(path, sizeof path, "/tmp/paseo-collection-test-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    migration();
    batch_one_migration();
    find_deck();
    fractional_activity();
    assert(unlink(path) == 0);
    return 0;
}
