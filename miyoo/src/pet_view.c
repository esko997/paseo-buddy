#include "pet_view.h"

#include <stdio.h>
#include <string.h>

#include "appearance.h"
#include "cosmetics.h"
#include "evolution_view.h"
#include "morphology.h"
#include "sprites.h"
#include "theme.h"

#define SILHOUETTE 0x2e3344 /* locked items: a dark shape to look forward to */
#define RUG_SWATCH 0x5a3c6e /* rug previews, close to the den rug */
#define BUBBLE_MS 4000

/* ---- drawing the pet ------------------------------------------------------------------------ */

/* The form the pet wears: generated forms show only on adults, earlier stages keep the classic art. */
static int worn_form(const Pet *p) { return p->stage == STAGE_ADULT && p->form < PET_FORMS ? p->form : 0; }

/* Colours per sprite row for coat id (or -1). A new coat's own palette, else the old shiny coat
 * blended by diet like the natural colours, else the natural diet blend with its vertical gradient.
 * Coats are flat: they override the natural palette whatever the pet ate. */
static void pet_rows(const Pet *p, int coat, Palette rows[16]) {
    const Palette *fixed = appearance_coat_palette(coat);
    Palette base;
    if (fixed) {
        base = *fixed;
    } else if (coat == U_SHINY) {
        morphology_mix(p, SHINY_PALETTES, &base);
    } else {
        morphology_palette(p, &base);
        morphology_gradient(&base, rows);
        return;
    }
    for (int r = 0; r < 16; r++) rows[r] = base;
}

static void blit_shape(Canvas *c, MorphGrid g, int x, int y, int s, const Palette rows[16], Mood mood, bool blink,
                       bool chomp) {
    for (int r = 0; r < 16; r++) {
        const Palette *pal = &rows[r];
        for (int col = 0; col < 16; col++) {
            char ch = g[r][col];
            uint32_t color;
            switch (ch) {
            case 'o': color = COL_OUTLINE; break;
            case 'b': color = pal->body; break;
            case 'l': color = pal->light; break;
            case 'd': color = pal->dark; break;
            case 'a': color = pal->accent; break;
            case 'w': color = COL_SHELL; break;
            case 'c': color = COL_CHEEK; break;
            case 'm': color = (chomp || mood == MOOD_SLEEP) ? pal->body : COL_MOUTH; break;
            case 'E': {
                /* Eyes are two pixels tall; the upper one decides open/closed/looking up. */
                bool upper = r + 1 < 16 && g[r + 1][col] == 'E';
                bool closed = blink || mood == MOOD_SLEEP || mood == MOOD_HAPPY;
                if (upper) color = closed ? pal->body : (mood == MOOD_ALERT ? COL_SHELL : COL_OUTLINE);
                else color = COL_OUTLINE;
                break;
            }
            default: continue;
            }
            gfx_rect(c, x + col * s, y + r * s, s, s, color);
        }
    }
}

static void blit_silhouette(Canvas *c, MorphGrid g, int x, int y, int s, uint32_t color) {
    for (int r = 0; r < 16; r++)
        for (int col = 0; col < 16; col++)
            if (g[r][col] != '.') gfx_rect(c, x + col * s, y + r * s, s, s, color);
}

/* A cosmetic layer shifted by (dx,dy) sprite pixels. silhouette != 0 draws every pixel in that color
 * (twinkles skipped); phase picks which twinkle set ('*' or '+') is lit. */
static void blit_item(Canvas *c, const Cosmetic *k, int x, int y, int s, int dx, int dy, uint32_t silhouette,
                      int phase) {
    for (int r = 0; r < 16; r++) {
        for (int col = 0; col < 16; col++) {
            char ch = k->art[r][col];
            uint32_t color;
            switch (ch) {
            case 'o': color = COL_OUTLINE; break;
            case 'w': color = COL_SHELL; break;
            case '1': color = k->ink[0]; break;
            case '2': color = k->ink[1]; break;
            case '3': color = k->ink[2]; break;
            case '*':
            case '+':
                if (silhouette || (ch == '*') != (phase % 2 == 0)) continue;
                color = COL_SHELL;
                break;
            default: continue;
            }
            if (silhouette) color = silhouette;
            gfx_rect(c, x + (col + dx) * s, y + (r + dy) * s, s, s, color);
        }
    }
}

/* Worn layers are drawn for SPR_KID (and the generated forms, which share its anchors); the baby is
 * narrower and sits a row lower. */
static void wear_nudge(bool baby, int slot, int *dx, int *dy) {
    *dx = *dy = 0;
    if (!baby) return;
    *dy = 1;
    if (slot == WEAR_HELD) *dx = -1;
}

/* p drawn as `form` (morphology_shape) with its worn cosmetics and colours. `ghost` (an unlock id or
 * -1) is drawn as a silhouette in its slot: the wardrobe's locked preview. */
