#include "pet.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const int64_t NOW = 1792929600; /* fixed bridge time, not the device clock */
static char path[128];

static void traits_vec(PetTraits t, uint8_t v[7]) {
    v[0] = t.body, v[1] = t.ears, v[2] = t.tail, v[3] = t.crown;
    v[4] = t.wings, v[5] = t.markings, v[6] = t.motion;
}

static bool same_traits(PetTraits a, PetTraits b) {
    uint8_t x[7], y[7];
    traits_vec(a, x);
    traits_vec(b, y);
    return memcmp(x, y, sizeof x) == 0;
}

static bool same_forms(const Pet *a, const Pet *b) {
    for (int form = 0; form < PET_FORMS; form++)
        if (!same_traits(pet_form_traits(a, form), pet_form_traits(b, form))) return false;
    return true;
}

/* A one-off backlog meal from its own ledger: adds XP and diet, never draws a find. */
static void eat(Pet *p, const char *ledger, Family family, int64_t tokens, int64_t now) {
    Feed f = {0};
    snprintf(f.ledger_id, sizeof f.ledger_id, "%s", ledger);
    f.fed[family] = tokens;
    pet_feed(p, &f, now, 0);
}

static void adult(Pet *p, const char *name, Family family, int64_t xp) {
    pet_init(p, name, NOW);
    eat(p, "grow", family, xp, NOW);
    assert(p->stage == STAGE_ADULT && p->adult_family == family && p->xp == xp && p->life_xp == xp);
}

static void write_file(const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    assert(fclose(f) == 0);
}

static void old_save_migration(void) {
    char text[512];
    snprintf(text, sizeof text,
             "name=Legacy\nborn=%lld\nxp=5000000\nstage=4\nadult_family=1\n"
             "unlocked=200021\nequipped=0 5 -1 -1 -1 -1 21\n"
             "find_points=0\nfind_tokens=0\nfind_day=-1\nfind_rng=abc\n"
             "seen=ledger 0 5000000 0 0 50 0 %lld 1\n",
             (long long)(NOW - 86400), (long long)NOW);
    write_file(text);
    Pet p, q, born;
    assert(pet_load(&p, path));
    pet_init(&born, "Legacy", NOW - 86400);
    /* Missing fields: the original look, nothing browsed, and the DNA it was born with. */
    assert(p.form == 0 && p.forms_seen == 0);
    assert(p.dna != 0 && p.dna == born.dna && pet_dna(&p) == p.dna);
    assert(pet_forms_available(&p) == 4);
    /* Old progress and outfits are untouched; the original life is the whole lifetime. */
    assert(p.xp == 5000000 && p.stage == STAGE_ADULT && p.adult_family == FAM_GPT);
    assert(p.generation == 1 && p.inherited_trait == -1 && p.life_xp == p.xp && p.life_born == p.born);
    assert(memcmp(p.life_diet, p.diet, sizeof p.diet) == 0);
    assert(pet_has(&p, U_SPROUT) && pet_has(&p, U_GLASSES) && pet_has(&p, U_SHINY));
    assert(pet_worn(&p, WEAR_HAT) == U_SPROUT && pet_worn(&p, WEAR_FACE) == U_GLASSES);
    assert(pet_worn(&p, WEAR_SHINY) == U_SHINY);
    assert(p.find_rng == 0xabc);
    /* A later save preserves the seeded DNA. */
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(q.dna == p.dna && q.form == 0 && q.forms_seen == 0 && !pet_save_needed(&p, &q));
    assert(q.generation == 1 && q.life_xp == 5000000 && q.life_born == p.born && q.inherited_trait == -1);
    assert(same_forms(&p, &q));

    /* The fallback never looks at the find sequence: a different (or missing) find_rng, same DNA. */
    snprintf(text, sizeof text, "name=Legacy\nborn=%lld\nxp=5000000\nstage=4\nadult_family=1\nfind_rng=123\n",
             (long long)(NOW - 86400));
    write_file(text);
    assert(pet_load(&q, path) && q.dna == p.dna && same_forms(&p, &q));
    snprintf(text, sizeof text, "name=Legacy\nborn=%lld\nxp=5000000\nstage=4\nadult_family=1\n",
             (long long)(NOW - 86400));
    write_file(text);
    assert(pet_load(&q, path) && q.dna == p.dna && same_forms(&p, &q));
    puts("PASS old save keeps original form, progress and outfits; DNA seeded from birth");
}

