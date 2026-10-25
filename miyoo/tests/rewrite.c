/* DNA Rewrite storage: life archives, the cutover of the current pet, and every refusal path. */
#include "pet.h"
#include "rewrite.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const int64_t NOW = 1792929600; /* fixed bridge time, not the device clock */
static const int64_t MEAL = 200000000; /* past the final form's 107M tokens */

static char root[64];
static char dir[128], pet_path[256], life1[288], life2[288], staging[288];
static char error[1024];

/* Fresh directory per scenario, so no leftover file can satisfy a later check. */
static void scenario(const char *name) {
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    assert(mkdir(dir, 0700) == 0);
    snprintf(pet_path, sizeof pet_path, "%s/pet.txt", dir);
    snprintf(life1, sizeof life1, "%s.life-1", pet_path);
    snprintf(life2, sizeof life2, "%s.life-2", pet_path);
    snprintf(staging, sizeof staging, "%s.tmp", pet_path);
}

/* Removes the files a scenario may create; rmdir() then proves nothing else (no stray staging
 * file) was left behind. */
static void finish(void) {
    const char *files[] = {pet_path, life1, life2};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) assert(unlink(files[i]) == 0 || errno == ENOENT);
    assert(rmdir(dir) == 0);
}

/* Cumulative counts reported by one bridge ledger. */
static void feed(Pet *p, const char *ledger, Family family, int64_t cumulative, int64_t now) {
    Feed f = {0};
    snprintf(f.ledger_id, sizeof f.ledger_id, "%s", ledger);
    f.fed[family] = cumulative;
    pet_feed(p, &f, now, 0);
}

/* An original pet at its final form: generation 1 credits the whole backlog to its life. */
static void grown(Pet *p, const char *name) {
    pet_init(p, name, NOW);
    feed(p, "bridge", FAM_CLAUDE, MEAL, NOW);
    assert(p->generation == 1 && p->stage == STAGE_ADULT && pet_rewrite_ready(p));
}

static bool missing(const char *path) {
    struct stat st;
    return lstat(path, &st) != 0 && errno == ENOENT;
}

static size_t slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, cap, f);
    assert(!ferror(f) && n < cap);
    fclose(f);
    return n;
}