static void draw_form(Canvas *c, const Pet *p, int form, Mood mood, int x, int y, int s, int64_t now_ms, int ghost) {
    Palette rows[16];
    pet_rows(p, pet_worn(p, WEAR_SHINY), rows);
    int tick = (int)(now_ms / 125); /* 8 Hz animation clock */
    bool blink = (now_ms / 100) % 37 == 0;
    bool chomp = mood == MOOD_EATING && (tick / 2) % 2;
    bool hat = pet_worn(p, WEAR_HAT) >= 0 || pet_unlock_slot(ghost) == WEAR_HAT;
    bool generated = form >= 1 && form < PET_FORMS;
    bool egg = !generated && p->stage == STAGE_EGG, baby = !generated && p->stage == STAGE_BABY;
    MorphGrid g;
    int motion = morphology_shape(p, form, hat, g);
    int dx = 0, dy = 0;
    if (egg) {
        if (mood == MOOD_ALERT || (tick / 4) % 6 == 0) dx = ((tick / 2) % 2 ? 1 : -1) * (s / 2);
    } else {
        morphology_motion(motion, mood, now_ms, s, &dx, &dy);
    }
    int px = x + dx, py = y + dy;
    blit_shape(c, g, px, py, s, rows, mood, !egg && blink, !egg && chomp);
    if (!egg) { /* eggs keep their outfit for later */
        int phase = tick / 3;
        for (int slot = WEAR_HAT; slot <= WEAR_HELD; slot++) {
            int id = pet_worn(p, slot), ox, oy;
            bool ghosted = pet_unlock_slot(ghost) == slot;
            if (ghosted) id = ghost;
            if (id < 0) continue;
            wear_nudge(baby, slot, &ox, &oy);
            blit_item(c, &COSMETICS[id], px, py, s, ox, oy, ghosted ? SILHOUETTE : 0, phase);
        }
        /* only the old shiny coat sparkles; the newer coats are plain recolors */
        if (pet_worn(p, WEAR_SHINY) == U_SHINY && (tick / 4) % 3 != 2) blit_item(c, &SHINE, px, py, s, 0, 0, 0, tick / 4);
    }
    int right = x + 16 * s;
    switch (mood) {
    case MOOD_SLEEP:
        gfx_text(c, FONT_BODY, right - 2 * s, y + 2 * s - (tick / 4) % 3 * 3, DIM, "z", 0);
        gfx_text(c, FONT_BIG, right - s, y - (tick / 4) % 3 * 3, DIM, "Z", 0);
        break;
    case MOOD_ALERT:
        if ((tick / 2) % 2) gfx_text(c, FONT_BIG, right - 2 * s, y, ALERT_COL, "!", 0);
        break;
    case MOOD_WORKING: {
        char dots[4] = "   ";
        for (int i = 0; i <= tick % 4 && i < 3; i++) dots[i] = '.';
        gfx_text(c, FONT_BIG, right - 3 * s, y, rows[0].accent, dots, 0);
        break;
    }
    case MOOD_HAPPY:
        if ((tick / 8) % 2) gfx_text(c, FONT_BODY, right - 3 * s, y, COL_CHEEK, "<3", 0);
        break;
    default:
        break;
    }
}

/* The pet as it is: its worn form. */
static void draw_pet(Canvas *c, const Pet *p, Mood mood, int x, int y, int s, int64_t now_ms, int ghost) {
    draw_form(c, p, worn_form(p), mood, x, y, s, now_ms, ghost);
}

void ui_draw_pet(Canvas *c, const App *app, int x, int y, int s, int64_t now_ms) {
    const Pet *p = &app->pet;
    Mood mood = pet_mood(p, app_now(app), app->tz_offset, app->n_alerts, app->n_running);
    draw_pet(c, p, mood, x, y, s, now_ms, -1);
}

void pet_draw_form_preview(Canvas *c, const Pet *p, int form, int x, int y, int scale, int64_t now_ms) {
    draw_form(c, p, form >= 1 && form < PET_FORMS ? form : 0, MOOD_IDLE, x, y, scale, now_ms, -1);
}

/* ---- item art outside the pet: icons, shelf ornaments, rug motifs ---------------------------- */

typedef struct {
    int x0, y0, x1, y1; /* inclusive bounding box in sprite pixels; x0 > x1 when empty */
} Box;

static Box art_box(const Cosmetic *k) {
    Box b = {16, 16, -1, -1};
    for (int r = 0; r < 16; r++)
        for (int col = 0; col < 16; col++) {
            if (k->art[r][col] == '.') continue;
            if (col < b.x0) b.x0 = col;
            if (col > b.x1) b.x1 = col;
            if (r < b.y0) b.y0 = r;
            if (r > b.y1) b.y1 = r;
        }
    return b;
}

/* Den animation for desk ornaments: the lava lamp's blobs drift, the pumpkin's candle flickers. */
static Cosmetic animated(int id, int64_t now_ms) {
    Cosmetic k = COSMETICS[id];
    if (id == U_PUMPKIN) {
        static const uint32_t FLICKER[4] = {0xffe27a, 0xffd04a, 0xfff2b0, 0xffc838};
        k.ink[2] = FLICKER[(now_ms / 180 + (now_ms / 470) % 3) % 4];
    } else if (id == U_LAVA_LAMP) {
        static char rows[16][17];
        for (int r = 0; r < 16; r++) {
            memcpy(rows[r], COSMETICS[id].art[r], 17);
            for (int col = 0; col < 16; col++)
                if (rows[r][col] == '2') rows[r][col] = '1';
            k.art[r] = rows[r];
        }
        /* two blobs bobbing out of phase inside the glass (rows 3..10) */
        int t = (int)(now_ms / 260);
        int a = 3 + (t % 14 < 7 ? t % 7 : 6 - t % 7), b = 10 - (t / 2 % 14 < 7 ? t / 2 % 7 : 6 - t / 2 % 7);
        rows[a][7] = rows[a][8] = '2';
        rows[b][6] = '2';
        if (b + 1 <= 10) rows[b + 1][6] = '2';
    }
    return k;
}

/* Any item centered in a w x h box, at most max_s pixels per art pixel (generic cosmetics only). Walls,
 * windows and robot costumes draw their own art; coats show the pet's own shape (its worn form, or the
 * kid it will hatch into) in that coat. */
static void draw_icon(Canvas *c, const Pet *p, int id, int x, int y, int w, int h, bool locked, int max_s,
                      int64_t now_ms) {
    int tick = (int)(now_ms / 125);
    if (appearance_draw_preview(c, id, x, y, w, h, locked, now_ms)) return;
    if (pet_unlock_slot(id) == WEAR_SHINY) {
        int s = (w < h ? w : h) / 16, px = x + (w - 16 * s) / 2, py = y + (h - 16 * s) / 2;
        MorphGrid g;
        if (p->stage == STAGE_EGG)
            for (int r = 0; r < 16; r++) memcpy(g[r], SPR_KID[r], 17);
        else
            morphology_shape(p, worn_form(p), false, g);
        if (locked) {
            blit_silhouette(c, g, px, py, s, SILHOUETTE);
            return;
        }
        Palette rows[16];
        pet_rows(p, id, rows);
        blit_shape(c, g, px, py, s, rows, MOOD_IDLE, false, false);
        if (id == U_SHINY) blit_item(c, &SHINE, px, py, s, 0, 0, 0, tick / 4);
        return;
    }
    Cosmetic k = locked ? COSMETICS[id] : animated(id, now_ms);
    Box b = art_box(&k);
    if (b.x1 < b.x0) return;
    int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
    int s = w / bw < h / bh ? w / bw : h / bh;
    if (s > max_s) s = max_s;
    if (s < 1) s = 1;
    int ox = x + (w - bw * s) / 2 - b.x0 * s, oy = y + (h - bh * s) / 2 - b.y0 * s;
    if (pet_unlock_slot(id) == WEAR_ROOM && !locked) gfx_rect(c, x + 2, y + (h - bh * s) / 2 - 6, w - 4, bh * s + 12, RUG_SWATCH);
    blit_item(c, &k, ox, oy, s, 0, 0, locked ? SILHOUETTE : 0, tick / 3);
}