static void no_reroll(void) {
    Pet p, q, before;
    adult(&p, "Rerun", FAM_CLAUDE, 2500000);
    before = p;
    assert(p.dna == pet_dna(&p));

    /* A fresh meal on a known ledger draws the welcome find, advancing the find sequence. */
    Feed f = {0};
    strcpy(f.ledger_id, "grow");
    f.fed[FAM_CLAUDE] = 2500000;
    f.turns = 1;
    pet_feed(&p, &f, NOW + 1, 0);
    assert(p.find_rng != before.find_rng && p.find_day >= 0);
    assert(p.dna == before.dna && same_forms(&p, &before));
    p.find_rng ^= UINT64_C(0xDEADBEEFCAFEF00D);
    assert(same_forms(&p, &before));

    /* Restarts cannot reroll. */
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(q.dna == p.dna && same_forms(&q, &before) && !pet_save_needed(&p, &q));

    /* The adult family is locked: a new favorite changes colours elsewhere, never the forms. */
    eat(&q, "gpt", FAM_GPT, 50000000, NOW + 2);
    assert(pet_favorite(&q) == FAM_GPT && q.adult_family == FAM_CLAUDE);
    assert(same_forms(&q, &before));

    /* Same name and birth, same DNA, regardless of what happened since. */
    Pet twin;
    pet_init(&twin, "Rerun", NOW);
    assert(twin.dna == before.dna);
    puts("PASS no reroll after finds, find_rng changes, reload or diet changes");
}

static void inherited_forms(void) {
    Pet p;
    adult(&p, "Lineage", FAM_GEMINI, 200000000);
    uint8_t prev[7] = {0}, cur[7];
    PetTraits zero = {0};
    assert(same_traits(pet_form_traits(&p, 0), zero));
    assert(same_traits(pet_form_traits(&p, -1), zero) && same_traits(pet_form_traits(&p, PET_FORMS), zero));
    for (int form = 1; form < PET_FORMS; form++) {
        traits_vec(pet_form_traits(&p, form), cur);
        for (int i = 0; i < 7; i++) assert(cur[i] <= 2);
        for (int other = 1; other < form; other++) /* every generated form is unique */
            assert(!same_traits(pet_form_traits(&p, form), pet_form_traits(&p, other)));
        if (form > 1) { /* Each transformation retains six of its seven inherited traits. */
            int diffs = 0;
            for (int i = 0; i < 7; i++)
                diffs += cur[i] != prev[i];
            assert(diffs == 1);
        }
        memcpy(prev, cur, sizeof prev);
    }

    /* Pets differ by DNA, and the same DNA differs by adult family. */
    Pet other;
    adult(&other, "Stranger", FAM_GEMINI, 200000000);
    assert(other.dna != p.dna && !same_forms(&p, &other));
    other = p;
    other.adult_family = FAM_CLAUDE;
    assert(!same_forms(&p, &other));

    /* Before adulthood previews use the current favorite, which then becomes the locked family. */
    Pet teen = p;
    teen.stage = STAGE_TEEN;
    teen.adult_family = FAM_OTHER;
    assert(pet_favorite(&teen) == FAM_GEMINI && same_forms(&teen, &p));
    puts("PASS inherited forms: unique, one trait at a time, per pet and per adult family");
}

