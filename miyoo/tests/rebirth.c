#include "pet.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const int64_t NOW = 1792929600; /* fixed bridge time, not the device clock */
static const int64_t DAY = 86400;
static const int64_t FINAL_XP = 107000000; /* pet_form_threshold(PET_FORMS - 1) */
static char path[128];

/* One bridge: cumulative counts, as a real ledger only ever reports totals. */
typedef struct {
    char id[16];
    int64_t fed[N_FAMILIES];
    int64_t turns;
} Bridge;

static Bridge bridge(const char *id) {
    Bridge b = {0};
    snprintf(b.id, sizeof b.id, "%s", id);
    return b;
}

static FeedResult feed(Pet *p, Bridge *b, Family family, int64_t tokens, int64_t now) {
    static Feed f;
    memset(&f, 0, sizeof f);
    b->fed[family] += tokens;
    snprintf(f.ledger_id, sizeof f.ledger_id, "%s", b->id);
    memcpy(f.fed, b->fed, sizeof f.fed);
    f.turns = b->turns;
    return pet_feed(p, &f, now, 0);
}

static const LedgerSeen *seen(const Pet *p, const char *id) {
    for (int i = 0; i < PET_LEDGERS; i++)
        if (!strcmp(p->seen[i].ledger_id, id)) return &p->seen[i];
    return NULL;
}

static void traits_vec(PetTraits t, uint8_t v[PET_TRAITS]) {
    v[0] = t.body, v[1] = t.ears, v[2] = t.tail, v[3] = t.crown;
    v[4] = t.wings, v[5] = t.markings, v[6] = t.motion;
}

static bool same_traits(PetTraits a, PetTraits b) {
    uint8_t x[PET_TRAITS], y[PET_TRAITS];
    traits_vec(a, x);
    traits_vec(b, y);
    return memcmp(x, y, sizeof x) == 0;
}

static bool same_forms(const Pet *a, const Pet *b) {
    for (int form = 0; form < PET_FORMS; form++)
        if (!same_traits(pet_form_traits(a, form), pet_form_traits(b, form))) return false;
    return true;
}

static uint8_t trait_of(const Pet *p, int form, int trait) {
    uint8_t v[PET_TRAITS];
    traits_vec(pet_form_traits(p, form), v);
    return v[trait];
}

/* The form sequence exactly as released before DNA rewrites existed: generation 1 must keep it. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static PetTraits released_traits(const Pet *p, int form) {
    PetTraits t = {0};
    if (form < 1 || form >= PET_FORMS) return t;
    Family family = p->stage == STAGE_ADULT ? p->adult_family : pet_favorite(p);
    uint64_t h = pet_dna(p) ^ (UINT64_C(0x9E3779B97F4A7C15) * (uint64_t)(family + 1));
    uint64_t x = splitmix64(&h);
    uint8_t v[7];
    for (int i = 0; i < 7; i++, x /= 3) v[i] = (uint8_t)(x % 3);
    int first = (int)(x % 7);
    for (int k = 2; k <= form; k++) {
        int i = (first + k - 2) % 7;
        v[i] = (uint8_t)((v[i] + 1) % 3);
    }
    t.body = v[0], t.ears = v[1], t.tail = v[2], t.crown = v[3];
    t.wings = v[4], t.markings = v[5], t.motion = v[6];
    return t;
}

static void write_file(const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    assert(fclose(f) == 0);
}

/* An original pet that reached its final form from one bridge's backlog. */
static void finished(Pet *p, Bridge *home, const char *name, Family family) {
    pet_init(p, name, NOW);
    *home = bridge("home");
    feed(p, home, family, FINAL_XP, NOW);
    assert(p->generation == 1 && p->stage == STAGE_ADULT && p->life_xp == FINAL_XP);
    assert(pet_forms_available(p) == PET_FORMS && pet_rewrite_ready(p));
}