/* A wall shelf (plank, lip, two brackets, soft shadow) w wide at y with an ornament standing on it. */
static void draw_shelf(Canvas *c, int id, int x, int w, int y, int s, uint32_t silhouette, int64_t now_ms) {
    gfx_rect(c, x + s, y + 3 * s, w - s, s + 1, 0x000000);
    gfx_rect(c, x, y, w, 3 * s, 0x8a5a3a);
    gfx_rect(c, x, y, w, s, 0xb07a50);
    gfx_rect(c, x + 2 * s, y + 3 * s, 2 * s, 4 * s, 0x6a4028);
    gfx_rect(c, x + w - 4 * s, y + 3 * s, 2 * s, 4 * s, 0x6a4028);
    Cosmetic k = silhouette ? COSMETICS[id] : animated(id, now_ms);
    Box b = art_box(&k);
    int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
    blit_item(c, &k, x + (w - bw * s) / 2 - b.x0 * s, y - bh * s - b.y0 * s, s, 0, 0, silhouette, (int)(now_ms / 375));
}

/* A rug's motif tiled in staggered rows over [x0,x1) x [y0,y1). */
static void draw_rug(Canvas *c, int id, int x0, int y0, int x1, int y1, int s, int rows, uint32_t silhouette) {
    const Cosmetic *k = &COSMETICS[id];
    Box b = art_box(k);
    int mw = (b.x1 - b.x0 + 1) * s, mh = (b.y1 - b.y0 + 1) * s, gap = mw * 3, row_h = (y1 - y0) / rows;
    for (int r = 0; r < rows; r++) {
        int y = y0 + r * row_h + (row_h - mh) / 2;
        for (int x = x0 + 6 + (r % 2) * gap / 2; x + mw <= x1 - 4; x += gap)
            blit_item(c, k, x - b.x0 * s, y - b.y0 * s, s, 0, 0, silhouette, 0);
    }
}

/* Den ornament spots agreed with scene.c's layout: a little wall shelf at the right edge
 * (x 590-634, y 48-152) and the rug's inner field (x 112-528, y 390-440). */
#define SHELF_X 592
#define SHELF_W 40
#define SHELF_Y 128 /* plank 128-137, brackets to 149 */
#define RUG_X0 112
#define RUG_X1 528
#define RUG_Y0 390
#define RUG_Y1 440

void pet_draw_den_decor(Canvas *c, const App *app, int64_t now_ms) {
    if (!app->pet_ready) return;
    int rug = pet_worn(&app->pet, WEAR_ROOM), orn = pet_worn(&app->pet, WEAR_DESK);
    if (rug >= 0) draw_rug(c, rug, RUG_X0, RUG_Y0, RUG_X1, RUG_Y1, 2, 2, 0);
    if (orn >= 0) draw_shelf(c, orn, SHELF_X, SHELF_W, SHELF_Y, 3, 0, now_ms);
}

/* ---- Pet screen: stats page ------------------------------------------------------------------ */

static void stat_line(Canvas *c, int x, int y, const char *label, const char *value) {
    gfx_text(c, FONT_BODY, x, y, DIM, label, 0);
    gfx_text(c, FONT_BODY, x + 120, y, TEXT, value, 0);
}

double pet_xp_progress(const Pet *p, char *buf, size_t len) {
    int available = pet_forms_available(p);
    int64_t next = p->stage == STAGE_ADULT ? pet_form_threshold(available) : stage_threshold(p->stage);
    char have[16], need[16];
    format_tokens(p->xp, have, sizeof have);
    if (!next) {
        snprintf(buf, len, "%s tok", have);
        return 1;
    }
    int64_t prev = p->stage == STAGE_EGG ? 0 : stage_threshold((Stage)(p->stage - 1));
    if (p->stage == STAGE_ADULT) prev = pet_form_threshold(available - 1);
    format_tokens(next, need, sizeof need);
    snprintf(buf, len, "%s/%s", have, need);
    return (double)(p->xp - prev) / (double)(next - prev);
}

const char *pet_mood_line(const App *app, int64_t now) {
    static char buf[48];
    switch (pet_mood(&app->pet, now, app->tz_offset, app->n_alerts, app->n_running)) {
    case MOOD_ALERT: return app->n_alerts == 1 ? "An agent wants you!" : "Agents want you!";
    case MOOD_WORKING:
        snprintf(buf, sizeof buf, "Watching %d agent%s work...", app->n_running, app->n_running == 1 ? "" : "s");
        return buf;
    case MOOD_EATING: return "Nom nom nom";
    case MOOD_SLEEP: return "Zzz...";
    case MOOD_HAPPY: return "Yay! Good work today.";
    default: return app->pet.stage == STAGE_EGG ? "An egg. It's warm." : "Hanging out.";
    }
}

static int count_found(const Pet *p) {
    int n = 0;
    for (int id = 0; id < N_UNLOCKS; id++) n += pet_has(p, id);
    return n;
}

static const int SCREEN_SCALE[N_STAGES] = {9, 9, 10, 11, 12};

