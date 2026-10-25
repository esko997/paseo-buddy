#include "rebirth_view.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pet_view.h"
#include "rewrite.h"
#include "theme.h"

#define BUBBLE_MS 4000
#define READY_COL 0x46c06f
#define FINAL_FORM (PET_FORMS - 1)

enum { PAGE_CLOSED, PAGE_HUB, PAGE_TRAIT, PAGE_CONFIRM, PAGE_ALBUM };

/* Layout inside [HEADER_H, FOOTER_Y), shared by every page: a title row, big panels (184 px wide)
 * with an info column beside them, a full-width block below, a note line and the button row. */
#define TITLE_Y (HEADER_H + 6)
#define PANEL_Y (HEADER_H + 30)
#define PANEL_W 184
#define PANEL_H 194
#define BIG 10 /* 160 px previews */
#define LEFT_X 14
#define SECOND_X (LEFT_X + PANEL_W + 8)        /* second panel (trait preview, confirmation) */
#define INFO_X (LEFT_X + PANEL_W + 14)         /* info beside one panel (hub, album) */
#define SIDE_X (SECOND_X + PANEL_W + 12)       /* info beside two panels */
#define BLOCK_Y (PANEL_Y + PANEL_H + 14)       /* full-width block under the panels */
#define CONF_PANEL_H 150                       /* the confirmation uses shorter panels... */
#define CONF_SCALE 7                           /* ...with 112 px previews */
#define CONF_ROWS_Y (PANEL_Y + CONF_PANEL_H + 10)
#define CONF_ROW_H 42
#define CONF_ERR_Y (CONF_ROWS_Y + 3 * CONF_ROW_H + 2)
#define CELL 36
#define PITCH 38
#define STRIP_X ((SCREEN_W - PET_FORMS * PITCH + (PITCH - CELL)) / 2)
#define NOTE_Y (FOOTER_Y - 66)
#define BUTTON_Y (FOOTER_Y - 38)
#define BUTTON_H 26
#define FULL_W (SCREEN_W - 28)

#define N_TRAIT_FIELDS PET_TRAITS
static const char *const TRAIT_NAMES[N_TRAIT_FIELDS] = {"Body", "Ears", "Tail", "Crown", "Wings", "Markings", "Motion"};
static const char *const TRAIT_VALUES[N_TRAIT_FIELDS][3] = {
    {"round", "square", "pear"},    {"rounded", "pointed", "drooping"}, {"curl", "fin", "tuft"},
    {"antenna", "horns", "sprout"}, {"small", "feather", "fin"},        {"freckles", "belly", "stripes"},
    {"bounce", "float", "waddle"},
};

/* ---- helpers --------------------------------------------------------------------------------- */

static void trait_array(PetTraits t, int v[N_TRAIT_FIELDS]) {
    v[0] = t.body, v[1] = t.ears, v[2] = t.tail, v[3] = t.crown;
    v[4] = t.wings, v[5] = t.markings, v[6] = t.motion;
}

static const char *trait_value(int trait, int value) {
    if (trait < 0 || trait >= N_TRAIT_FIELDS || value < 0 || value > 2) return "?";
    return TRAIT_VALUES[trait][value];
}

static int clamp_trait(int t) { return t < 0 || t >= N_TRAIT_FIELDS ? 0 : t; }

static int forms_ready(const Pet *p) {
    int n = pet_forms_available(p);
    return n < 1 ? 1 : n > PET_FORMS ? PET_FORMS : n;
}

static int worn_form(const Pet *p) { return p->form < PET_FORMS ? p->form : 0; }

static unsigned long life_no(const Pet *p) { return (unsigned long)p->generation; }

/* Short label: "Original" or "Form 7". */
static void form_name(int form, char *buf, size_t len) {
    if (form == 0) snprintf(buf, len, "Original");
    else snprintf(buf, len, "Form %d", form);
}

/* In a sentence: "its original form" or "Form 7". */
static void form_prose(int form, char *buf, size_t len) {
    if (form == 0) snprintf(buf, len, "its original form");
    else snprintf(buf, len, "Form %d", form);
}

static void age_text(int64_t now, int64_t born, char *buf, size_t len) {
    int64_t days = (now - born) / 86400;
    if (days < 1) snprintf(buf, len, "born today");
    else snprintf(buf, len, "%lld day%s old", (long long)days, days == 1 ? "" : "s");
}

static int count_found(const Pet *p) {
    int n = 0;
    for (int id = 0; id < N_UNLOCKS; id++) n += pet_has(p, id);
    return n;
}

static void say(App *app, const char *msg, int64_t now_ms) {
    snprintf(app->bubble, sizeof app->bubble, "%s", msg);
    app->bubble_until_ms = now_ms + BUBBLE_MS;
}