/* Fifteen distinct forms, neighbours one field apart, and the inherited field never changing. */
static void check_lineage(const Pet *p) {
    int kept = p->inherited_trait;
    uint8_t prev[PET_TRAITS] = {0}, cur[PET_TRAITS];
    for (int form = 1; form < PET_FORMS; form++) {
        traits_vec(pet_form_traits(p, form), cur);
        for (int i = 0; i < PET_TRAITS; i++) assert(cur[i] <= 2);
        if (kept >= 0) assert(cur[kept] == p->inherited_value);
        for (int other = 1; other < form; other++)
            assert(!same_traits(pet_form_traits(p, form), pet_form_traits(p, other)));
        if (form > 1) {
            int diffs = 0;
            for (int i = 0; i < PET_TRAITS; i++)
                if (cur[i] != prev[i]) diffs++, assert(i != kept);
            assert(diffs == 1);
        }
        memcpy(prev, cur, sizeof prev);
    }
}

static void migration(void) {
    char text[640];
    /* A released save: no life fields, nine-field seen lines (and one from before finds). */
    snprintf(text, sizeof text,
             "name=Legacy\nborn=%lld\nxp=8000000\nstage=4\nadult_family=2\ndiet=1000000 0 7000000 0\n"
             "unlocked=200021\nequipped=0 5 -1 -1 -1 -1 21\nfind_rng=abc\ndna=1234567\nform=3\nforms_seen=4\n"
             "seen=home 1000000 0 7000000 0 50 0 %lld 1\nseen=old 0 0 0 0 5 0 %lld\n",
             (long long)(NOW - DAY), (long long)NOW, (long long)NOW);
    write_file(text);
    Pet p, q;
    assert(pet_load(&p, path));
    assert(p.generation == 1 && p.inherited_trait == -1 && p.inherited_value == 0);
    assert(p.life_xp == 8000000 && p.life_born == NOW - DAY && memcmp(p.life_diet, p.diet, sizeof p.diet) == 0);
    assert(p.dna == 0x1234567 && p.form == 3 && p.forms_seen == 4 && pet_forms_available(&p) == 5);
    assert(pet_favorite(&p) == FAM_GEMINI && p.adult_family == FAM_GEMINI);
    for (int form = 0; form < PET_FORMS; form++) assert(same_traits(pet_form_traits(&p, form), released_traits(&p, form)));
    assert(seen(&p, "home")->finds_armed && !seen(&p, "home")->life_armed);
    assert(!seen(&p, "old")->finds_armed && !seen(&p, "old")->life_armed);
    assert(!pet_rewrite_ready(&p));

    /* The original life still eats everything, a new bridge's history included, as before. */
    Bridge laptop = bridge("laptop");
    feed(&p, &laptop, FAM_CLAUDE, 3000000, NOW);
    assert(p.xp == 11000000 && p.life_xp == 11000000 && p.life_diet[FAM_CLAUDE] == 4000000);
    assert(seen(&p, "laptop")->life_armed && memcmp(p.life_diet, p.diet, sizeof p.diet) == 0);

    /* New fields persist explicitly and reload exactly. */
    assert(pet_save(&p, path) && pet_load(&q, path));
    assert(!pet_save_needed(&p, &q) && same_forms(&p, &q) && q.generation == 1 && q.life_xp == p.life_xp);
    assert(seen(&q, "laptop")->life_armed && !seen(&q, "old")->life_armed && seen(&q, "home")->finds_armed);
    puts("PASS released saves load as generation 1 with the same DNA, forms and feeding");
}

