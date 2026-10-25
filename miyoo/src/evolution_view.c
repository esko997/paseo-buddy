#include "evolution_view.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pet_view.h"
#include "rebirth_view.h"
#include "theme.h"

#define BUBBLE_MS 4000
#define NEW_COL 0x46c06f /* forms reached since the browser was last opened */

/* Layout, top to bottom inside [HEADER_H, FOOTER_Y): title row, CURRENT and PREVIEW panels with the
 * previewed form's traits beside them, a strip of all PET_FORMS forms, the next-form progress, what
 * the previewed form needs or offers, a note line, and the A/B choice. */
#define TITLE_Y (HEADER_H + 6)
#define PANEL_Y (HEADER_H + 30)
#define PANEL_W 184
#define PANEL_H 194
#define BIG 10 /* big previews: 160 px */
#define CUR_X 14
#define PRE_X (CUR_X + PANEL_W + 8)
#define INFO_X (PRE_X + PANEL_W + 12)
#define STRIP_Y (PANEL_Y + PANEL_H + 14)
#define CELL 36
#define PITCH 38
#define STRIP_X ((SCREEN_W - PET_FORMS * PITCH + (PITCH - CELL)) / 2)
#define PROG_Y (STRIP_Y + CELL + 8)
#define STATUS_Y (PROG_Y + 24)
#define NOTE_Y (STATUS_Y + 44)
#define BUTTON_Y (NOTE_Y + 26)
#define BUTTON_H 26
#define JUMP 5 /* L2/R2 */

/* Forms past this were ready but not yet browsed when the browser opened: tagged NEW. UI-only, like
 * the wardrobe's book page, so it lives here rather than in App. */
static int new_after = PET_FORMS;

#define N_TRAIT_FIELDS 7
static const char *const TRAIT_NAMES[N_TRAIT_FIELDS] = {"Body", "Ears", "Tail", "Crown", "Wings", "Markings", "Motion"};
static const char *const TRAIT_VALUES[N_TRAIT_FIELDS][3] = {
    {"round", "square", "pear"},    {"rounded", "pointed", "drooping"}, {"curl", "fin", "tuft"},
    {"antenna", "horns", "sprout"}, {"small", "feather", "fin"},        {"freckles", "belly", "stripes"},
    {"bounce", "float", "waddle"},
};

static void trait_array(PetTraits t, int v[N_TRAIT_FIELDS]) {
    v[0] = t.body, v[1] = t.ears, v[2] = t.tail, v[3] = t.crown;
    v[4] = t.wings, v[5] = t.markings, v[6] = t.motion;
}

static int clamp_form(int f) { return f < 0 ? 0 : f >= PET_FORMS ? PET_FORMS - 1 : f; }

static int ready_forms(const Pet *p) {
    int n = pet_forms_available(p);
    return n < 1 ? 1 : n > PET_FORMS ? PET_FORMS : n;
}

static int worn_form(const Pet *p) { return p->form < PET_FORMS ? p->form : 0; }

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

static bool is_new(const Pet *p, int form) { return form > new_after && form < ready_forms(p); }

static int count_new(const Pet *p) {
    int n = 0;
    for (int f = 0; f < PET_FORMS; f++) n += is_new(p, f);
    return n;
}