static void go(App *app, int page) {
    app->rebirth_page = page;
    app->bubble_until_ms = 0; /* a stale note shouldn't follow onto another page */
}

/* Darkens a box toward `to` (t in 0..256): locked forms stay visible but plainly out of reach. */
static void dim_box(Canvas *c, int x, int y, int w, int h, uint32_t to, int t) {
    int x0 = x < 0 ? 0 : x, y0 = y < HEADER_H ? HEADER_H : y;
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w, y1 = y + h > FOOTER_Y ? FOOTER_Y : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + yy * SCREEN_W;
        for (int xx = x0; xx < x1; xx++) row[xx] = lerp_col(row[xx], to, t);
    }
}

/* A filled label like the forms browser's WEARING tag; returns its width. */
static int draw_tag(Canvas *c, int x, int y, const char *s, uint32_t bg) {
    int w = (int)strlen(s) * gfx_font_w(FONT_BODY) + 12;
    gfx_rect(c, x, y, w, 20, bg);
    gfx_text(c, FONT_BODY, x + 6, y, BG, s, 0);
    return w;
}

/* A key cap and its action; dimmed when the action is not available. Returns its width. */
static int draw_button(Canvas *c, int x, const char *key, const char *text, bool on, uint32_t cap) {
    int fw = gfx_font_w(FONT_BODY), w = 34 + (int)strlen(text) * fw + 10;
    gfx_rect(c, x, BUTTON_Y, w, BUTTON_H, PANEL);
    gfx_rect(c, x, BUTTON_Y, 26, BUTTON_H, on ? cap : FAINT);
    gfx_text(c, FONT_BODY, x + (26 - (int)strlen(key) * fw) / 2, BUTTON_Y + 3, BG, key, 0);
    gfx_text(c, FONT_BODY, x + 34, BUTTON_Y + 3, on ? TEXT : FAINT, text, 0);
    return w + 12;
}

static void draw_title(Canvas *c, const char *title, uint32_t col, const char *right) {
    int fw = gfx_font_w(FONT_BODY), rw = (int)strlen(right) * fw;
    gfx_text(c, FONT_BODY, LEFT_X, TITLE_Y, col, title, 0);
    gfx_text(c, FONT_BODY, SCREEN_W - 14 - rw, TITLE_Y, DIM, right, 0);
}

/* The bubble while it lasts, else a quiet page note. */
static void draw_note(Canvas *c, const App *app, int64_t now_ms, const char *idle) {
    if (now_ms < app->bubble_until_ms) gfx_text_fit(c, FONT_BODY, LEFT_X, NOTE_Y, SAY_COL, app->bubble, FULL_W);
    else if (idle) gfx_text_fit(c, FONT_BODY, LEFT_X, NOTE_Y, FAINT, idle, FULL_W);
}

/* A labelled panel holding pet p as `form` (none if p is NULL), centred below the label row. */
static void draw_panel(Canvas *c, const Pet *p, int form, int x, int h, int scale, const char *label, const char *name,
                       uint32_t col, int64_t now_ms) {
    int fw = gfx_font_w(FONT_BODY), size = 16 * scale, nw = (int)strlen(name) * fw;
    gfx_rect(c, x, PANEL_Y, PANEL_W, h, PANEL);
    gfx_text_fit(c, FONT_BODY, x + 10, PANEL_Y + 4, col, label, PANEL_W - 24 - nw);
    gfx_text(c, FONT_BODY, x + PANEL_W - 10 - nw, PANEL_Y + 4, TEXT, name, 0);
    if (p) pet_draw_form_preview(c, p, form, x + (PANEL_W - size) / 2, PANEL_Y + 28 + (h - 30 - size) / 2, scale, now_ms);
    gfx_frame(c, x, PANEL_Y, PANEL_W, h, 2, col);
}

/* A dim label with its value 110 px to the right, clipped to the info column. */
static void stat_row(Canvas *c, int x, int y, int w, const char *label, const char *value, uint32_t col) {
    gfx_text(c, FONT_BODY, x, y, DIM, label, 0);
    gfx_text_fit(c, FONT_BODY, x + 110, y, col, value, w - 110);
}


/* Every form of p in a row at y: its worn one has a purple cap, unreached ones are dimmed and the
 * selected one is framed in yellow. */
static void draw_strip(Canvas *c, const Pet *p, int sel, int y, int64_t now_ms) {
    int ready = forms_ready(p), worn = worn_form(p);
    for (int f = 0; f < PET_FORMS; f++) {
        int x = STRIP_X + f * PITCH;
        gfx_rect(c, x, y, CELL, CELL, f == sel ? PANEL_HI : PANEL);
        pet_draw_form_preview(c, p, f, x + (CELL - 32) / 2, y + (CELL - 32) / 2, 2, now_ms);
        if (f >= ready) dim_box(c, x, y, CELL, CELL, PANEL, 170);
        if (f == worn) gfx_rect(c, x, y, CELL, 3, GROW_COL);
        if (f == sel) gfx_frame(c, x - 1, y - 1, CELL + 2, CELL + 2, 2, SAY_COL);
    }
}