static void load_validation(void) {
    char text[512];
    Pet p;
    /* A tenth seen field is read independently of finds_armed. */
    snprintf(text, sizeof text,
             "name=Ten\nborn=%lld\nxp=10\nstage=1\ngeneration=2\nlife_born=%lld\nlife_xp=0\nlife_diet=0 0 0 0\n"
             "inherited_trait=6\ninherited_value=2\nseen=a 1 0 0 0 0 0 %lld 0 1\nseen=b 1 0 0 0 0 0 %lld 1 0\n",
             (long long)NOW, (long long)(NOW + DAY), (long long)NOW, (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path));
    assert(p.generation == 2 && p.life_born == NOW + DAY && p.life_xp == 0 && p.xp == 10);
    assert(p.inherited_trait == 6 && p.inherited_value == 2);
    assert(!seen(&p, "a")->finds_armed && seen(&p, "a")->life_armed);
    assert(seen(&p, "b")->finds_armed && !seen(&p, "b")->life_armed);

    /* Generation 0 is the original life; the original life never inherits. */
    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\ngeneration=0\ninherited_trait=3\n"
             "inherited_value=1\n", (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path) && p.generation == 1 && p.inherited_trait == -1 && p.inherited_value == 0);
    assert(p.life_xp == 5000000);
    /* Bad inheritance records fall back to the plain sequence. */
    const char *bad[] = {"inherited_trait=7\ninherited_value=1\n", "inherited_trait=2\ninherited_value=3\n",
                         "inherited_trait=-2\ninherited_value=0\n", "inherited_trait=1\ninherited_value=-1\n"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\ngeneration=3\nlife_xp=0\n%s",
                 (long long)NOW, bad[i]);
        write_file(text);
        assert(pet_load(&p, path) && p.generation == 3 && p.inherited_trait == -1 && p.inherited_value == 0);
        assert(pet_forms_available(&p) == 1 && p.form == 0);
    }
    snprintf(text, sizeof text, "name=Odd\nborn=%lld\nxp=5000000\nstage=4\ngeneration=99999999999\n", (long long)NOW);
    write_file(text);
    assert(pet_load(&p, path) && p.generation == UINT32_MAX && !pet_rewrite_ready(&p));
    puts("PASS load reads the tenth seen field and sanitizes generation and inheritance");
}

static void readiness(void) {
    Pet p;
    Bridge home = bridge("home");
    pet_init(&p, "Ready", NOW);
    assert(!pet_rewrite_ready(&p));
    feed(&p, &home, FAM_CLAUDE, FINAL_XP - 1, NOW);
    assert(p.stage == STAGE_ADULT && pet_forms_available(&p) == PET_FORMS - 1 && !pet_rewrite_ready(&p));
    Pet out;
    assert(!pet_rewrite_preview(&p, 0, NOW + 1, &out));
    feed(&p, &home, FAM_CLAUDE, 1, NOW + 1);
    assert(pet_forms_available(&p) == PET_FORMS && pet_rewrite_ready(&p));

    /* Readiness follows the current life only, and needs an adult. */
    Pet q = p;
    q.life_xp = FINAL_XP - 1;
    q.xp = INT64_MAX;
    assert(!pet_rewrite_ready(&q));
    q = p;
    q.stage = STAGE_TEEN;
    assert(!pet_rewrite_ready(&q));
    /* The generation must be able to advance. */
    q = p;
    q.generation = UINT32_MAX;
    assert(!pet_rewrite_ready(&q) && !pet_rewrite_preview(&q, 0, NOW + 1, &out));
    q.generation = UINT32_MAX - 1;
    assert(pet_rewrite_ready(&q) && pet_rewrite_preview(&q, 0, NOW + 1, &out) && out.generation == UINT32_MAX);
    q.generation = 0;
    assert(!pet_rewrite_ready(&q));
    puts("PASS rewrite readiness: adult with every form this life, generation can advance");
}