static void draw_stats(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet;
    int64_t now = app_now(app);
    int s = SCREEN_SCALE[p->stage], size = 16 * s;
    ui_draw_pet(c, app, 24 + (204 - size) / 2, HEADER_H + 20 + (204 - size), s, now_ms);

    int x = 250, y = HEADER_H + 14;
    char line[96];
    gfx_text(c, FONT_BIG, x, y, TEXT, p->name, 0);
    y += 36;
    int64_t days = (now - p->born) / 86400;
    if (days < 1) snprintf(line, sizeof line, "%s, born today", stage_name(p->stage));
    else snprintf(line, sizeof line, "%s, %lld day%s old", stage_name(p->stage), (long long)days, days == 1 ? "" : "s");
    gfx_text(c, FONT_BODY, x, y, DIM, line, 0);
    y += 24;
    const char *say = now_ms < app->bubble_until_ms ? app->bubble : pet_mood_line(app, now);
    gfx_text_wrap(c, FONT_BODY, x, y, SCREEN_W - x - 12, 2, SAY_COL, say);
    y += 52;
    char xp[40], val[48], tok[16];
    double frac = pet_xp_progress(p, xp, sizeof xp);
    gfx_text(c, FONT_BODY, x, y, DIM, "GROW", 0);
    gfx_bar(c, x + 50, y + 5, 150, 10, frac, FAINT, GROW_COL);
    gfx_text(c, FONT_BODY, x + 210, y, DIM, xp, 0);
    bool today = p->day == (app_now(app) + app->tz_offset) / 86400;
    format_tokens(today ? p->today_tokens : 0, tok, sizeof tok);
    int64_t turns = today ? p->today_turns : 0;
    snprintf(val, sizeof val, "%s tok, %lld turn%s", tok, (long long)turns, turns == 1 ? "" : "s");
    stat_line(c, x, y + 28, "Ate today", val);
    format_tokens(p->best_day_tokens, tok, sizeof tok);
    stat_line(c, x, y + 52, "Best day", tok);
    snprintf(val, sizeof val, "%lld", (long long)p->lifetime_turns);
    stat_line(c, x, y + 76, "Turns ever", val);
    snprintf(val, sizeof val, "%d of %d found", count_found(p), N_UNLOCKS);
    stat_line(c, x, y + 100, "Wardrobe", val);

    y = HEADER_H + 250;
    gfx_text(c, FONT_BODY, 24, y, TEXT, "DIET (output tokens eaten)", 0);
    y += 28;
    int64_t most = 1;
    for (int i = 0; i < N_FAMILIES; i++)
        if (p->diet[i] > most) most = p->diet[i];
    for (int i = 0; i < N_FAMILIES; i++, y += 26) {
        format_tokens(p->diet[i], tok, sizeof tok);
        gfx_text(c, FONT_BODY, 24, y, DIM, family_name((Family)i), 0);
        gfx_bar(c, 110, y + 5, 360, 10, (double)p->diet[i] / (double)most, FAINT, PALETTES[i].body);
        gfx_text(c, FONT_BODY, 484, y, TEXT, tok, 0);
    }
    int forms = pet_forms_available(p);
    if (forms - 1 > p->forms_seen)
        snprintf(line, sizeof line, "NEW: adult forms ready! A to preview; keep or transform.");
    else if (p->stage == STAGE_ADULT)
        snprintf(line, sizeof line, "A: forms (%d/%d)  Natural colours follow its token diet.", forms, PET_FORMS);
    else
        snprintf(line, sizeof line, "A: preview adult forms. Natural colours follow its diet.");
    gfx_text_fit(c, FONT_BODY, 24, y + 6, forms - 1 > p->forms_seen ? SAY_COL : FAINT, line, SCREEN_W - 48);
}

/* ---- Pet screen: wardrobe -------------------------------------------------------------------- */

#define PAGE_ROWS 5 /* wardrobe rows per page; L2/R2 flip pages */

/* Ids in a slot, in catalog order; ids must hold N_UNLOCKS. */
static int slot_items(int slot, int *ids) {
    int n = 0;
    for (int id = 0; id < N_UNLOCKS; id++)
        if (pet_unlock_slot(id) == slot) ids[n++] = id;
    return n;
}

static int clamp_sel(int sel, int n) { return sel < 0 || n <= 0 ? 0 : sel < n ? sel : n - 1; }

static int wardrobe_id(const App *app) {
    int ids[N_UNLOCKS], n = slot_items(app->wardrobe_slot, ids);
    return n ? ids[clamp_sel(app->wardrobe_sel, n)] : -1;
}

/* The collection book is a second page of the open wardrobe (Y flips between them). UI-only state,
 * so it lives here rather than in App; reset whenever the wardrobe opens. */
static bool book_open;
static int book_set;

/* Robot costume slot -> the family whose robots wear it, -1 for other slots. */
static int robot_family(int slot) {
    for (int f = 0; f < N_FAMILIES; f++)
        if (pet_robot_slot((Family)f) == slot) return f;
    return -1;
}

/* Tabs scroll: TAB_SHOWN of the N_WEAR slots at once between two arrows, with a dot per slot below. */
#define TAB_Y (HEADER_H + 4)
#define TAB_H 28
#define TAB_SHOWN 5
#define TAB_ARROW 22
#define TAB_X0 (12 + TAB_ARROW)
#define TAB_W ((SCREEN_W - 2 * 12 - 2 * TAB_ARROW) / TAB_SHOWN)
#define DOTS_Y (TAB_Y + TAB_H + 3)
#define LIST_X 236
#define LIST_Y (HEADER_H + 46)
#define ROW_H 62
#define ICON 52