/* Recomputes the candidate next life for the chosen trait (input only: pure, no I/O). The DNA and
 * the old final form are fixed, so this never rerolls; it only refreshes copied lifetime stats. */
static bool refresh_preview(App *app) {
    app->rebirth_trait = clamp_trait(app->rebirth_trait);
    app->rebirth_preview_ok = pet_rewrite_preview(&app->pet, app->rebirth_trait, app_now(app), &app->rebirth_preview);
    return app->rebirth_preview_ok;
}

/* Loads one archived life into the album cache (input only). A missing or bad archive stays visible
 * as an error; nothing is substituted for it. */
static void album_load(App *app, uint32_t gen) {
    app->rebirth_album_gen = gen;
    app->rebirth_album_ok = false;
    app->rebirth_album_form = 0;
    app->rebirth_album_err[0] = 0;
    if (!pet_lineage_load(app->pet_path, gen, &app->rebirth_album, app->rebirth_album_err,
                          sizeof app->rebirth_album_err)) {
        if (!app->rebirth_album_err[0])
            snprintf(app->rebirth_album_err, sizeof app->rebirth_album_err, "Its archive is missing or unreadable.");
        return;
    }
    app->rebirth_album_ok = true;
    int ready = forms_ready(&app->rebirth_album), worn = worn_form(&app->rebirth_album);
    app->rebirth_album_form = worn < ready ? worn : ready - 1;
}

/* ---- hub ------------------------------------------------------------------------------------- */

/* Why the final reward is not ready yet. */
static void locked_text(const Pet *p, char *buf, size_t len) {
    char need[16], left[16];
    int64_t at = pet_form_threshold(FINAL_FORM), togo = at - p->life_xp;
    format_tokens(at, need, sizeof need);
    format_tokens(togo > 0 ? togo : 0, left, sizeof left);
    if (p->stage == STAGE_ADULT && forms_ready(p) >= PET_FORMS) /* only the lineage counter can stop it */
        snprintf(buf, len, "This lineage has reached the last life number; no further rewrites.");
    else if (p->stage != STAGE_ADULT)
        snprintf(buf, len, "Unlocks at this life's final Form 15: %s tokens eaten this life (%s to go). Grow up first.",
                 need, left);
    else
        snprintf(buf, len, "Unlocks at this life's final Form 15: %s tokens eaten this life (%s to go).", need, left);
}