static void preview(void) {
    Pet p, out, snap;
    Bridge home;
    finished(&p, &home, "Phoenix", FAM_CLAUDE);
    Bridge laptop = bridge("laptop");
    laptop.turns = 30;
    feed(&p, &laptop, FAM_GPT, 400000, NOW + 10); /* backlog, then fresh activity and finds */
    feed(&p, &laptop, FAM_GEMINI, 1500000, NOW + 20);
    assert(pet_toggle_equip(&p, U_SHINY) && pet_worn(&p, WEAR_SHINY) == U_SHINY);
    assert(pet_choose_form(&p, 3));
    pet_seen_forms(&p);
    pet_pat(&p, NOW + 30);
    p.eating_until = NOW + 40;
    assert(p.find_day >= 0 && p.today_tokens > 0 && p.forms_seen == PET_FORMS - 1);

    /* Refusals write nothing at all. */
    memcpy(&snap, &p, sizeof p);
    Pet sentinel;
    memset(&sentinel, 0xA5, sizeof sentinel);
    const int bad_traits[] = {-1, PET_TRAITS, 100};
    for (unsigned i = 0; i < sizeof bad_traits / sizeof bad_traits[0]; i++) {
        memcpy(&out, &sentinel, sizeof out);
        assert(!pet_rewrite_preview(&p, bad_traits[i], NOW + DAY, &out) && !memcmp(&out, &sentinel, sizeof out));
    }
    const int64_t bad_times[] = {0, -5, NOW - 1};
    for (unsigned i = 0; i < sizeof bad_times / sizeof bad_times[0]; i++) {
        memcpy(&out, &sentinel, sizeof out);
        assert(!pet_rewrite_preview(&p, 0, bad_times[i], &out) && !memcmp(&out, &sentinel, sizeof out));
    }
    assert(!memcmp(&snap, &p, sizeof p));

    /* A preview never touches the current pet. */
    assert(pet_rewrite_preview(&p, 2, NOW + DAY, &out));
    assert(!memcmp(&snap, &p, sizeof p));

    /* A fresh Baby life ... */
    assert(out.generation == 2 && out.life_born == NOW + DAY && out.life_xp == 0);
    for (int i = 0; i < N_FAMILIES; i++) assert(out.life_diet[i] == 0);
    assert(out.stage == STAGE_BABY && out.adult_family == FAM_OTHER && pet_favorite(&out) == FAM_OTHER);
    assert(out.form == 0 && out.forms_seen == 0 && out.cheer_until == 0 && out.eating_until == 0);
    assert(pet_forms_available(&out) == 1 && !pet_rewrite_ready(&out));
    assert(out.dna != 0 && out.dna != p.dna && out.inherited_trait == 2);
    /* ... carrying the trait from the OLD FINAL form, not the worn form 3. */
    assert(out.inherited_value == trait_of(&p, PET_FORMS - 1, 2));
    bool worn_differs = false;
    for (int trait = 0; trait < PET_TRAITS; trait++) {
        Pet t;
        assert(pet_rewrite_preview(&p, trait, NOW + DAY, &t));
        assert(t.inherited_trait == trait && t.inherited_value == trait_of(&p, PET_FORMS - 1, trait));
        worn_differs |= trait_of(&p, p.form, trait) != trait_of(&p, PET_FORMS - 1, trait);
    }
    assert(worn_differs);

    /* ... keeping the name, every lifetime total, the collection, outfits, finds and day caps. */
    assert(!strcmp(out.name, p.name) && out.born == p.born && out.xp == p.xp);
    assert(!memcmp(out.diet, p.diet, sizeof p.diet) && out.lifetime_turns == p.lifetime_turns);
    assert(out.day == p.day && out.today_tokens == p.today_tokens && out.today_turns == p.today_turns);
    assert(out.best_day_tokens == p.best_day_tokens);
    assert(!memcmp(&out.unlocked, &p.unlocked, sizeof p.unlocked) && !memcmp(out.equipped, p.equipped, sizeof p.equipped));
    assert(pet_worn(&out, WEAR_SHINY) == U_SHINY);
    assert(out.find_points == p.find_points && out.find_tokens == p.find_tokens && out.find_day == p.find_day);
    assert(out.find_rng == p.find_rng);
    /* ... with the same ledgers, each disarmed for the new life. */
    for (int i = 0; i < PET_LEDGERS; i++) {
        const LedgerSeen *a = &out.seen[i], *b = &p.seen[i];
        assert(!strcmp(a->ledger_id, b->ledger_id) && !memcmp(a->fed, b->fed, sizeof a->fed));
        assert(a->turns == b->turns && a->errors == b->errors && a->last_seen == b->last_seen);
        assert(a->finds_armed == b->finds_armed && !a->life_armed);
    }
    assert(seen(&p, "home")->life_armed && seen(&p, "laptop")->life_armed);

    /* The candidate never depends on the chosen trait, the time, the find sequence or later feeds,
     * so cancelling and reopening (even after live meals) shows the same DNA and inheritance. */
    Pet again, other = p;
    assert(pet_rewrite_preview(&p, 2, NOW + DAY, &again) && !pet_save_needed(&out, &again));
    assert(pet_rewrite_preview(&p, 5, NOW + 9 * DAY, &again) && again.dna == out.dna);
    other.find_rng ^= UINT64_C(0xDEADBEEFCAFEF00D);
    assert(pet_rewrite_preview(&other, 2, NOW + DAY, &again) && again.dna == out.dna);
    other = p;
    feed(&other, &home, FAM_GEMINI, 120000000, NOW + 50); /* favorite changes, adult family is locked */
    assert(pet_favorite(&other) == FAM_GEMINI && other.adult_family == FAM_CLAUDE);
    assert(pet_rewrite_preview(&other, 2, NOW + DAY, &again));
    assert(again.dna == out.dna && again.inherited_value == out.inherited_value && again.generation == 2);
    assert(again.xp == other.xp && again.diet[FAM_GEMINI] == other.diet[FAM_GEMINI]); /* newest lifetime state */

    /* Same old DNA and generation, same next DNA; a different pet gets its own. */
    Pet twin, twin_out, stranger, stranger_out;
    Bridge b;
    finished(&twin, &b, "Phoenix", FAM_GPT);
    assert(twin.dna == p.dna && pet_rewrite_preview(&twin, 0, NOW + 3 * DAY, &twin_out) && twin_out.dna == out.dna);
    finished(&stranger, &b, "Stranger", FAM_CLAUDE);
    assert(pet_rewrite_preview(&stranger, 0, NOW + DAY, &stranger_out) && stranger_out.dna != out.dna);
    puts("PASS preview: pure, Baby with fresh DNA, final-form trait, everything collected kept, stable");
}