static void draw_preview(Canvas *c, const App *app, int id, int64_t now_ms) {
    const Pet *p = &app->pet;
    int slot = pet_unlock_slot(id);
    bool owned = pet_has(p, id);
    int bx = 14, by = LIST_Y, bw = 206, bh = 236;
    gfx_rect(c, bx, by, bw, bh, PANEL);
    if (slot == WEAR_DESK || slot == WEAR_ROOM) {
        /* den items: a big look at the ornament / rug, the pet isn't wearing these */
        uint32_t sil = owned ? 0 : SILHOUETTE;
        gfx_rect(c, bx, by + bh - 40, bw, 40, 0x2a2140);
        if (slot == WEAR_DESK) {
            draw_shelf(c, id, bx + 33, bw - 66, by + 140, 6, sil, now_ms);
        } else {
            gfx_rect(c, bx + 14, by + 50, bw - 28, 110, owned ? RUG_SWATCH : BG);
            gfx_frame(c, bx + 14, by + 50, bw - 28, 110, 4, owned ? 0x7a5a8e : FAINT);
            draw_rug(c, id, bx + 22, by + 58, bx + bw - 22, by + 152, 3, 3, sil);
        }
        gfx_text(c, FONT_BODY, bx + 12, by + bh - 30, DIM, slot == WEAR_DESK ? "On the den shelf" : "On the den rug", 0);
        return;
    }
    if (slot == WEAR_WALL || slot == WEAR_WINDOW || robot_family(slot) >= 0) {
        /* den looks: the wallpaper, window view or robot costume itself, at full size */
        gfx_rect(c, bx, by + bh - 40, bw, 40, 0x2a2140);
        appearance_draw_preview(c, id, bx + 8, by + 8, bw - 16, bh - 56, !owned, now_ms);
        char cap[40];
        if (slot == WEAR_WALL) snprintf(cap, sizeof cap, "On the den wall");
        else if (slot == WEAR_WINDOW) snprintf(cap, sizeof cap, "In the den window");
        else snprintf(cap, sizeof cap, "For %s robots", family_name((Family)robot_family(slot)));
        gfx_text_fit(c, FONT_BODY, bx + 12, by + bh - 30, DIM, cap, bw - 24);
        return;
    }
    Pet look = *p;
    int ghost = -1;
    if (id >= 0 && owned) look.equipped[slot] = (int16_t)id; /* try it on */
    else if (id >= 0 && slot != WEAR_SHINY) ghost = id;     /* a peek at the shape */
    Mood mood = pet_mood(p, app_now(app), app->tz_offset, app->n_alerts, app->n_running);
    int s = 11, size = 16 * s;
    draw_pet(c, &look, mood, bx + (bw - size) / 2, by + 26, s, now_ms, p->stage == STAGE_EGG ? -1 : ghost);
    if (p->stage == STAGE_EGG) gfx_text(c, FONT_BODY, bx + 12, by + bh - 28, DIM, "Worn once it hatches", 0);
}

static int64_t today_of(const App *app) { return (app_now(app) + app->tz_offset) / 86400; }

static int count_finds(void) {
    int n = 0;
    for (int id = 0; id < N_UNLOCKS; id++) n += pet_unlock_is_find(id);
    return n;
}

/* "Next find [bar] 412/1000" on one line, w wide at x,y. */
static void draw_find_bar(Canvas *c, const App *app, int x, int y, int w) {
    const Pet *p = &app->pet;
    int fw = gfx_font_w(FONT_BODY);
    if (pet_finds_left(p) == 0) {
        gfx_text_fit(c, FONT_BODY, x, y, GROW_COL, "Every find discovered!", w);
        return;
    }
    bool ready = pet_find_waiting(p, app_now(app), app->tz_offset);
    int64_t pts = pet_find_progress(p);
    if (pts < 0) pts = 0;
    if (pts >= FIND_POINTS) pts = FIND_POINTS - 1;
    char num[24];
    if (ready) snprintf(num, sizeof num, "Ready");
    else snprintf(num, sizeof num, "%lld/%d", (long long)pts, FIND_POINTS);
    int lw = 10 * fw, nw = (int)strlen(num) * fw;
    gfx_text(c, FONT_BODY, x, y, DIM, "Next find", 0);
    gfx_bar(c, x + lw, y + 5, w - lw - nw - fw, 10, ready ? 1.0 : (double)pts / FIND_POINTS, FAINT, SAY_COL);
    gfx_text(c, FONT_BODY, x + w - nw, y, ready ? SAY_COL : TEXT, num, 0);
}

/* Where the daily find stands: welcome find pending, waiting for tomorrow, today's in, or how many remain. */
static void find_status(const App *app, char *buf, size_t len) {
    const Pet *p = &app->pet;
    int left = pet_finds_left(p), total = count_finds();
    if (left == 0) snprintf(buf, len, "All %d finds are in the book!", total);
    else if (p->find_day < 0) snprintf(buf, len, "First find arrives with the next turn or fresh tokens.");
    else if (pet_find_waiting(p, app_now(app), app->tz_offset)) snprintf(buf, len, "A find is ready! It turns up tomorrow.");
    else if (today_of(app) == p->find_day) snprintf(buf, len, "Today's find is in. %d of %d left to find.", left, total);
    else snprintf(buf, len, "%d of %d finds left. Up to one find a day.", left, total);
}

/* TAB_SHOWN tabs around the open slot; the arrows light up when more slots lie that way, and the dot
 * row marks every slot (open one yellow, ones with something on in purple). */
static void draw_tabs(Canvas *c, const App *app) {
    const Pet *p = &app->pet;
    int fw = gfx_font_w(FONT_BODY), cur = app->wardrobe_slot;
    int shown = N_WEAR < TAB_SHOWN ? N_WEAR : TAB_SHOWN, first = cur - shown / 2;
    if (first > N_WEAR - shown) first = N_WEAR - shown;
    if (first < 0) first = 0;
    gfx_text(c, FONT_BODY, 12 + (TAB_ARROW - fw) / 2, TAB_Y + 3, first > 0 ? TEXT : FAINT, "<", 0);
    gfx_text(c, FONT_BODY, TAB_X0 + TAB_SHOWN * TAB_W + (TAB_ARROW - fw) / 2, TAB_Y + 3,
             first + shown < N_WEAR ? TEXT : FAINT, ">", 0);
    for (int t = first; t < first + shown; t++) {
        int x = TAB_X0 + (t - first) * TAB_W;
        bool on = t == cur;
        gfx_rect(c, x + 2, TAB_Y, TAB_W - 4, TAB_H, on ? PANEL_HI : PANEL);
        if (on) gfx_rect(c, x + 2, TAB_Y + TAB_H - 3, TAB_W - 4, 3, SAY_COL);
        if (pet_worn(p, t) >= 0) gfx_rect(c, x + 2, TAB_Y, TAB_W - 4, 2, GROW_COL); /* something on */
        const char *name = pet_slot_name(t);
        int tw = (int)strlen(name) * fw, room = TAB_W - 8;
        if (tw > room) tw = room;
        gfx_text_fit(c, FONT_BODY, x + (TAB_W - tw) / 2, TAB_Y + 4, on ? TEXT : DIM, name, room);
    }
    int pitch = 10, dx = (SCREEN_W - (N_WEAR * pitch - 4)) / 2;
    for (int t = 0; t < N_WEAR; t++)
        gfx_rect(c, dx + t * pitch, DOTS_Y, 6, 4, t == cur ? SAY_COL : pet_worn(p, t) >= 0 ? GROW_COL : FAINT);
}