static void draw_hub(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet;
    int64_t now = app_now(app);
    int fw = gfx_font_w(FONT_BODY), worn = worn_form(p), x = INFO_X, w = SCREEN_W - 14 - INFO_X, y = PANEL_Y;
    bool ready = pet_rewrite_ready(p);
    char line[192], name[24], val[64], tok[16], age[32];

    snprintf(line, sizeof line, "Life #%lu", life_no(p));
    draw_title(c, "DNA REWRITE", TEXT, line);

    form_name(worn, name, sizeof name);
    draw_panel(c, p, worn, LEFT_X, PANEL_H, BIG, "CURRENT", name, GROW_COL, now_ms);

    gfx_text(c, FONT_BIG, x, y, TEXT, p->name, 0);
    y += 36;
    age_text(now, p->life_born, age, sizeof age);
    form_prose(worn, name, sizeof name);
    snprintf(line, sizeof line, "%s, %s, wearing %s", stage_name(p->stage), age, name);
    gfx_text_fit(c, FONT_BODY, x, y, DIM, line, w);
    y += 26;
    double frac = pet_xp_progress(p, val, sizeof val);
    int nw = (int)strlen(val) * fw;
    gfx_text(c, FONT_BODY, x, y, DIM, "This life", 0);
    gfx_bar(c, x + 110, y + 5, w - 110 - nw - fw, 10, frac, FAINT, GROW_COL);
    gfx_text(c, FONT_BODY, x + w - nw, y, TEXT, val, 0);
    y += 24;
    snprintf(val, sizeof val, "%d of %d ready", forms_ready(p), PET_FORMS);
    stat_row(c, x, y, w, "Forms", val, TEXT);
    y += 24;
    format_tokens(p->xp, tok, sizeof tok);
    snprintf(val, sizeof val, "%s tokens, always kept", tok);
    stat_row(c, x, y, w, "Lifetime", val, TEXT);
    y += 24;
    snprintf(val, sizeof val, "%lld", (long long)p->lifetime_turns);
    stat_row(c, x, y, w, "Turns ever", val, TEXT);
    y += 24;
    if (p->generation <= 1) snprintf(val, sizeof val, "Original life");
    else snprintf(val, sizeof val, "%lu past li%s archived", life_no(p) - 1, p->generation == 2 ? "fe" : "ves");
    stat_row(c, x, y, w, "Lineage", val, TEXT);
    y += 24;
    if (p->inherited_trait >= 0 && p->inherited_trait < N_TRAIT_FIELDS)
        snprintf(val, sizeof val, "%s: %s", TRAIT_NAMES[p->inherited_trait],
                 trait_value(p->inherited_trait, p->inherited_value));
    else snprintf(val, sizeof val, "None (original DNA)");
    stat_row(c, x, y, w, "Inherited", val, p->inherited_trait >= 0 ? GROW_COL : TEXT);

    /* the final reward: ready, or what it still needs */
    int bx = LEFT_X, by = BLOCK_Y, bh = NOTE_Y - 8 - BLOCK_Y;
    uint32_t col = ready ? READY_COL : FAINT;
    gfx_rect(c, bx, by, FULL_W, bh, PANEL);
    gfx_frame(c, bx, by, FULL_W, bh, 2, col);
    int tx = bx + 12 + draw_tag(c, bx + 12, by + 8, "FINAL REWARD", GROW_COL) + 8;
    draw_tag(c, tx, by + 8, ready ? "READY" : "LOCKED", ready ? READY_COL : DIM);
    char have[16], need[16];
    format_tokens(p->life_xp, have, sizeof have);
    format_tokens(pet_form_threshold(FINAL_FORM), need, sizeof need);
    snprintf(val, sizeof val, "Form 15: %s/%s", have, need);
    gfx_text(c, FONT_BODY, bx + FULL_W - 12 - (int)strlen(val) * fw, by + 8, ready ? READY_COL : DIM, val, 0);
    if (ready) {
        snprintf(line, sizeof line,
                 "Form 15 reached. Optional: start life #%lu as a Baby with fresh DNA and one Form 15 trait. "
                 "Lifetime stats, cosmetics and outfits stay; this life is archived.",
                 life_no(p) + 1);
        gfx_text_wrap(c, FONT_BODY, bx + 12, by + 34, FULL_W - 24, 3, TEXT, line);
    } else {
        double f = (double)p->life_xp / (double)pet_form_threshold(FINAL_FORM);
        gfx_bar(c, bx + 12, by + 36, FULL_W - 24, 8, f, FAINT, GROW_COL);
        locked_text(p, line, sizeof line);
        gfx_text_wrap(c, FONT_BODY, bx + 12, by + 50, FULL_W - 24, 2, DIM, line);
    }

    draw_note(c, app, now_ms, ready ? "Optional and never automatic. Every past life stays in the album."
                                    : "Keep feeding this life; past lives stay in the album.");
    int bxx = LEFT_X;
    bxx += draw_button(c, bxx, "A", "Begin rewrite", ready, SAY_COL);
    bxx += draw_button(c, bxx, "Y", "Lineage album", true, SAY_COL);
    draw_button(c, bxx, "B", "Back to forms", true, SAY_COL);
}

static bool hub_handle(App *app, Button b, int64_t now_ms) {
    const Pet *p = &app->pet;
    char msg[64], left[16];
    switch (b) {
    case PAD_A:
        if (!pet_rewrite_ready(p)) {
            if (p->stage == STAGE_ADULT && forms_ready(p) >= PET_FORMS) {
                say(app, "No further rewrites: the lineage is full.", now_ms);
            } else {
                int64_t togo = pet_form_threshold(FINAL_FORM) - p->life_xp;
                format_tokens(togo > 0 ? togo : 0, left, sizeof left);
                snprintf(msg, sizeof msg, "Locked: Form 15 needs %s more tokens this life.", left);
                say(app, msg, now_ms);
            }
            return true;
        }
        if (!refresh_preview(app)) {
            say(app, "The next life can't be previewed right now.", now_ms);
            return true;
        }
        go(app, PAGE_TRAIT);
        return true;
    case PAD_Y:
        go(app, PAGE_ALBUM);
        if (p->generation > 1) {
            uint32_t newest = p->generation - 1, gen = app->rebirth_album_gen;
            if (gen < 1 || gen > newest) gen = newest;
            if (!app->rebirth_album_ok || app->rebirth_album_gen != gen) album_load(app, gen);
        } else {
            app->rebirth_album_gen = 0;
            app->rebirth_album_ok = false;
        }
        return true;
    case PAD_B: /* back to the forms browser */
        go(app, PAGE_CLOSED);
        return true;
    default: return true;
    }
}

/* ---- trait preview --------------------------------------------------------------------------- */