static void save_fields(void) {
    Pet p, n, q;
    Bridge home;
    finished(&p, &home, "Saver", FAM_GEMINI);
    assert(pet_rewrite_preview(&p, 4, NOW + DAY, &n));
    assert(pet_save_needed(&n, &p));
    assert(pet_save(&n, path) && pet_load(&q, path));
    assert(!pet_save_needed(&n, &q) && same_forms(&n, &q));
    assert(q.generation == 2 && q.life_born == NOW + DAY && q.life_xp == 0 && q.inherited_trait == 4);
    assert(q.inherited_value == n.inherited_value && q.dna == n.dna && q.xp == p.xp && q.stage == STAGE_BABY);
    assert(!seen(&q, "home")->life_armed);

    /* Every new persisted field counts for saving. */
    q = n;
    q.life_xp++;
    assert(pet_save_needed(&q, &n));
    q = n;
    q.life_diet[FAM_OTHER]++;
    assert(pet_save_needed(&q, &n));
    q = n;
    q.life_born++;
    assert(pet_save_needed(&q, &n));
    q = n;
    q.generation++;
    assert(pet_save_needed(&q, &n));
    q = n;
    q.inherited_trait = 1;
    assert(pet_save_needed(&q, &n));
    q = n;
    q.inherited_value = (uint8_t)((q.inherited_value + 1) % 3);
    assert(pet_save_needed(&q, &n));
    q = n;
    q.seen[0].life_armed = true;
    assert(pet_save_needed(&q, &n));
    puts("PASS new life fields persist and count for saving");
}

