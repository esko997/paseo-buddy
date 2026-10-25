#include "morphology.h"

#include <string.h>

#include "theme.h"

/* ---- natural colours ---------------------------------------------------------------------------- */

static uint32_t palette_field(const Palette *pal, int f) {
    return f == 0 ? pal->body : f == 1 ? pal->light : f == 2 ? pal->dark : pal->accent;
}

void morphology_mix(const Pet *p, const Palette table[5], Palette *out) {
    uint64_t w[N_FAMILIES], top = 0;
    for (int i = 0; i < N_FAMILIES; i++) {
        w[i] = p->diet[i] > 0 ? (uint64_t)p->diet[i] : 0;
        if (w[i] > top) top = w[i];
    }
    if (!top) {
        *out = table[PAL_UNFED];
        return;
    }
    /* Scale every weight alike until each fits 32 bits: sums and products below stay far from overflow
     * and equal proportions give equal colours however large the lifetime totals get. */
    while (top >> 32) {
        for (int i = 0; i < N_FAMILIES; i++) w[i] >>= 1;
        top >>= 1;
    }
    uint64_t sum = 0;
    for (int i = 0; i < N_FAMILIES; i++) sum += w[i];
    uint32_t mixed[4];
    for (int f = 0; f < 4; f++) {
        uint32_t col = 0;
        for (int shift = 0; shift <= 16; shift += 8) {
            uint64_t acc = sum / 2; /* round to nearest */
            for (int i = 0; i < N_FAMILIES; i++) acc += w[i] * ((palette_field(&table[i], f) >> shift) & 255);
            col |= (uint32_t)(acc / sum) << shift;
        }
        mixed[f] = col;
    }
    *out = (Palette){mixed[0], mixed[1], mixed[2], mixed[3]};
}

void morphology_palette(const Pet *p, Palette *out) { morphology_mix(p, PALETTES, out); }

/* Body rows span about 4..14; row 9 keeps the base colour, the ends move up to ~30% toward light/dark. */
#define GRADIENT_MID 9
#define GRADIENT_HALF 5
#define GRADIENT_MAX 80 /* of 256 */

void morphology_gradient(const Palette *base, Palette rows[16]) {
    for (int r = 0; r < 16; r++) {
        int k = r - GRADIENT_MID;
        if (k < -GRADIENT_HALF) k = -GRADIENT_HALF;
        if (k > GRADIENT_HALF) k = GRADIENT_HALF;
        rows[r] = *base;
        if (k < 0) rows[r].body = lerp_col(base->body, base->light, -k * GRADIENT_MAX / GRADIENT_HALF);
        else if (k > 0) rows[r].body = lerp_col(base->body, base->dark, k * GRADIENT_MAX / GRADIENT_HALF);
    }
}

/* ---- generated forms ---------------------------------------------------------------------------- */

/* Bodies (round, square, pear). Every one keeps SPR_KID's anchors: head top on row 4 for hats, eyes
 * at rows 7-8 columns 5 and 10, mouth row 9 columns 7-8, cheeks row 9 columns 3 and 12, the neck at
 * rows 10-13 and room on the right for held items. */
static const Sprite BODIES[3] = {
    {"................", "................", "................", "................",
     "....oooooooo....",
     "..oobllbbbbbdo..",
     ".obllbbbbbbbbdo.",
     ".obbbEbbbbEbbdo.",
     ".obbbEbbbbEbbdo.",
     ".obcbbbmmbbbcdo.",
     ".obbbbbbbbbbbdo.",
     ".obbbbbbbbbbbdo.",
     "..obbbbbbbbbdo..",
     "...obbbbbbddo...",
     "....oooooooo....",
     "................"},
    {"................", "................", "................", "................",
     "..oooooooooooo..",
     "..olllbbbbbbdo..",
     "..olbbbbbbbbdo..",
     "..obbEbbbbEbdo..",
     "..obbEbbbbEbdo..",
     "..ocbbbmmbbbco..",
     "..obbbbbbbbbdo..",
     "..obbbbbbbbbdo..",
     "..obbbbbbbbbdo..",
     "..oooooooooooo..",
     "...oo......oo...",
     "................"},
    {"................", "................", "................", "................",
     ".....oooooo.....",
     "...oobllbbdoo...",
     "..obllbbbbbbdo..",
     "..obbEbbbbEbdo..",
     "..obbEbbbbEbdo..",
     "..ocbbbmmbbbco..",
     ".obbbbbbbbbbbdo.",
     ".obbbbbbbbbbbdo.",
     "obbbbbbbbbbbbddo",
     "obbbbbbbbbbbbddo",
     ".oooooooooooooo.",
     "................"},
};