static void draw_trait(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet, *next = app->rebirth_preview_ok ? &app->rebirth_preview : NULL;
    int fw = gfx_font_w(FONT_BODY), sel = clamp_trait(app->rebirth_trait);
    char line[200], name[40];
    int v[N_TRAIT_FIELDS];
    trait_array(pet_form_traits(p, FINAL_FORM), v);
    int value = next ? next->inherited_value : v[sel];

    snprintf(line, sizeof line, "Life #%lu -> #%lu", life_no(p), life_no(p) + 1);
    draw_title(c, "INHERIT ONE TRAIT", TEXT, line);

    draw_panel(c, p, FINAL_FORM, LEFT_X, PANEL_H, BIG, "OLD FINAL", "Form 15", GROW_COL, now_ms);
    snprintf(name, sizeof name, "Life #%lu", life_no(p) + 1);
    draw_panel(c, next, 0, SECOND_X, PANEL_H, BIG, "NEW BABY", name, SAY_COL, now_ms);
    if (!next)
        gfx_text_wrap(c, FONT_BODY, SECOND_X + 10, PANEL_Y + 90, PANEL_W - 20, 3, ALERT_COL, "Preview unavailable.");

    /* old final form's seven traits; the chosen one is highlighted */
    int x = SIDE_X, w = SCREEN_W - 14 - SIDE_X, y = PANEL_Y;
    gfx_text(c, FONT_BODY, x, y, DIM, "FORM 15 TRAITS", 0);
    y += 26;
    for (int i = 0; i < N_TRAIT_FIELDS; i++, y += 22) {
        bool on = i == sel;
        if (on) {
            gfx_rect(c, x, y - 1, w, 22, PANEL_HI);
            gfx_rect(c, x, y - 1, 4, 22, SAY_COL);
        }
        gfx_text(c, FONT_BODY, x + 10, y, on ? TEXT : DIM, TRAIT_NAMES[i], 0);
        gfx_text(c, FONT_BODY, x + 10 + 9 * fw, y, on ? SAY_COL : TEXT, trait_value(i, v[i]), 0);
    }

    /* what is carried over, and from where */
    y = BLOCK_Y;
    int tw = draw_tag(c, LEFT_X, y, "INHERITS", SAY_COL);
    snprintf(line, sizeof line, "%s: %s", TRAIT_NAMES[sel], trait_value(sel, value));
    gfx_text(c, FONT_BODY, LEFT_X + tw + 10, y, SAY_COL, line, 0);
    snprintf(line, sizeof line,
             "From old final Form 15. Baby keeps its classic look. Every adult form of life #%lu keeps "
             "%s: %s. New DNA supplies the other six traits.",
             life_no(p) + 1, TRAIT_NAMES[sel], trait_value(sel, value));
    gfx_text_wrap(c, FONT_BODY, LEFT_X, y + 28, FULL_W, 4, TEXT, line);

    draw_note(c, app, now_ms, "Nothing changes yet. Browsing traits never rerolls the new DNA.");
    int bx = LEFT_X;
    bx += draw_button(c, bx, "A", "Continue", next != NULL, SAY_COL);
    draw_button(c, bx, "B", "Cancel", true, SAY_COL);
}

static bool trait_handle(App *app, Button b, int64_t now_ms) {
    int t = clamp_trait(app->rebirth_trait);
    switch (b) {
    case PAD_LEFT:
    case PAD_UP: t = (t + N_TRAIT_FIELDS - 1) % N_TRAIT_FIELDS; break;
    case PAD_RIGHT:
    case PAD_DOWN: t = (t + 1) % N_TRAIT_FIELDS; break;
    case PAD_A:
        if (!refresh_preview(app)) {
            say(app, "The next life can't be previewed right now.", now_ms);
            return true;
        }
        app->rebirth_error[0] = 0;
        go(app, PAGE_CONFIRM);
        return true;
    case PAD_B: go(app, PAGE_HUB); return true;
    default: return true;
    }
    app->rebirth_trait = t;
    refresh_preview(app);
    app->bubble_until_ms = 0;
    return true;
}

/* ---- confirmation ---------------------------------------------------------------------------- */

/* A coloured key word and up to two lines of explanation. */
static void confirm_row(Canvas *c, int y, const char *tag, uint32_t col, const char *text) {
    draw_tag(c, LEFT_X, y, tag, col);
    gfx_text_wrap(c, FONT_BODY, LEFT_X + 100, y, FULL_W - 100, 2, TEXT, text);
}

/* A dim heading over its value in the side column. */
static void side_item(Canvas *c, int y, const char *label, const char *value, uint32_t col) {
    int w = SCREEN_W - 14 - SIDE_X;
    gfx_text(c, FONT_BODY, SIDE_X, y, DIM, label, 0);
    gfx_text_fit(c, FONT_BODY, SIDE_X, y + 20, col, value, w);
}