/* Worn tag and A-button hints per kind of slot: den things aren't "worn", robots dress up. */
static const char *worn_tag(int slot) {
    if (slot == WEAR_DESK) return "IN THE DEN";
    if (slot == WEAR_ROOM) return "ROLLED OUT";
    if (slot == WEAR_WALL) return "ON THE WALL";
    if (slot == WEAR_WINDOW) return "IN VIEW";
    if (robot_family(slot) >= 0) return "DRESSED UP";
    return "WEARING";
}

static const char *a_hint(int slot, bool worn) {
    if (slot == WEAR_WALL) return worn ? "A: take down" : "A: put up";
    if (slot == WEAR_WINDOW) return worn ? "A: back to sky" : "A: show view";
    if (robot_family(slot) >= 0) return worn ? "A: undress bots" : "A: dress bots";
    return worn ? "A: take off" : "A: put on";
}

static void draw_wardrobe(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet;
    int slot = app->wardrobe_slot, fw = gfx_font_w(FONT_BODY), rw = SCREEN_W - LIST_X - 14;
    draw_tabs(c, app);

    int ids[N_UNLOCKS], n = slot_items(slot, ids);
    int sel = clamp_sel(app->wardrobe_sel, n), first = sel / PAGE_ROWS * PAGE_ROWS;
    draw_preview(c, app, n ? ids[sel] : -1, now_ms);

    for (int i = first; i < n && i < first + PAGE_ROWS; i++) {
        int id = ids[i], y = LIST_Y + (i - first) * ROW_H;
        bool owned = pet_has(p, id), worn = pet_worn(p, slot) == id, on = i == sel;
        gfx_rect(c, LIST_X, y, rw, ROW_H - 6, on ? PANEL_HI : PANEL);
        if (on) gfx_frame(c, LIST_X, y, rw, ROW_H - 6, 2, SAY_COL);
        gfx_rect(c, LIST_X + 4, y + 2, ICON, ICON, BG);
        draw_icon(c, p, id, LIST_X + 6, y + 4, ICON - 4, ICON - 4, !owned, 5, now_ms);
        int tx = LIST_X + ICON + 16, tr = LIST_X + rw - 10, name_r = tr;
        if (worn) {
            const char *tag = worn_tag(slot);
            int tw = (int)strlen(tag) * fw, rx = tr - tw;
            gfx_rect(c, rx - 6, y + 6, tw + 12, 20, GROW_COL);
            gfx_text(c, FONT_BODY, rx, y + 6, BG, tag, 0);
            name_r = rx - 6 - fw; /* the name stops a character short of the tag */
        }
        gfx_text_fit(c, FONT_BODY, tx, y + 6, owned ? TEXT : DIM, owned ? pet_unlock_name(id) : "???", name_r - tx);
        const char *sub = owned && on ? a_hint(slot, worn) : pet_unlock_hint(id);
        gfx_text_fit(c, FONT_BODY, tx, y + 30, owned ? (on ? DIM : FAINT) : DIM, sub, tr - tx);
    }

    char line[64];
    int by = LIST_Y + PAGE_ROWS * ROW_H; /* just under the last row */
    if (n) {
        snprintf(line, sizeof line, "%s %d of %d", pet_slot_name(slot), sel + 1, n);
        gfx_text(c, FONT_BODY, LIST_X, by, DIM, line, 0);
        int pages = (n + PAGE_ROWS - 1) / PAGE_ROWS;
        snprintf(line, sizeof line, "Page %d/%d", first / PAGE_ROWS + 1, pages);
        gfx_text(c, FONT_BODY, LIST_X + rw - (int)strlen(line) * fw, by, pages > 1 ? TEXT : FAINT, line, 0);
    } else {
        gfx_text(c, FONT_BODY, LIST_X + 10, LIST_Y + 10, DIM, "Nothing for this spot yet.", 0);
    }
    draw_find_bar(c, app, LIST_X, by + 24, rw);

    int found = count_found(p);
    snprintf(line, sizeof line, "Found %d of %d", found, N_UNLOCKS);
    int y = LIST_Y + 236 + 14;
    gfx_text(c, FONT_BODY, 14, y, DIM, line, 0);
    gfx_bar(c, 14, y + 26, 206, 8, (double)found / N_UNLOCKS, FAINT, GROW_COL);
    if (now_ms < app->bubble_until_ms) {
        gfx_text_wrap(c, FONT_BODY, 14, y + 44, 206, 3, SAY_COL, app->bubble);
    } else if (n) { /* the selected item's full hint (set items name their set); wraps, last line clips */
        gfx_text_wrap(c, FONT_BODY, 14, y + 44, 206, 3, FAINT, pet_unlock_hint(ids[sel]));
    }
}

/* ---- Pet screen: collection book ------------------------------------------------------------- */

/* BOOK_PAGE cards per page in BOOK_COLS columns; the page shown is the one holding book_set. */
#define BOOK_COLS 2
#define BOOK_PAGE 6
#define BOOK_PAGES ((N_COLLECTION_SETS + BOOK_PAGE - 1) / BOOK_PAGE)
#define BOOK_ROWS ((BOOK_PAGE + BOOK_COLS - 1) / BOOK_COLS)
#define CARD_W 302
#define CARD_H 68
#define CARD_GAP 6
#define CARD_Y (HEADER_H + 32)
#define PIECES_Y (CARD_Y + BOOK_ROWS * (CARD_H + CARD_GAP) + 2)
#define PIECE 38     /* icon box in the selected set's strip */
#define PIECES_MAX 9 /* the biggest set (nine base items); the strip stops there */
#define FIND_Y (PIECES_Y + PIECE + 8)

/* Base items of a set in catalog order (not its reward); ids must hold N_UNLOCKS. */
static int set_members(int set, int *ids) {
    int n = 0, reward = pet_set_reward(set);
    for (int id = 0; id < N_UNLOCKS; id++)
        if (id != reward && pet_unlock_set(id) == set) ids[n++] = id;
    return n;
}

