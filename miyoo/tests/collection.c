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
               "unlocked=8000000000200021\nequipped=0 5 -1 -1 -1 -1 21 127\n"
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

static void find_deck(void) {
    Pet p, restored;
    pet_init(&p, "Collector", NOW);
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
    for (int day = 0; day < 40; day++) {
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
    assert(finds(&p) == 32);
    for (int set = 0; set < N_COLLECTION_SETS; set++) {
        int have, total;
        pet_set_progress(&p, set, &have, &total);
        assert(have == total && total > 0);
    }
    for (int id = U_TEA_POT; id <= U_TREASURE_CHEST; id++) assert(pet_has(&p, id));
    puts("PASS duplicate-free deck, daily cap, restart determinism and all six set rewards");
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
    find_deck();
    fractional_activity();
    assert(unlink(path) == 0);
    return 0;
}