static void draw_confirm(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet, *next = app->rebirth_preview_ok ? &app->rebirth_preview : NULL;
    int sel = clamp_trait(app->rebirth_trait), found = count_found(p);
    char line[200], label[24], val[48], tok[16];
    int v[N_TRAIT_FIELDS];
    trait_array(pet_form_traits(p, FINAL_FORM), v);
    int value = next ? next->inherited_value : v[sel];

    snprintf(line, sizeof line, "Life #%lu -> #%lu", life_no(p), life_no(p) + 1);
    draw_title(c, "CONFIRM DNA REWRITE", SAY_COL, line);

    snprintf(label, sizeof label, "LIFE #%lu", life_no(p));
    draw_panel(c, p, FINAL_FORM, LEFT_X, CONF_PANEL_H, CONF_SCALE, label, "Form 15", GROW_COL, now_ms);
    snprintf(label, sizeof label, "LIFE #%lu", life_no(p) + 1);
    draw_panel(c, next, 0, SECOND_X, CONF_PANEL_H, CONF_SCALE, label, "Baby", SAY_COL, now_ms);

    snprintf(val, sizeof val, "%s: %s", TRAIT_NAMES[sel], trait_value(sel, value));
    side_item(c, PANEL_Y, "Inherits", val, SAY_COL);
    if (next) {
        snprintf(val, sizeof val, "%08X > %08X", (unsigned)(pet_dna(p) >> 32), (unsigned)(pet_dna(next) >> 32));
        side_item(c, PANEL_Y + 46, "DNA rewrite", val, TEXT);
    } else {
        side_item(c, PANEL_Y + 46, "DNA rewrite", "preview unavailable", ALERT_COL);
    }
    side_item(c, PANEL_Y + 92, "Growth", "Baby, from zero", TEXT);

    int y = CONF_ROWS_Y;
    snprintf(line, sizeof line, "Life #%lu and all %d of its forms stay forever in the lineage album, read only.",
             life_no(p), PET_FORMS);
    confirm_row(c, y, "ARCHIVE", GROW_COL, line);
    snprintf(line, sizeof line, "%s starts life #%lu as a Baby: new DNA, fresh colours, growth from zero.", p->name,
             life_no(p) + 1);
    confirm_row(c, y + CONF_ROW_H, "RESTART", SAY_COL, line);
    format_tokens(p->xp, tok, sizeof tok);
    snprintf(line, sizeof line, "Name, %s lifetime tokens, turns, records, %d cosmetics, outfits, finds.", tok,
             found);
    confirm_row(c, y + 2 * CONF_ROW_H, "KEPT", READY_COL, line);

    if (app->rebirth_error[0]) {
        snprintf(line, sizeof line, "Not rewritten; the pet is unchanged. %s", app->rebirth_error);
        gfx_text_wrap(c, FONT_BODY, LEFT_X, CONF_ERR_Y, FULL_W, 2, ALERT_COL, line);
    } else if (now_ms < app->bubble_until_ms) {
        gfx_text_fit(c, FONT_BODY, LEFT_X, NOTE_Y, SAY_COL, app->bubble, FULL_W);
    } else {
        gfx_text_fit(c, FONT_BODY, LEFT_X, NOTE_Y, FAINT, "Only Y rewrites. A does nothing here; B goes back.", FULL_W);
    }
    int bx = LEFT_X;
    bx += draw_button(c, bx, "Y", "Rewrite DNA now", next != NULL, ALERT_COL);
    draw_button(c, bx, "B", "Back to traits", true, SAY_COL);
}

static bool commit(App *app, int64_t now_ms) {
    char err[sizeof app->rebirth_error], msg[64];
    err[0] = 0;
    app->bubble_until_ms = 0;
    if (!pet_rewrite_commit(&app->pet, app->pet_path, clamp_trait(app->rebirth_trait), app_now(app), err, sizeof err)) {
        snprintf(app->rebirth_error, sizeof app->rebirth_error, "%s", err[0] ? err : "the rewrite could not be saved.");
        return true;
    }
    /* pet_rewrite_commit replaced app->pet and saved it; a leftover message is a durability warning */
    if (err[0]) fprintf(stderr, "dna rewrite: %s\n", err);
    app->saved_pet = app->pet;
    app->last_save = app_now(app);
    app->rebirth_page = PAGE_CLOSED;
    app->rebirth_preview_ok = false;
    app->rebirth_error[0] = 0;
    app->rebirth_album_gen = 0;
    app->rebirth_album_ok = false;
    app->evolution_open = false; /* back to the stats page */
    app->wardrobe_open = false;
    snprintf(msg, sizeof msg, "Life #%lu begins! %s is a Baby again.", life_no(&app->pet), app->pet.name);
    say(app, msg, now_ms);
    return true;
}