static void draw_set_card(Canvas *c, const Pet *p, int set, int x, int y, bool on, int64_t now_ms) {
    int fw = gfx_font_w(FONT_BODY), have = 0, total = 0, reward = pet_set_reward(set);
    pet_set_progress(p, set, &have, &total);
    bool complete = total > 0 && have >= total, got = pet_has(p, reward);
    gfx_rect(c, x, y, CARD_W, CARD_H, on ? PANEL_HI : PANEL);
    if (on) gfx_frame(c, x, y, CARD_W, CARD_H, 2, SAY_COL);
    gfx_rect(c, x + 6, y + 6, 56, 56, BG);
    if (reward >= 0) draw_icon(c, p, reward, x + 8, y + 8, 52, 52, !got, 4, now_ms); /* the gift, a shadow till earned */
    int tx = x + 72, tr = x + CARD_W - 10;
    char buf[64];
    snprintf(buf, sizeof buf, "%d/%d", have, total);
    int cw = (int)strlen(buf) * fw;
    gfx_text(c, FONT_BODY, tr - cw, y + 4, complete ? GROW_COL : DIM, buf, 0);
    gfx_text_fit(c, FONT_BODY, tx, y + 4, TEXT, pet_set_name(set), tr - cw - fw - tx);
    gfx_bar(c, tx, y + 29, tr - tx, 8, total ? (double)have / total : 0, FAINT, complete ? GROW_COL : SAY_COL);
    snprintf(buf, sizeof buf, "%s %s", got ? "Got:" : "Gift:", reward >= 0 ? pet_unlock_name(reward) : "?");
    gfx_text_fit(c, FONT_BODY, tx, y + 42, got ? GROW_COL : DIM, buf, tr - tx);
}

static void draw_book(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet;
    int fw = gfx_font_w(FONT_BODY), sets_done = 0;
    for (int s = 0; s < N_COLLECTION_SETS; s++) {
        int have = 0, total = 0;
        pet_set_progress(p, s, &have, &total);
        sets_done += total > 0 && have >= total;
    }
    char line[96];
    gfx_text(c, FONT_BODY, 14, HEADER_H + 6, TEXT, "COLLECTION BOOK", 0);
    snprintf(line, sizeof line, "Sets %d/%d  Found %d/%d", sets_done, N_COLLECTION_SETS, count_found(p), N_UNLOCKS);
    int right = SCREEN_W - 14 - (int)strlen(line) * fw;
    gfx_text(c, FONT_BODY, right, HEADER_H + 6, DIM, line, 0);
    int page = book_set / BOOK_PAGE, title_r = 14 + 16 * fw;
    snprintf(line, sizeof line, "Page %d/%d", page + 1, BOOK_PAGES);
    int pw_text = (int)strlen(line) * fw;
    gfx_text(c, FONT_BODY, title_r + (right - title_r - pw_text) / 2, HEADER_H + 6, BOOK_PAGES > 1 ? TEXT : FAINT, line, 0);

    for (int s = page * BOOK_PAGE; s < N_COLLECTION_SETS && s < (page + 1) * BOOK_PAGE; s++) {
        int k = s - page * BOOK_PAGE;
        int x = 14 + k % BOOK_COLS * (CARD_W + 8), y = CARD_Y + k / BOOK_COLS * (CARD_H + CARD_GAP);
        draw_set_card(c, p, s, x, y, s == book_set, now_ms);
    }

    /* The selected set's pieces: found ones in color, missing ones as shadows. */
    int ids[N_UNLOCKS], n = set_members(book_set, ids), have = 0, total = 0;
    for (int i = 0; i < n && i < PIECES_MAX; i++) {
        int x = 14 + i * (PIECE + 4);
        gfx_rect(c, x, PIECES_Y, PIECE, PIECE, PANEL);
        draw_icon(c, p, ids[i], x + 2, PIECES_Y + 2, PIECE - 4, PIECE - 4, !pet_has(p, ids[i]), 3, now_ms);
    }
    pet_set_progress(p, book_set, &have, &total);
    bool complete = total > 0 && have >= total;
    int tx = 14 + PIECES_MAX * (PIECE + 4) + 6, tw = SCREEN_W - 14 - tx;
    snprintf(line, sizeof line, "%s set", pet_set_name(book_set));
    gfx_text_fit(c, FONT_BODY, tx, PIECES_Y, TEXT, line, tw);
    if (complete) snprintf(line, sizeof line, "Done! Gift unlocked.");
    else snprintf(line, sizeof line, "%d more to find", total - have);
    gfx_text_fit(c, FONT_BODY, tx, PIECES_Y + 20, complete ? GROW_COL : DIM, line, tw);

    /* How finds arrive. */
    int pw = SCREEN_W - 28;
    gfx_rect(c, 14, FIND_Y, pw, FOOTER_Y - 6 - FIND_Y, PANEL);
    draw_find_bar(c, app, 26, FIND_Y + 8, pw - 24);
    find_status(app, line, sizeof line);
    gfx_text_fit(c, FONT_BODY, 26, FIND_Y + 34, SAY_COL, line, pw - 24);
    gfx_text_fit(c, FONT_BODY, 26, FIND_Y + 58, FAINT, "+1 point per finished turn and per 1k output tokens.", pw - 24);
    snprintf(line, sizeof line, "%d points bring a find. A full set adds its gift.", FIND_POINTS);
    gfx_text_fit(c, FONT_BODY, 26, FIND_Y + 78, FAINT, line, pw - 24);
}

void pet_screen_draw(Canvas *c, const App *app, int64_t now_ms) {
    if (!app->pet_ready) {
        gfx_text(c, FONT_BODY, 20, 200, DIM, "Waiting for a bridge (pet runs on its clock)...", 0);
        return;
    }
    if (app->evolution_open) evolution_draw(c, app, now_ms);
    else if (app->wardrobe_open && book_open) draw_book(c, app, now_ms);
    else if (app->wardrobe_open) draw_wardrobe(c, app, now_ms);
    else draw_stats(c, app, now_ms);
}

const char *pet_screen_hints(const App *app) {
    if (app->pet_ready && app->evolution_open) return evolution_hints();
    if (app->pet_ready && app->wardrobe_open && book_open) return "D-pad pick set  L2/R2 page  Y/B wardrobe  X pat";
    if (app->pet_ready && app->wardrobe_open) return "D-pad browse  L2/R2 page  A on/off  Y book  B close  X pat";
    return "A forms  X pat  Y wardrobe  L/R switch  MENU quit";
}