static void thresholds(void) {
    assert(pet_form_threshold(0) == 0 && pet_form_threshold(-1) == 0 && pet_form_threshold(PET_FORMS) == 0);
    assert(pet_form_threshold(1) == 2000000 && pet_form_threshold(2) == 3000000);
    assert(pet_form_threshold(3) == 5000000 && pet_form_threshold(4) == 8000000);
    assert(pet_form_threshold(15) == 107000000);
    assert(pet_form_threshold(1) == stage_threshold(STAGE_TEEN)); /* form 1 arrives with adulthood */
    Pet p;
    pet_init(&p, "Steps", NOW);
    assert(pet_forms_available(&p) == 1);
    p.stage = STAGE_ADULT;
    for (int form = 1; form < PET_FORMS; form++) {
        assert(pet_form_threshold(form) > pet_form_threshold(form - 1));
        p.life_xp = pet_form_threshold(form) - 1;
        assert(pet_forms_available(&p) == form);
        p.life_xp = pet_form_threshold(form);
        assert(pet_forms_available(&p) == form + 1);
    }
    p.life_xp = INT64_MAX;
    assert(pet_forms_available(&p) == PET_FORMS);
    p.stage = STAGE_TEEN;
    assert(pet_forms_available(&p) == 1); /* XP alone never unlocks forms before adulthood */
    /* Forms follow the current life: lifetime XP from earlier lives never unlocks them. */
    p.stage = STAGE_ADULT;
    p.xp = INT64_MAX;
    p.life_xp = pet_form_threshold(2) - 1;
    assert(pet_forms_available(&p) == 2);

    /* Through real meals: one token short stays a teen with only the original. */
    pet_init(&p, "Steps", NOW);
    eat(&p, "a", FAM_CLAUDE, 1999999, NOW);
    assert(p.stage == STAGE_TEEN && pet_forms_available(&p) == 1);
    eat(&p, "b", FAM_CLAUDE, 1, NOW + 1);
    assert(p.stage == STAGE_ADULT && pet_forms_available(&p) == 2 && p.form == 0);
    puts("PASS form thresholds and their exact boundaries");
}

static void choosing(void) {
    Pet p, q;
    adult(&p, "Chooser", FAM_CLAUDE, 5000000);
    assert(pet_forms_available(&p) == 4 && p.form == 0);
    assert(pet_worn(&p, WEAR_HAT) == U_SPROUT && pet_worn(&p, WEAR_DESK) == U_TROPHY);
    assert(pet_toggle_equip(&p, U_SHINY) == false); /* not earned at 5M */
    Pet base = p;

    /* Locked, invalid and already worn forms are refused without any change. */
    const int refused[] = {4, 5, PET_FORMS - 1, PET_FORMS, -1, 255, 0};
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        assert(!pet_choose_form(&p, refused[i]));
        assert(p.form == 0 && !pet_save_needed(&p, &base));
    }

    /* Choosing never touches equipment, XP, unlocks or finds, and needs a save. */
    assert(pet_choose_form(&p, 3) && p.form == 3);
    assert(!pet_choose_form(&p, 3));
    assert(memcmp(p.equipped, base.equipped, sizeof p.equipped) == 0);
    assert(memcmp(&p.unlocked, &base.unlocked, sizeof p.unlocked) == 0);
    assert(p.xp == base.xp && p.stage == base.stage && p.adult_family == base.adult_family);
    assert(p.find_rng == base.find_rng && p.find_points == base.find_points && p.dna == base.dna);
    assert(p.forms_seen == base.forms_seen);
    assert(pet_save_needed(&p, &base));

    /* Equipment changes while transformed stay put across form changes and a reload. */
    assert(pet_toggle_equip(&p, U_SPROUT) && pet_worn(&p, WEAR_HAT) == -1);
    int16_t worn[N_WEAR];
    memcpy(worn, p.equipped, sizeof worn);
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(q.form == 3 && !pet_save_needed(&p, &q));
    assert(memcmp(q.equipped, worn, sizeof worn) == 0);

    /* Every reached form, including the original, stays switchable in any order. */
    assert(pet_choose_form(&q, 1) && q.form == 1);
    assert(pet_choose_form(&q, 0) && q.form == 0);
    assert(pet_choose_form(&q, 3) && q.form == 3);
    assert(pet_choose_form(&q, 2) && q.form == 2);
    assert(pet_choose_form(&q, 0) && q.form == 0);
    assert(memcmp(q.equipped, worn, sizeof worn) == 0 && q.xp == base.xp);

    /* Before adulthood only the original exists. */
    Pet young;
    pet_init(&young, "Young", NOW);
    assert(!pet_choose_form(&young, 1) && !pet_choose_form(&young, 0) && young.form == 0);
    puts("PASS locked choice refusal, original form recovery and equipment retention");
}