static bool confirm_handle(App *app, Button b, int64_t now_ms) {
    switch (b) {
    case PAD_Y: return commit(app, now_ms);
    case PAD_B:
        app->rebirth_error[0] = 0;
        refresh_preview(app);
        go(app, PAGE_TRAIT);
        return true;
    case PAD_A: say(app, "A does not rewrite. Press Y to confirm, B to go back.", now_ms); return true;
    default: return true;
    }
}

/* ---- lineage album --------------------------------------------------------------------------- */

static void draw_album_message(Canvas *c, const char *head, uint32_t col, const char *text) {
    int h = BLOCK_Y - 14 - PANEL_Y;
    gfx_rect(c, LEFT_X, PANEL_Y, FULL_W, h, PANEL);
    gfx_frame(c, LEFT_X, PANEL_Y, FULL_W, h, 2, col);
    gfx_text(c, FONT_BIG, LEFT_X + 16, PANEL_Y + 16, col, head, 0);
    gfx_text_wrap(c, FONT_BODY, LEFT_X + 16, PANEL_Y + 64, FULL_W - 32, 6, TEXT, text);
}

static void draw_album_life(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *a = &app->rebirth_album;
    int fw = gfx_font_w(FONT_BODY), ready = forms_ready(a), worn = worn_form(a);
    int sel = app->rebirth_album_form < 0 ? 0 : app->rebirth_album_form >= ready ? ready - 1 : app->rebirth_album_form;
    int x = INFO_X, w = SCREEN_W - 14 - INFO_X, y = PANEL_Y;
    char line[160], label[24], name[24], tok[16];

    snprintf(label, sizeof label, "LIFE #%lu", life_no(a));
    form_name(sel, name, sizeof name);
    draw_panel(c, a, sel, LEFT_X, PANEL_H, BIG, label, name, GROW_COL, now_ms);

    gfx_text(c, FONT_BIG, x, y, TEXT, name, 0);
    y += 36;
    int tx = x;
    if (sel == worn) tx += draw_tag(c, tx, y, "WORN AT ARCHIVE", GROW_COL) + 8;
    if (sel == FINAL_FORM) draw_tag(c, tx, y, "FINAL", READY_COL);
    else if (sel != worn) draw_tag(c, tx, y, "REACHED", DIM);
    y += 28;
    if (sel == 0) {
        gfx_text_wrap(c, FONT_BODY, x, y, w, 3, DIM, "Its classic look, as it was when this life was archived.");
    } else {
        int v[N_TRAIT_FIELDS], half = w / 2;
        trait_array(pet_form_traits(a, sel), v);
        for (int i = 0; i < N_TRAIT_FIELDS; i++) {
            int cx = x + (i < 4 ? 0 : half), cy = y + (i < 4 ? i : i - 4) * 20;
            bool inherited = i == a->inherited_trait;
            if (inherited) gfx_rect(c, cx, cy + 4, 4, 12, GROW_COL);
            gfx_text(c, FONT_BODY, cx + 10, cy, DIM, TRAIT_NAMES[i], 0);
            gfx_text_fit(c, FONT_BODY, cx + 10 + 9 * fw, cy, inherited ? GROW_COL : TEXT, trait_value(i, v[i]),
                         half - 10 - 9 * fw - 4);
        }
    }
    y += 4 * 20 + 6;
    if (a->inherited_trait >= 0 && a->inherited_trait < N_TRAIT_FIELDS)
        snprintf(line, sizeof line, "Inherited %s: %s from life #%lu.", TRAIT_NAMES[a->inherited_trait],
                 trait_value(a->inherited_trait, a->inherited_value), life_no(a) - 1);
    else snprintf(line, sizeof line, "The original life: no inherited trait.");
    gfx_text_fit(c, FONT_BODY, x, y, a->inherited_trait >= 0 ? GROW_COL : DIM, line, w);

    draw_strip(c, a, sel, BLOCK_Y, now_ms);

    format_tokens(a->life_xp, tok, sizeof tok);
    snprintf(line, sizeof line, "%d of %d forms reached. Ate %s tokens in this life. Favorite: %s. Adult family: %s.",
             ready, PET_FORMS, tok, family_name(pet_favorite(a)), family_name(a->adult_family));
    gfx_text_wrap(c, FONT_BODY, LEFT_X, BLOCK_Y + CELL + 10, FULL_W, 2, TEXT, line);
}