static void say(App *app, const char *msg, int64_t now_ms) {
    snprintf(app->bubble, sizeof app->bubble, "%s", msg);
    app->bubble_until_ms = now_ms + BUBBLE_MS;
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

/* A filled label like the wardrobe's WEARING tag; returns its width. */
static int draw_tag(Canvas *c, int x, int y, const char *s, uint32_t bg) {
    int w = (int)strlen(s) * gfx_font_w(FONT_BODY) + 12;
    gfx_rect(c, x, y, w, 20, bg);
    gfx_text(c, FONT_BODY, x + 6, y, BG, s, 0);
    return w;
}

/* What a locked form needs, written out in full. */
static void requirement(const Pet *p, int form, char *buf, size_t len) {
    char name[16], need[16], left[16];
    int64_t at = pet_form_threshold(form), togo = at - p->life_xp;
    form_name(form, name, sizeof name);
    format_tokens(at, need, sizeof need);
    format_tokens(togo > 0 ? togo : 0, left, sizeof left);
    if (p->stage != STAGE_ADULT) {
        char settle[16];
        format_tokens(stage_threshold(STAGE_TEEN), settle, sizeof settle); /* teen -> adult */
        snprintf(buf, len, "Not final: adult traits settle at %s tokens. %s unlocks at %s (%s to go).", settle, name,
                 need, left);
    } else {
        snprintf(buf, len, "Locked. %s unlocks at %s tokens eaten (%s to go).", name, need, left);
    }
}

/* "CURRENT ... Form 3" over a big preview. The preview panel dims and says LOCKED for unreached forms. */
static void draw_panel(Canvas *c, const Pet *p, int form, int x, const char *label, uint32_t col, bool locked,
                       int64_t now_ms) {
    int fw = gfx_font_w(FONT_BODY), size = 16 * BIG;
    char name[16];
    form_name(form, name, sizeof name);
    gfx_rect(c, x, PANEL_Y, PANEL_W, PANEL_H, PANEL);
    gfx_text(c, FONT_BODY, x + 10, PANEL_Y + 4, col, label, 0);
    gfx_text(c, FONT_BODY, x + PANEL_W - 10 - (int)strlen(name) * fw, PANEL_Y + 4, locked ? DIM : TEXT, name, 0);
    pet_draw_form_preview(c, p, form, x + (PANEL_W - size) / 2, PANEL_Y + 28, BIG, now_ms);
    if (locked) { /* dimmed but visible, with a tag along the bottom so the face stays clear */
        const char *tag = p->stage != STAGE_ADULT ? "NOT FINAL" : "LOCKED";
        dim_box(c, x + 2, PANEL_Y + 26, PANEL_W - 4, PANEL_H - 28, PANEL, 120);
        int tw = (int)strlen(tag) * fw + 12;
        draw_tag(c, x + (PANEL_W - tw) / 2, PANEL_Y + PANEL_H - 26, tag, DIM);
    }
    gfx_frame(c, x, PANEL_Y, PANEL_W, PANEL_H, 2, col);
}

/* The previewed form's name, standing and traits; traits that differ from the worn form are marked. */
static void draw_info(Canvas *c, const Pet *p, int sel) {
    int fw = gfx_font_w(FONT_BODY), x = INFO_X, w = SCREEN_W - 14 - INFO_X, y = PANEL_Y;
    int ready = ready_forms(p), worn = worn_form(p);
    char name[16];
    form_name(sel, name, sizeof name);
    gfx_text(c, FONT_BIG, x, y, TEXT, name, 0);
    y += 36;
    if (sel == worn) draw_tag(c, x, y, "WEARING", GROW_COL);
    else if (sel >= ready) draw_tag(c, x, y, "LOCKED", DIM);
    else if (is_new(p, sel)) draw_tag(c, x, y, "NEW - READY", NEW_COL);
    else draw_tag(c, x, y, "READY", SAY_COL);
    y += 28;
    if (sel == 0) {
        gfx_text_wrap(c, FONT_BODY, x, y, w, 7, DIM,
                      "The classic look it grew up with. Its shape follows its stage; its colors follow what it eats.");
        return;
    }
    int v[N_TRAIT_FIELDS], cv[N_TRAIT_FIELDS];
    trait_array(pet_form_traits(p, sel), v);
    trait_array(pet_form_traits(p, worn), cv);
    bool compare = worn != 0 && worn != sel; /* the original has no generated traits to compare */
    for (int i = 0; i < N_TRAIT_FIELDS; i++, y += 20) {
        bool diff = compare && v[i] != cv[i];
        if (diff) gfx_rect(c, x, y + 4, 4, 12, SAY_COL);
        gfx_text(c, FONT_BODY, x + 10, y, DIM, TRAIT_NAMES[i], 0);
        const char *val = v[i] >= 0 && v[i] < 3 ? TRAIT_VALUES[i][v[i]] : "?";
        gfx_text(c, FONT_BODY, x + 10 + 9 * fw, y, diff ? SAY_COL : TEXT, val, 0);
    }
}

/* Every form in a row: the worn one has a purple cap, NEW ones a green dot, locked ones are dimmed,
 * and the previewed one is framed in yellow. */
static void draw_strip(Canvas *c, const Pet *p, int sel, int64_t now_ms) {
    int ready = ready_forms(p), worn = worn_form(p);
    for (int f = 0; f < PET_FORMS; f++) {
        int x = STRIP_X + f * PITCH;
        gfx_rect(c, x, STRIP_Y, CELL, CELL, f == sel ? PANEL_HI : PANEL);
        pet_draw_form_preview(c, p, f, x + (CELL - 32) / 2, STRIP_Y + (CELL - 32) / 2, 2, now_ms);
        if (f >= ready) dim_box(c, x, STRIP_Y, CELL, CELL, PANEL, 170);
        if (f == worn) gfx_rect(c, x, STRIP_Y, CELL, 3, GROW_COL);
        if (is_new(p, f)) gfx_rect(c, x + CELL - 9, STRIP_Y + 4, 6, 6, NEW_COL);
        if (f == sel) gfx_frame(c, x - 1, STRIP_Y - 1, CELL + 2, CELL + 2, 2, SAY_COL);
    }
}

/* "Next: Form 4 [bar] 9.20M/11.00M"; before adulthood the bar shows growing up, which opens forms. */
static void draw_progress(Canvas *c, const Pet *p, int x, int y, int w) {
    int fw = gfx_font_w(FONT_BODY), ready = ready_forms(p);
    if (ready >= PET_FORMS) {
        gfx_text_fit(c, FONT_BODY, x, y, GROW_COL, "All forms unlocked! Y opens DNA Rewrite and past lives.", w);
        return;
    }
    char label[24], num[40], have[16], need[16];
    int64_t lo, hi;
    if (p->stage != STAGE_ADULT) {
        snprintf(label, sizeof label, "Grow up");
        lo = 0, hi = stage_threshold(STAGE_TEEN); /* teen -> adult */
    } else {
        snprintf(label, sizeof label, "Next: Form %d", ready);
        lo = pet_form_threshold(ready - 1), hi = pet_form_threshold(ready);
    }
    format_tokens(p->life_xp, have, sizeof have);
    format_tokens(hi, need, sizeof need);
    snprintf(num, sizeof num, "%s/%s", have, need);
    int lw = 14 * fw, nw = (int)strlen(num) * fw;
    gfx_text(c, FONT_BODY, x, y, DIM, label, 0);
    double frac = hi > lo ? (double)(p->life_xp - lo) / (double)(hi - lo) : 1.0;
    gfx_bar(c, x + lw, y + 5, w - lw - nw - fw, 10, frac, FAINT, GROW_COL);
    gfx_text(c, FONT_BODY, x + w - nw, y, TEXT, num, 0);
}

/* What the previewed form means right now. */
static void status_text(const Pet *p, int sel, char *buf, size_t len) {
    int ready = ready_forms(p), worn = worn_form(p);
    char cur[24];
    form_prose(worn, cur, sizeof cur);
    if (sel >= ready) requirement(p, sel, buf, len);
    else if (sel == worn && p->stage != STAGE_ADULT)
        snprintf(buf, len, "Its current look. New forms begin once it grows up; browse them now.");
    else if (sel == worn) snprintf(buf, len, "This is its current form. Browse to compare another.");
    else snprintf(buf, len, "Ready! A transforms it. Nothing is used up: %s stays switchable.", cur);
}

/* A key cap and its action; dimmed when the action is not available. Returns its width. */
static int draw_button(Canvas *c, int x, int y, const char *key, const char *text, bool on) {
    int fw = gfx_font_w(FONT_BODY), w = 34 + (int)strlen(text) * fw + 10;
    gfx_rect(c, x, y, w, BUTTON_H, PANEL);
    gfx_rect(c, x, y, 26, BUTTON_H, on ? SAY_COL : FAINT);
    gfx_text(c, FONT_BODY, x + (26 - fw) / 2, y + 3, BG, key, 0);
    gfx_text(c, FONT_BODY, x + 34, y + 3, on ? TEXT : FAINT, text, 0);
    return w;
}

void evolution_draw(Canvas *c, const App *app, int64_t now_ms) {
    if (!app->pet_ready) return;
    if (app->rebirth_page) {
        rebirth_draw(c, app, now_ms);
        return;
    }
    const Pet *p = &app->pet;
    int fw = gfx_font_w(FONT_BODY), ready = ready_forms(p), worn = worn_form(p);
    int sel = clamp_form(app->evolution_sel), fresh = count_new(p);
    bool locked = sel >= ready;
    char line[128], name[16], cur[16];

    /* title: FORMS, what this screen is for, and how many forms are ready */
    gfx_text(c, FONT_BODY, 14, TITLE_Y, TEXT, "FORMS", 0);
    snprintf(line, sizeof line, "%d of %d ready", ready, PET_FORMS);
    int right = SCREEN_W - 14 - (int)strlen(line) * fw;
    gfx_text(c, FONT_BODY, right, TITLE_Y, DIM, line, 0);
    if (fresh) {
        char tag[16];
        snprintf(tag, sizeof tag, "%d NEW", fresh);
        right -= (int)strlen(tag) * fw + 12 + fw;
        draw_tag(c, right, TITLE_Y, tag, NEW_COL);
    }
    char settle[16];
    format_tokens(stage_threshold(STAGE_TEEN), settle, sizeof settle); /* teen -> adult */
    if (p->stage != STAGE_ADULT) snprintf(line, sizeof line, "Adult traits settle at %s tokens.", settle);
    else snprintf(line, sizeof line, "Reached forms stay yours.");
    gfx_text_fit(c, FONT_BODY, 14 + 7 * fw, TITLE_Y, FAINT, line, right - fw - (14 + 7 * fw));

    draw_panel(c, p, worn, CUR_X, "CURRENT", GROW_COL, false, now_ms);
    draw_panel(c, p, sel, PRE_X, "PREVIEW", SAY_COL, locked, now_ms);
    draw_info(c, p, sel);
    draw_strip(c, p, sel, now_ms);
    draw_progress(c, p, 14, PROG_Y, SCREEN_W - 28);

    status_text(p, sel, line, sizeof line);
    gfx_text_wrap(c, FONT_BODY, 14, STATUS_Y, SCREEN_W - 28, 2, locked ? DIM : TEXT, line);

    int coat = pet_worn(p, WEAR_SHINY);
    if (now_ms < app->bubble_until_ms) {
        gfx_text_fit(c, FONT_BODY, 14, NOTE_Y, SAY_COL, app->bubble, SCREEN_W - 28);
    } else if (coat >= 0) {
        snprintf(line, sizeof line, "Coat on: %s colors replace its natural diet colors.", pet_unlock_name(coat));
        gfx_text_fit(c, FONT_BODY, 14, NOTE_Y, FAINT, line, SCREEN_W - 28);
    } else if (pet_worn(p, WEAR_HAT) >= 0) {
        gfx_text_fit(c, FONT_BODY, 14, NOTE_Y, FAINT, "Hat on: crowns and upper ears are hidden beneath it.", SCREEN_W - 28);
    }

    form_name(sel, name, sizeof name);
    form_name(worn, cur, sizeof cur);
    if (locked) snprintf(line, sizeof line, "Transform (%s locked)", name);
    else if (sel == worn) snprintf(line, sizeof line, "Transform (already worn)");
    else snprintf(line, sizeof line, "Transform into %s", name);
    int x = 14 + draw_button(c, 14, BUTTON_Y, "A", line, !locked && sel != worn) + 12;
    snprintf(line, sizeof line, "Keep current: %s", cur);
    draw_button(c, x, BUTTON_Y, "B", line, true);
}

const char *evolution_hints(void) { return "D-pad forms L2/R2 jump A wear B back Y DNA/lives X pat"; }

void evolution_open(App *app) {
    if (!app->pet_ready) return;
    Pet *p = &app->pet;
    int ready = ready_forms(p);
    new_after = p->forms_seen;
    app->evolution_sel = new_after + 1 < ready ? new_after + 1 : worn_form(p);
    pet_seen_forms(p);
    app->evolution_open = true;
    app->rebirth_page = 0;
    app->bubble_until_ms = 0;
}

bool evolution_handle(App *app, Button b, int64_t now_ms) {
    if (!app->pet_ready) {
        app->evolution_open = false;
        return false;
    }
    if (app->rebirth_page) return rebirth_handle(app, b, now_ms);
    Pet *p = &app->pet;
    int sel = clamp_form(app->evolution_sel), worn = worn_form(p);
    char msg[64], name[24], need[16];
    switch (b) {
    case PAD_L1:
    case PAD_R1: /* still switches screens */
        app->evolution_open = false;
        app->bubble_until_ms = 0;
        return false;
    case PAD_X: return false; /* still pats */
    case PAD_Y:
        rebirth_open(app, now_ms);
        return true;
    case PAD_B: /* keep the current form */
        app->evolution_open = false;
        app->bubble_until_ms = 0;
        return true;
    case PAD_A:
        if (sel >= ready_forms(p)) { /* show what it needs; nothing changes */
            form_name(sel, name, sizeof name);
            format_tokens(pet_form_threshold(sel), need, sizeof need);
            snprintf(msg, sizeof msg, "%s needs %s tokens.", name, need);
            say(app, msg, now_ms);
            return true;
        }
        form_prose(sel, name, sizeof name);
        if (sel == worn) {
            snprintf(msg, sizeof msg, "Staying in %s.", name);
        } else if (pet_choose_form(p, sel)) {
            if (sel == 0) snprintf(msg, sizeof msg, "Back to its original form!");
            else snprintf(msg, sizeof msg, "Transformed into %s!", name);
        } else { /* defensive: the model refused, so stay open and change nothing */
            snprintf(msg, sizeof msg, "That form can't be worn right now.");
            say(app, msg, now_ms);
            return true;
        }
        say(app, msg, now_ms);
        app->evolution_open = false;
        return true;
    case PAD_LEFT:
    case PAD_UP: sel = (sel + PET_FORMS - 1) % PET_FORMS; break;
    case PAD_RIGHT:
    case PAD_DOWN: sel = (sel + 1) % PET_FORMS; break;
    case PAD_L2: sel = clamp_form(sel - JUMP); break; /* stops at the ends, like the wardrobe pages */
    case PAD_R2: sel = clamp_form(sel + JUMP); break;
    default: return true;
    }
    app->evolution_sel = sel;
    app->bubble_until_ms = 0; /* a stale "Not yet!" shouldn't follow the cursor */
    return true;
}