static void seen_and_catch_up(void) {
    Pet p;
    pet_init(&p, "Catchup", NOW);
    pet_seen_forms(&p);
    assert(p.forms_seen == 0);

    /* A big backlog unlocks many forms at once but never transforms on its own. */
    eat(&p, "backlog", FAM_GPT, 8000000, NOW);
    assert(p.stage == STAGE_ADULT && pet_forms_available(&p) == 5 && p.form == 0 && p.forms_seen == 0);
    Pet base = p;
    pet_seen_forms(&p);
    assert(p.forms_seen == 4 && pet_save_needed(&p, &base));
    assert(memcmp(&p.unlocked, &base.unlocked, sizeof p.unlocked) == 0);
    assert(p.find_rng == base.find_rng && p.find_points == base.find_points && p.form == 0);
    Pet once = p;
    pet_seen_forms(&p);
    assert(!pet_save_needed(&p, &once));

    eat(&p, "more", FAM_GPT, 200000000, NOW + 1);
    assert(pet_forms_available(&p) == PET_FORMS && p.form == 0 && p.forms_seen == 4);
    pet_seen_forms(&p);
    assert(p.forms_seen == PET_FORMS - 1);

    /* Every persisted evolution field counts for saving. */
    Pet q = p;
    q.dna ^= 1;
    assert(pet_save_needed(&q, &p));
    q = p;
    q.form = 7;
    assert(pet_save_needed(&q, &p));
    q = p;
    q.forms_seen = 3;
    assert(pet_save_needed(&q, &p));
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(!pet_save_needed(&p, &q) && q.forms_seen == PET_FORMS - 1);
    puts("PASS catch-up unlocks without transforming, seen marking and save-needed fields");
}

static void load_validation(void) {
    Pet p, born;
    char text[512];
    /* Forms past the ready range (a hand edit or a newer build) fall back; seen is clamped. */
    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\ndna=abc\nform=9\nforms_seen=15\n",
             (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path));
    assert(p.dna == 0xabc && p.form == 0 && p.forms_seen == 3);

    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\ndna=abc\nform=2\nforms_seen=2\n",
             (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path) && p.form == 2 && p.forms_seen == 2);

    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=3\nform=1\nforms_seen=1\n", (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path) && p.form == 0 && p.forms_seen == 0);

    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\ndna=0\nform=-3\nforms_seen=-2\n",
             (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path) && p.form == 0 && p.forms_seen == 0);
    pet_init(&born, "Odd", NOW);
    assert(p.dna == born.dna);

    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\nform=300\nforms_seen=999\n", (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path) && p.form == 0 && p.forms_seen == 3);
    puts("PASS load keeps forms within the ready range");
}

int main(void) {
    snprintf(path, sizeof path, "/tmp/paseo-evolution-test-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    old_save_migration();
    no_reroll();
    inherited_forms();
    thresholds();
    choosing();
    seen_and_catch_up();
    load_validation();
    assert(unlink(path) == 0);
    return 0;
}