static void draw_album(Canvas *c, const App *app, int64_t now_ms) {
    const Pet *p = &app->pet;
    char line[200], head[48];
    uint32_t gen = app->rebirth_album_gen;
    bool any = p->generation > 1;

    if (!any) snprintf(line, sizeof line, "No past lives");
    else snprintf(line, sizeof line, "Life #%lu of %lu archived", (unsigned long)gen, life_no(p) - 1);
    draw_title(c, "LINEAGE ALBUM", TEXT, line);

    if (!any) {
        snprintf(line, sizeof line,
                 "This is life #1, the original. When a life reaches its final Form 15 and you choose a DNA "
                 "Rewrite, that life and all of its forms are archived here to browse, read only.");
        draw_album_message(c, "No past lives yet", DIM, line);
    } else if (!app->rebirth_album_ok) {
        snprintf(head, sizeof head, "Life #%lu unavailable", (unsigned long)gen);
        snprintf(line, sizeof line, "Its archive could not be read, so nothing is shown in its place. %s",
                 app->rebirth_album_err);
        draw_album_message(c, head, ALERT_COL, line);
    } else {
        draw_album_life(c, app, now_ms);
    }

    draw_note(c, app, now_ms, "Read only: past forms are never worn by the current life.");
    int bx = LEFT_X;
    bx += draw_button(c, bx, "L2", "Older life", any && gen > 1, SAY_COL);
    bx += draw_button(c, bx, "R2", "Newer life", any && gen < p->generation - 1, SAY_COL);
    draw_button(c, bx, "B", "Back", true, SAY_COL);
}

static bool album_handle(App *app, Button b, int64_t now_ms) {
    (void)now_ms;
    uint32_t gen = app->rebirth_album_gen, newest = app->pet.generation > 1 ? app->pet.generation - 1 : 0;
    switch (b) {
    case PAD_B: go(app, PAGE_HUB); return true;
    case PAD_L2: /* stops at the ends, like the forms browser's jumps */
        if (newest && gen > 1) album_load(app, gen - 1);
        app->bubble_until_ms = 0;
        return true;
    case PAD_R2:
        if (newest && gen < newest) album_load(app, gen + 1);
        app->bubble_until_ms = 0;
        return true;
    case PAD_LEFT:
    case PAD_UP:
    case PAD_RIGHT:
    case PAD_DOWN: {
        if (!app->rebirth_album_ok) return true;
        int n = forms_ready(&app->rebirth_album), f = app->rebirth_album_form;
        f = b == PAD_LEFT || b == PAD_UP ? f - 1 : f + 1;
        app->rebirth_album_form = (f % n + n) % n; /* reached forms only */
        return true;
    }
    default: return true;
    }
}

/* ---- entry points ---------------------------------------------------------------------------- */

void rebirth_open(App *app, int64_t now_ms) {
    (void)now_ms;
    if (!app->pet_ready) return;
    app->rebirth_trait = clamp_trait(app->rebirth_trait);
    app->rebirth_preview_ok = false;
    app->rebirth_error[0] = 0;
    app->rebirth_album_gen = 0; /* the album rereads on demand, so a fixed archive shows up */
    app->rebirth_album_ok = false;
    app->rebirth_album_err[0] = 0;
    go(app, PAGE_HUB);
}

void rebirth_draw(Canvas *c, const App *app, int64_t now_ms) {
    if (!app->pet_ready) return;
    switch (app->rebirth_page) {
    case PAGE_HUB: draw_hub(c, app, now_ms); break;
    case PAGE_TRAIT: draw_trait(c, app, now_ms); break;
    case PAGE_CONFIRM: draw_confirm(c, app, now_ms); break;
    case PAGE_ALBUM: draw_album(c, app, now_ms); break;
    default: break;
    }
}

bool rebirth_handle(App *app, Button b, int64_t now_ms) {
    if (!app->pet_ready) {
        app->rebirth_page = PAGE_CLOSED;
        app->evolution_open = false;
        return false;
    }
    switch (b) {
    case PAD_L1:
    case PAD_R1: /* still switches screens */
        app->rebirth_page = PAGE_CLOSED;
        app->evolution_open = false;
        app->bubble_until_ms = 0;
        return false;
    case PAD_X: return false; /* still pats */
    default: break;
    }
    switch (app->rebirth_page) {
    case PAGE_HUB: return hub_handle(app, b, now_ms);
    case PAGE_TRAIT: return trait_handle(app, b, now_ms);
    case PAGE_CONFIRM: return confirm_handle(app, b, now_ms);
    case PAGE_ALBUM: return album_handle(app, b, now_ms);
    default: app->rebirth_page = PAGE_CLOSED; return true;
    }
}

const char *rebirth_hints(const App *app) {
    switch (app->rebirth_page) {
    case PAGE_HUB:
        return app->pet_ready && pet_rewrite_ready(&app->pet) ? "A rewrite  Y album  B forms  X pat  L/R switch"
                                                              : "Y album  B forms  X pat  L/R switch";
    case PAGE_TRAIT: return "Left/Right trait  A continue  B cancel  X pat";
    case PAGE_CONFIRM: return "Y REWRITE DNA  B back  X pat";
    case PAGE_ALBUM: return "Left/Right forms  L2/R2 life  B back  X pat";
    default: return "";
    }
}