static void fresh_feeding(void) {
    Pet p, n;
    Bridge home;
    finished(&p, &home, "Fresh", FAM_CLAUDE);
    const Bridge start = home; /* the bridge as the old life last saw it */
    assert(pet_rewrite_preview(&p, 0, NOW + DAY, &n));

    /* An unchanged snapshot only arms the ledger, and that alone needs a save. */
    Pet before = n;
    FeedResult r = feed(&n, &home, FAM_GPT, 0, NOW + DAY);
    assert(r.tokens == 0 && seen(&n, "home")->life_armed && pet_save_needed(&n, &before));
    assert(n.life_xp == 0 && n.xp == p.xp);

    /* A known bridge's first snapshot after a rewrite is a baseline: lifetime only. */
    Pet q;
    assert(pet_rewrite_preview(&p, 0, NOW + DAY, &q));
    Bridge home2 = start;
    r = feed(&q, &home2, FAM_GPT, 1200000, NOW + DAY);
    assert(r.tokens == 1200000 && q.xp == p.xp + 1200000 && q.diet[FAM_GPT] == 1200000);
    assert(q.life_xp == 0 && q.life_diet[FAM_GPT] == 0 && q.stage == STAGE_BABY && !r.hatched && !r.evolved);
    /* Day records and milestones still count it: they are lifetime. */
    assert(q.today_tokens >= 1200000 && pet_has(&q, U_MEDAL) && pet_has(&q, U_STAR_RUG));
    assert(seen(&q, "home")->life_armed);

    /* Once armed, growth feeds both. */
    r = feed(&n, &home, FAM_GPT, 100000, NOW + DAY + 1);
    assert(r.tokens == 100000 && n.life_xp == 100000 && n.life_diet[FAM_GPT] == 100000);
    assert(n.xp == p.xp + 100000 && n.stage == STAGE_KID && r.evolved && !r.hatched);

    /* A new bridge's history is lifetime only, even though it is armed afterwards. */
    Bridge laptop = bridge("laptop");
    r = feed(&n, &laptop, FAM_CLAUDE, 50000000, NOW + DAY + 2);
    assert(r.tokens == 50000000 && n.xp == p.xp + 50100000 && n.life_xp == 100000);
    assert(n.life_diet[FAM_CLAUDE] == 0 && n.stage == STAGE_KID && seen(&n, "laptop")->life_armed);
    feed(&n, &laptop, FAM_GEMINI, 1000, NOW + DAY + 3);
    assert(n.life_xp == 101000 && n.life_diet[FAM_GEMINI] == 1000);

    /* The new life grows up into its own favorite, not the lifetime one. */
    feed(&n, &home, FAM_GPT, 1899000, NOW + DAY + 4);
    assert(n.life_xp == 2000000 && n.stage == STAGE_ADULT && n.adult_family == FAM_GPT);
    assert(pet_favorite(&n) == FAM_GPT && n.diet[FAM_CLAUDE] > n.diet[FAM_GPT]);
    assert(pet_forms_available(&n) == 2 && n.form == 0);

    /* Age milestones still count from the original birth. */
    assert(!pet_has(&n, U_BONSAI));
    pet_check_unlocks(&n, NOW + 101 * DAY, 0);
    assert(pet_has(&n, U_BONSAI) && pet_has(&n, U_SCARF) && pet_has(&n, U_LOLLIPOP));

    /* A rebirth after the reload keeps disarmed ledgers disarmed: still a baseline. */
    Pet m, loaded;
    assert(pet_rewrite_preview(&p, 0, NOW + DAY, &m) && pet_save(&m, path) && pet_load(&loaded, path));
    Bridge home3 = start;
    feed(&loaded, &home3, FAM_GPT, 7000, NOW + DAY);
    assert(loaded.life_xp == 0 && loaded.xp == p.xp + 7000);
    puts("PASS later lives grow only from armed, known bridges; lifetime and milestones keep counting");
}

static void inheritance(void) {
    Pet p;
    Bridge home;
    finished(&p, &home, "Heir", FAM_GEMINI);
    for (int trait = 0; trait < PET_TRAITS; trait++) {
        Pet n;
        Bridge b = home;
        assert(pet_rewrite_preview(&p, trait, NOW + DAY, &n));
        uint8_t value = trait_of(&p, PET_FORMS - 1, trait);
        assert(n.inherited_trait == trait && n.inherited_value == value);
        check_lineage(&n); /* as a Baby: previews from the (empty) favorite */
        feed(&n, &b, FAM_CLAUDE, 0, NOW + DAY);
        feed(&n, &b, FAM_CLAUDE, 2000000, NOW + DAY + 1);
        assert(n.stage == STAGE_ADULT && n.adult_family == FAM_CLAUDE);
        check_lineage(&n);
        Pet locked = n;
        feed(&n, &b, FAM_GPT, FINAL_XP, NOW + DAY + 2); /* a new favorite after the family locks */
        assert(pet_favorite(&n) == FAM_GPT && n.adult_family == FAM_CLAUDE && same_forms(&n, &locked));
        assert(pet_forms_available(&n) == PET_FORMS && pet_rewrite_ready(&n));
        check_lineage(&n);
        assert(!same_forms(&n, &p)); /* fresh DNA: a different lineage */
        /* Without the inheritance this life's DNA would run the plain released sequence. */
        Pet plain = n;
        plain.inherited_trait = -1;
        for (int form = 0; form < PET_FORMS; form++)
            assert(same_traits(pet_form_traits(&plain, form), released_traits(&plain, form)));
    }
    /* The original life is untouched by all of this. */
    for (int form = 0; form < PET_FORMS; form++) assert(same_traits(pet_form_traits(&p, form), released_traits(&p, form)));
    check_lineage(&p);
    puts("PASS all seven inherited traits hold across fifteen distinct one-change forms");
}