/* Left ears (rounded, pointed, drooping), 4 wide at columns 2..5, mirrored for the right. Drawn
 * behind the body from row EAR_Y[body]; their last row tucks under the head outline. */
static const char *const EARS[3][5] = {
    {"....", "....", ".oo.", "oaao", "obbo"},
    {"o...", "oo..", "oao.", "oaao", "obbo"},
    {"....", "....", "..oo", ".oao", "oaao"},
};
#define EAR_X 2
static const int EAR_Y[3] = {0, 0, 1}; /* the pear's narrow head sits its ears on the shoulders */

/* Crowns (antenna, horns, sprout) on rows 0..3, behind the body, hidden by hats. */
static const char *const CROWNS[3][4] = {
    {".......oo.......", "......oaao......", ".......oo.......", ".......oo......."},
    {".....o....o.....", ".....ao..oa.....", ".....oa..ao.....", "......o..o......"},
    {".....oo..oo.....", "....oaaooaao....", ".....oaaaao.....", ".......oo......."},
};

/* Left wings (small, feather, fin), 3 wide at columns 0..2 on rows 5..9, mirrored, over the body. */
static const char *const WINGS[3][5] = {
    {"...", "...", "oo.", "oao", ".oo"},
    {"o..", "ao.", "aao", "aao", "ooo"},
    {"...", ".oo", "oao", "oa.", "o.."},
};

/* Tails (curl, fin, tuft), 5 wide at columns 11..15 on rows 10..15, over the lower right flank. */
static const char *const TAILS[3][6] = {
    {"..ooo", ".oaao", ".o.oa", "..oao", ".oao.", "..o.."},
    {"....o", "...oa", "..oaa", ".oaao", "..oao", "...oo"},
    {"..oaa", "..aaa", "...aa", "...o.", "..o..", "....."},
};

/* Freckles: shade dots on the sides of the head and over the body. */
static const int8_t FRECKLES[][2] = {{6, 2}, {8, 2}, {6, 13}, {11, 4}, {12, 6}, {11, 9}, {12, 11}, {13, 8}};

/* Eyes, mouth and cheeks plus the skin around them: no marking, wing or tail ever lands here. */
static bool face_zone(int r, int c) { return r >= 5 && r <= 10 && c >= 3 && c <= 12; }

/* Writes ch at (r,c): '.' never; front layers skip the face; `over` (if set) lists the codes it may
 * replace. */
static void put(MorphGrid g, int r, int c, char ch, bool front, const char *over) {
    if (r < 0 || r >= 16 || c < 0 || c >= 16 || ch == '.') return;
    if (front && face_zone(r, c)) return;
    if (over && !strchr(over, g[r][c])) return;
    g[r][c] = ch;
}

static void clear(MorphGrid g) {
    for (int r = 0; r < 16; r++) {
        memset(g[r], '.', 16);
        g[r][16] = '\0';
    }
}

static void overlay(MorphGrid g, const Sprite spr) {
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++)
            if (spr[r][c] != '.') g[r][c] = spr[r][c];
}

static int trait(uint8_t v) { return v < 3 ? v : 0; }