static void wardrobe_say(App *app, const char *msg, int64_t now_ms) {
    snprintf(app->bubble, sizeof app->bubble, "%s", msg);
    app->bubble_until_ms = now_ms + BUBBLE_MS;
}

/* Selecting a tab lands on what's worn there, else the top item. */
static void wardrobe_pick_slot(App *app, int slot) {
    app->wardrobe_slot = (slot + N_WEAR) % N_WEAR;
    int ids[N_UNLOCKS], n = slot_items(app->wardrobe_slot, ids), worn = pet_worn(&app->pet, app->wardrobe_slot);
    app->wardrobe_sel = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] == worn) app->wardrobe_sel = i;
}

/* Book cards sit in BOOK_COLS columns across all pages: left/right swap columns, up/down move rows
 * (crossing pages), L2/R2 flip a whole page keeping the card's spot. */
static bool book_handle(Button b) {
    int n = N_COLLECTION_SETS, to = book_set;
    switch (b) {
    case PAD_Y:
    case PAD_B: book_open = false; return true;
    case PAD_LEFT: to = book_set % BOOK_COLS ? book_set - 1 : book_set + BOOK_COLS - 1; break;
    case PAD_RIGHT: to = book_set % BOOK_COLS == BOOK_COLS - 1 ? book_set - (BOOK_COLS - 1) : book_set + 1; break;
    case PAD_UP: to = book_set - BOOK_COLS < 0 ? book_set + (n - 1) / BOOK_COLS * BOOK_COLS : book_set - BOOK_COLS; break;
    case PAD_DOWN: to = book_set + BOOK_COLS >= n ? book_set % BOOK_COLS : book_set + BOOK_COLS; break;
    case PAD_L2:
    case PAD_R2: {
        int page = book_set / BOOK_PAGE + (b == PAD_R2 ? 1 : BOOK_PAGES - 1);
        to = page % BOOK_PAGES * BOOK_PAGE + book_set % BOOK_PAGE;
        if (to >= n) to = n - 1; /* a short last page lands on its last card */
        break;
    }
    default: return true;
    }
    if (to >= 0 && to < n) book_set = to; /* a short last row can't be landed past */
    return true;
}

bool pet_screen_handle(App *app, Button b, int64_t now_ms) {
    if (!app->pet_ready) return false;
    if (app->evolution_open) return evolution_handle(app, b, now_ms);
    if (!app->wardrobe_open) {
        if (b == PAD_A) {
            evolution_open(app);
            return true;
        }
        if (b != PAD_Y) return false;
        app->wardrobe_open = true;
        book_open = false;
        wardrobe_pick_slot(app, app->wardrobe_slot);
        app->bubble_until_ms = 0;
        return true;
    }
    if (b == PAD_L1 || b == PAD_R1) { /* still switches screens */
        app->wardrobe_open = false;
        book_open = false;
        return false;
    }
    if (b == PAD_X) return false; /* still pats */
    if (book_open) return book_handle(b);
    int ids[N_UNLOCKS], n = slot_items(app->wardrobe_slot, ids), sel = clamp_sel(app->wardrobe_sel, n);
    switch (b) {
    case PAD_Y: { /* open the book on the selected item's set */
        int set = n ? pet_unlock_set(ids[sel]) : -1;
        if (set >= 0 && set < N_COLLECTION_SETS) book_set = set;
        book_open = true;
        return true;
    }
    case PAD_B: app->wardrobe_open = false; return true;
    case PAD_LEFT: wardrobe_pick_slot(app, app->wardrobe_slot - 1); return true;
    case PAD_RIGHT: wardrobe_pick_slot(app, app->wardrobe_slot + 1); return true;
    case PAD_UP:
        if (n) app->wardrobe_sel = (sel + n - 1) % n;
        return true;
    case PAD_DOWN:
        if (n) app->wardrobe_sel = (sel + 1) % n;
        return true;
    case PAD_L2: /* a page back / forward, stopping at the ends */
        app->wardrobe_sel = clamp_sel(sel - PAGE_ROWS, n);
        return true;
    case PAD_R2:
        app->wardrobe_sel = clamp_sel(sel + PAGE_ROWS, n);
        return true;
    case PAD_A: {
        int id = wardrobe_id(app);
        char msg[64];
        if (id < 0) return true;
        if (!pet_has(&app->pet, id)) {
            snprintf(msg, sizeof msg, "Not yet! %s.", pet_unlock_hint(id));
        } else {
            bool on = pet_worn(&app->pet, app->wardrobe_slot) != id;
            pet_toggle_equip(&app->pet, id);
            int slot = app->wardrobe_slot, fam = robot_family(slot);
            const char *name = pet_unlock_name(id);
            if (slot == WEAR_DESK) snprintf(msg, sizeof msg, on ? "%s is on the den shelf." : "%s put away.", name);
            else if (slot == WEAR_ROOM) snprintf(msg, sizeof msg, on ? "%s rolled out!" : "%s rolled up.", name);
            else if (slot == WEAR_WALL) snprintf(msg, sizeof msg, on ? "%s is up on the den wall." : "%s taken down.", name);
            else if (slot == WEAR_WINDOW && on) snprintf(msg, sizeof msg, "The den window shows %s.", name);
            else if (slot == WEAR_WINDOW) snprintf(msg, sizeof msg, "The window shows the sky again.");
            else if (fam >= 0 && on) snprintf(msg, sizeof msg, "%s bots dress as %s!", family_name((Family)fam), name);
            else if (fam >= 0) snprintf(msg, sizeof msg, "%s bots are back to normal.", family_name((Family)fam));
            else if (id == U_SHINY) snprintf(msg, sizeof msg, "%s", on ? "So shiny!" : "Back to its usual colors.");
            else if (slot == WEAR_SHINY && on) snprintf(msg, sizeof msg, "%s on! New colors.", name);
            else if (slot == WEAR_SHINY) snprintf(msg, sizeof msg, "Back to its usual colors.");
            else snprintf(msg, sizeof msg, on ? "Wearing the %s!" : "Took off the %s.", name);
        }
        wardrobe_say(app, msg, now_ms);
        return true;
    }
    default: return true;
    }
}