static void put(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static bool same_file(const char *path, const char *bytes, size_t len) {
    static char now[65536];
    size_t n = slurp(path, now, sizeof now);
    return n == len && memcmp(now, bytes, len) == 0;
}

/* The same persisted pet, life and lineage fields included (eating_until is never saved). */
static bool same_pet(const Pet *a, const Pet *b) {
    return !pet_save_needed(a, b) && a->generation == b->generation && a->life_born == b->life_born &&
           a->life_xp == b->life_xp && !memcmp(a->life_diet, b->life_diet, sizeof a->life_diet) &&
           a->inherited_trait == b->inherited_trait && a->inherited_value == b->inherited_value &&
           a->born == b->born;
}

/* The commit must refuse, explain why, and leave the in-memory pet untouched. */
static void refused(Pet *p, const char *path, int trait, int64_t now) {
    Pet before = *p;
    error[0] = 0;
    assert(!pet_rewrite_commit(p, path, trait, now, error, sizeof error));
    assert(error[0]);
    assert(same_pet(p, &before) && p->dna == before.dna && p->eating_until == before.eating_until);
}

static void roundtrip_and_next_life(void) {
    scenario("roundtrip");
    Pet p, old, expect, loaded;
    grown(&p, "Rex");
    assert(pet_save(&p, pet_path));
    old = p;
    assert(pet_rewrite_preview(&old, 2, NOW + 100, &expect));

    assert(pet_rewrite_commit(&p, pet_path, 2, NOW + 100, error, sizeof error));
    assert(!error[0]);
    assert(same_pet(&p, &expect) && p.generation == 2 && p.dna == expect.dna && p.dna != old.dna);
    assert(p.stage == STAGE_BABY && p.life_xp == 0 && p.life_born == NOW + 100 && p.inherited_trait == 2);
    assert(p.eating_until == 0 && p.cheer_until == 0);
    /* Lifetime stats and the collection carry over. */
    assert(p.xp == old.xp && !memcmp(&p.unlocked, &old.unlocked, sizeof p.unlocked));
    assert(!memcmp(p.equipped, old.equipped, sizeof p.equipped) && p.find_rng == old.find_rng);
    assert(missing(staging));

    /* The archive is the old life, exactly; the current file is the new one. */
    assert(pet_lineage_load(pet_path, 1, &loaded, error, sizeof error));
    assert(same_pet(&loaded, &old) && loaded.dna == old.dna && loaded.form == old.form);
    assert(pet_load(&loaded, pet_path) && same_pet(&loaded, &p));
    /* The current life has no archive; generation 0 does not exist. */
    loaded.name[0] = 'Z';
    assert(!pet_lineage_load(pet_path, 2, &loaded, error, sizeof error) && error[0] && loaded.name[0] == 'Z');
    assert(!pet_lineage_load(pet_path, 0, &loaded, error, sizeof error) && loaded.name[0] == 'Z');
    assert(pet_lineage_load(pet_path, 1, &loaded, NULL, 0) && loaded.dna == old.dna);
    puts("PASS commit archives the old life before the new one replaces the pet");

    /* The next life is not ready yet. */
    refused(&p, pet_path, 0, NOW + 150);
    assert(missing(life2));

    /* Grow life 2 to its final form: the first snapshot after the rewrite is only a baseline. */
    feed(&p, "bridge", FAM_CLAUDE, MEAL, NOW + 150);
    assert(p.life_xp == 0 && p.xp == MEAL);
    feed(&p, "bridge", FAM_CLAUDE, 2 * MEAL, NOW + 160);
    assert(p.life_xp == MEAL && p.xp == 2 * MEAL && pet_rewrite_ready(&p));

    static char first[65536];
    size_t first_len = slurp(life1, first, sizeof first);
    Pet second = p;
    assert(pet_rewrite_commit(&p, pet_path, 5, NOW + 200, error, sizeof error) && !error[0]);
    assert(p.generation == 3 && p.xp == 2 * MEAL && p.dna != second.dna && p.dna != old.dna);
    /* Its own archive; the first stays byte for byte as it was. */
    assert(same_file(life1, first, first_len));
    assert(pet_lineage_load(pet_path, 2, &loaded, error, sizeof error));
    assert(same_pet(&loaded, &second) && loaded.generation == 2 && loaded.dna == second.dna);
    assert(loaded.inherited_trait == 2);
    assert(pet_lineage_load(pet_path, 1, &loaded, error, sizeof error) && same_pet(&loaded, &old));
    assert(pet_load(&loaded, pet_path) && same_pet(&loaded, &p));
    puts("PASS each life keeps its own archive and earlier ones never change");
    finish();
}

static void failures_leave_pet_unchanged(void) {
    scenario("failures");
    Pet p;
    pet_init(&p, "Egg", NOW);
    refused(&p, pet_path, 0, NOW + 100); /* not at its final form */
    assert(missing(life1) && missing(pet_path));

    grown(&p, "Rex");
    assert(pet_save(&p, pet_path));
    static char current[65536];
    size_t current_len = slurp(pet_path, current, sizeof current);

    refused(&p, pet_path, -1, NOW + 100); /* no such trait */
    refused(&p, pet_path, PET_TRAITS, NOW + 100);
    refused(&p, pet_path, 0, 0);           /* no clock */
    refused(&p, pet_path, 0, NOW - 1);     /* before this life began */
    assert(missing(life1) && same_file(pet_path, current, current_len));

    refused(&p, NULL, 0, NOW + 100);
    refused(&p, "", 0, NOW + 100);
    char dir_only[300];
    snprintf(dir_only, sizeof dir_only, "%s/", root);
    refused(&p, dir_only, 0, NOW + 100);
    static char long_path[REWRITE_PATH_MAX + 8];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = 0;
    refused(&p, long_path, 0, NOW + 100);
    /* Fits on its own, but not with ".life-1.tmp" behind it. */
    memset(long_path, 'a', sizeof long_path);
    long_path[0] = '/';
    long_path[REWRITE_PATH_MAX - 8] = 0;
    refused(&p, long_path, 0, NOW + 100);
    assert(!pet_lineage_load(long_path, 1, &p, error, sizeof error) && error[0]);

    char nowhere[300];
    snprintf(nowhere, sizeof nowhere, "%s/no-such-dir/pet.txt", root);
    refused(&p, nowhere, 0, NOW + 100); /* the archive cannot be written */
    puts("PASS invalid requests and unwritable archives change nothing");

    /* The archive lands, then the current pet cannot be replaced: still the old life everywhere. */
    assert(mkdir(staging, 0700) == 0);
    refused(&p, pet_path, 3, NOW + 100);
    assert(same_file(pet_path, current, current_len));
    Pet staged;
    assert(pet_lineage_load(pet_path, 1, &staged, error, sizeof error) && same_pet(&staged, &p));
    puts("PASS a failed cutover leaves the current pet and keeps the archive as staging");

    /* Retry later with newer lifetime stats: the staged archive of this same life is refreshed. */
    assert(rmdir(staging) == 0);
    p.lifetime_turns += 5;
    feed(&p, "second", FAM_GPT, 1234, NOW + 110); /* generation 1: a new ledger's backlog feeds it */
    Pet old = p;
    assert(pet_rewrite_commit(&p, pet_path, 3, NOW + 120, error, sizeof error) && !error[0]);
    assert(p.generation == 2 && p.lifetime_turns == old.lifetime_turns);
    assert(pet_lineage_load(pet_path, 1, &staged, error, sizeof error) && same_pet(&staged, &old));
    assert(staged.lifetime_turns == old.lifetime_turns && staged.diet[FAM_GPT] == 1234);
    Pet cur;
    assert(pet_load(&cur, pet_path) && same_pet(&cur, &p));
    puts("PASS retry after an archive-only success refreshes that archive and completes");
    finish();
}

/* `bytes` at life-1 must make the archive unreadable and the commit refuse without touching it. */
static void rejects(Pet *p, const char *bytes, size_t len, const char *what) {
    put(life1, bytes, len);
    Pet out;
    out.name[0] = 'Z';
    assert(!pet_lineage_load(pet_path, 1, &out, error, sizeof error) && error[0] && out.name[0] == 'Z');
    refused(p, pet_path, 0, NOW + 100);
    assert(same_file(life1, bytes, len));
    printf("PASS refuses %s\n", what);
}

static void conflicts(void) {
    scenario("conflicts");
    static char text[65536], other_text[65536];
    Pet p, other;
    grown(&p, "Rex");
    assert(pet_save(&p, pet_path));
    static char current[65536];
    size_t current_len = slurp(pet_path, current, sizeof current);

    /* A valid archive of a different life (other name and birth, so other DNA). */
    pet_init(&other, "Max", NOW - 50);
    feed(&other, "bridge", FAM_GEMINI, MEAL, NOW - 50);
    assert(pet_rewrite_ready(&other) && other.dna != p.dna);
    assert(pet_save(&other, life1));
    size_t other_len = slurp(life1, other_text, sizeof other_text);
    Pet loaded;
    assert(pet_lineage_load(pet_path, 1, &loaded, error, sizeof error) && loaded.dna == other.dna);
    refused(&p, pet_path, 0, NOW + 100);
    assert(same_file(life1, other_text, other_len) && same_file(pet_path, current, current_len));
    puts("PASS refuses to overwrite another life's archive");

    rejects(&p, "", 0, "an empty archive");
    rejects(&p, "hello\n", 6, "a malformed archive");
    /* A real snapshot, cut short mid-line. */
    assert(pet_save(&p, staging));
    size_t len = slurp(staging, text, sizeof text);
    assert(unlink(staging) == 0);
    rejects(&p, text, len - 3, "a truncated archive");
    /* A legacy save: no generation or life fields, so pet_load() would only guess them. */
    const char *legacy = "name=Rex\nborn=1792929600\nxp=200000000\nstage=4\nadult_family=0\n"
                         "diet=200000000 0 0 0\nunlocked=0 0 0 0\nequipped=-1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1\n"
                         "dna=1\nform=0\n";
    rejects(&p, legacy, strlen(legacy), "a legacy save without lineage fields");
    /* The same life with one required field written twice. */
    char doubled[70000];
    int n = snprintf(doubled, sizeof doubled, "%.*sgeneration=1\n", (int)len, text);
    rejects(&p, doubled, (size_t)n, "a duplicated field");
    /* A life that never reached its final form. */
    Pet young;
    pet_init(&young, "Rex", NOW);
    assert(pet_save(&young, staging));
    len = slurp(staging, text, sizeof text);
    assert(unlink(staging) == 0);
    rejects(&p, text, len, "an archive of an unfinished life");
    /* A complete archive of another generation. */
    Pet later;
    assert(pet_rewrite_preview(&p, 1, NOW + 10, &later));
    later.life_xp = MEAL, later.stage = STAGE_ADULT, later.adult_family = FAM_CLAUDE;
    assert(pet_rewrite_ready(&later) && later.generation == 2);
    assert(pet_save(&later, staging));
    len = slurp(staging, text, sizeof text);
    assert(unlink(staging) == 0);
    rejects(&p, text, len, "an archive recording another generation");

    /* Not a regular file. */
    assert(unlink(life1) == 0 && mkdir(life1, 0700) == 0);
    refused(&p, pet_path, 0, NOW + 100);
    assert(rmdir(life1) == 0);
    assert(symlink(pet_path, life1) == 0);
    refused(&p, pet_path, 0, NOW + 100);
    assert(unlink(life1) == 0);
    assert(same_file(pet_path, current, current_len));
    puts("PASS refuses directories and symlinks in the archive's place");

    /* With the place clear the same pet commits. */
    assert(pet_rewrite_commit(&p, pet_path, 0, NOW + 100, error, sizeof error) && p.generation == 2);
    finish();
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/paseo-rewrite-test-XXXXXX");
    assert(mkdtemp(root));
    roundtrip_and_next_life();
    failures_leave_pet_unchanged();
    conflicts();
    assert(rmdir(root) == 0);
    return 0;
}