int morphology_compose(PetTraits t, bool hat, MorphGrid g) {
    int body = trait(t.body), ears = trait(t.ears), tail = trait(t.tail), crown = trait(t.crown);
    int wings = trait(t.wings), marks = trait(t.markings);
    clear(g);
    /* behind the body: ears (only their lower part under a hat) and the crown */
    for (int i = 0; i < 5; i++) {
        int r = EAR_Y[body] + i;
        if (hat && r < 3) continue;
        for (int j = 0; j < 4; j++) {
            put(g, r, EAR_X + j, EARS[ears][i][j], false, NULL);
            put(g, r, 15 - EAR_X - j, EARS[ears][i][j], false, NULL);
        }
    }
    if (!hat)
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 16; c++) put(g, r, c, CROWNS[crown][r][c], false, NULL);
    overlay(g, BODIES[body]);
    /* markings recolour body pixels only, never the face */
    if (marks == 0) {
        for (size_t i = 0; i < sizeof FRECKLES / sizeof FRECKLES[0]; i++)
            put(g, FRECKLES[i][0], FRECKLES[i][1], 'd', true, "bl");
    } else if (marks == 1) { /* pale belly patch */
        for (int r = 11; r <= 13; r++)
            for (int c = 5; c <= 10; c++)
                if (r < 13 || (c > 5 && c < 10)) put(g, r, c, 'l', true, "bd");
    } else { /* two bands around the body */
        for (int c = 0; c < 16; c++) {
            put(g, 11, c, 'd', true, "bl");
            put(g, 13, c, 'd', true, "bl");
        }
    }
    /* over the body: tail on the lower right flank, wings at the shoulders */
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 5; j++) put(g, 10 + i, 11 + j, TAILS[tail][i][j], true, NULL);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 3; j++) {
            put(g, 5 + i, j, WINGS[wings][i][j], true, NULL);
            put(g, 5 + i, 15 - j, WINGS[wings][i][j], true, NULL);
        }
    return trait(t.motion);
}

int morphology_shape(const Pet *p, int form, bool hat, MorphGrid g) {
    if (form >= 1 && form < PET_FORMS) return morphology_compose(pet_form_traits(p, form), hat, g);
    clear(g);
    switch (p->stage) {
    case STAGE_EGG: overlay(g, SPR_EGG); break;
    case STAGE_BABY: overlay(g, SPR_BABY); break;
    default:
        overlay(g, SPR_KID);
        /* A hat sits where the antenna/crest would poke out; the crest comes back when it's off. */
        if (p->stage == STAGE_TEEN && !hat) overlay(g, OVL_ANTENNA);
        if (p->stage == STAGE_ADULT) {
            if (!hat) overlay(g, OVL_CREST[(unsigned)p->adult_family < N_FAMILIES ? p->adult_family : FAM_OTHER]);
            overlay(g, OVL_WINGS);
        }
        break;
    }
    return MORPH_BOUNCE;
}

/* ---- motion ------------------------------------------------------------------------------------- */

void morphology_motion(int motion, Mood mood, int64_t now_ms, int s, int *dx, int *dy) {
    int tick = (int)(now_ms / 125); /* the pet's 8 Hz animation clock */
    *dx = *dy = 0;
    if (mood == MOOD_SLEEP) return;
    if (mood == MOOD_ALERT) { /* every pet hops for attention */
        *dy = (tick % 2) ? -s : 0;
        return;
    }
    switch (motion) {
    case MORPH_FLOAT: { /* a slow glide up and back down over two seconds */
        int t = tick % 16, tri = t < 8 ? t : 16 - t;
        *dy = -(tri * s + 4) / 8;
        break;
    }
    case MORPH_WADDLE: /* sways side to side, lifting a little at each step */
        *dx = ((tick / 3) % 2 ? 1 : -1) * (s / 2);
        if (tick % 3 == 0) *dy = -(s + 2) / 4;
        break;
    default:
        *dy = ((tick / 4) % 2) ? -s / 2 : 0;
        break;
    }
}