static void reuse(void) {
    Pet g1, g2, g3, g3b;
    Bridge home;
    finished(&g1, &home, "Again", FAM_OTHER);
    assert(pet_rewrite_preview(&g1, 1, NOW + DAY, &g2));
    feed(&g2, &home, FAM_CLAUDE, 0, NOW + DAY);
    assert(!pet_rewrite_ready(&g2));
    feed(&g2, &home, FAM_CLAUDE, FINAL_XP - 1, NOW + DAY + 1);
    assert(!pet_rewrite_ready(&g2));
    feed(&g2, &home, FAM_CLAUDE, 1, NOW + DAY + 2);
    assert(pet_rewrite_ready(&g2));

    /* The next rewrite: generation 3, new DNA, the trait carried from life 2's final form. */
    assert(pet_rewrite_preview(&g2, 1, NOW + 2 * DAY, &g3));
    assert(g3.generation == 3 && g3.dna != g2.dna && g3.dna != g1.dna && g3.dna != 0);
    assert(g3.inherited_trait == 1 && g3.inherited_value == g2.inherited_value); /* life 2 kept it */
    assert(pet_rewrite_preview(&g2, 6, NOW + 2 * DAY, &g3b));
    assert(g3b.dna == g3.dna && g3b.inherited_value == trait_of(&g2, PET_FORMS - 1, 6));
    assert(g3.xp == g2.xp && g3.xp == 2 * FINAL_XP && g3.life_xp == 0 && g3.life_born == NOW + 2 * DAY);
    check_lineage(&g3b);

    /* It persists, and its forms survive a reload. */
    Pet q;
    feed(&g3b, &home, FAM_GPT, 0, NOW + 2 * DAY);
    feed(&g3b, &home, FAM_GPT, 3000000, NOW + 2 * DAY + 1);
    assert(g3b.stage == STAGE_ADULT && pet_forms_available(&g3b) == 3);
    assert(pet_save(&g3b, path) && pet_load(&q, path));
    assert(!pet_save_needed(&g3b, &q) && same_forms(&g3b, &q) && q.generation == 3);
    puts("PASS rewrites repeat once each life reaches its final form");
}

static void save_paths(void) {
    /* A path whose temporary name would not fit is refused, never written beside a truncated name. */
    char dir[64], long_path[800], tmp[800];
    snprintf(dir, sizeof dir, "/tmp/paseo-rebirth-dir-XXXXXX");
    assert(mkdtemp(dir));
    Pet p;
    pet_init(&p, "Path", NOW);
    for (int len = 760; len <= 766; len++) {
        /* dir + "/" + "./" * k + "pet" (+ "/" padding) of exactly len bytes */
        int used = snprintf(long_path, sizeof long_path, "%s/", dir);
        if ((len - used - 3) % 2) used += snprintf(long_path + used, sizeof long_path - used, "/");
        while (used < len - 3) used += snprintf(long_path + used, sizeof long_path - used, "./");
        snprintf(long_path + used, sizeof long_path - used, "pet");
        assert((int)strlen(long_path) == len);
        bool fits = len + 4 < 768;
        assert(pet_save(&p, long_path) == fits);
        assert((access(long_path, F_OK) == 0) == fits);
        snprintf(tmp, sizeof tmp, "%s/pet.t", dir); /* what a truncated temporary name would be */
        assert(access(tmp, F_OK) != 0);
        snprintf(tmp, sizeof tmp, "%s/pet.tm", dir);
        assert(access(tmp, F_OK) != 0);
        snprintf(tmp, sizeof tmp, "%s/pet.", dir);
        assert(access(tmp, F_OK) != 0);
        snprintf(tmp, sizeof tmp, "%s/pet", dir);
        unlink(tmp);
    }
    assert(rmdir(dir) == 0);
    puts("PASS pet_save refuses temporary paths past 768 bytes");
}

int main(void) {
    snprintf(path, sizeof path, "/tmp/paseo-rebirth-test-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    migration();
    load_validation();
    readiness();
    preview();
    save_fields();
    fresh_feeding();
    inheritance();
    reuse();
    save_paths();
    assert(unlink(path) == 0);
    return 0;
}
