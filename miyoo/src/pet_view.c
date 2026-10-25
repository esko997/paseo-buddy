#include "pet_view.h"

#include <stdio.h>
#include <string.h>

#include "sprites.h"
#include "theme.h"

static int pet_palette(const Pet *p) {
    if (p->stage == STAGE_ADULT) return p->adult_family;
    int64_t fed = p->diet[0] + p->diet[1] + p->diet[2] + p->diet[3];
    return fed ? (int)pet_favorite(p) : PAL_UNFED;
}

static void blit_layer(Canvas *c, const Sprite spr, int x, int y, int s, const Palette *pal, Mood mood, bool blink,
                       bool chomp) {
    for (int r = 0; r < 16; r++) {
        for (int col = 0; col < 16; col++) {
            char ch = spr[r][col];
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
                bool upper = r + 1 < 16 && spr[r + 1][col] == 'E';
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

void ui_draw_pet(Canvas *c, const App *app, int x, int y, int s, int64_t now_ms) {
    const Pet *p = &app->pet;
    int64_t now = app_now(app);
    Mood mood = pet_mood(p, now, app->tz_offset, app->n_alerts, app->n_running);
    const Palette *pal = &PALETTES[pet_palette(p)];
    int tick = (int)(now_ms / 125); /* 8 Hz animation clock */
    bool blink = (now_ms / 100) % 37 == 0;
    bool chomp = mood == MOOD_EATING && (tick / 2) % 2;
    int bob = 0, wobble = 0;
    if (p->stage == STAGE_EGG) {
        if (mood == MOOD_ALERT || (tick / 4) % 6 == 0) wobble = ((tick / 2) % 2 ? 1 : -1) * (s / 2);
    } else if (mood == MOOD_ALERT) {
        bob = (tick % 2) ? -s : 0;
    } else if (mood != MOOD_SLEEP) {
        bob = ((tick / 4) % 2) ? -s / 2 : 0;
    }
    int px = x + wobble, py = y + bob;
    if (p->stage == STAGE_EGG) {
        blit_layer(c, SPR_EGG, px, py, s, pal, mood, false, false);
    } else if (p->stage == STAGE_BABY) {
        blit_layer(c, SPR_BABY, px, py, s, pal, mood, blink, chomp);
    } else {
        blit_layer(c, SPR_KID, px, py, s, pal, mood, blink, chomp);
        if (p->stage == STAGE_TEEN) blit_layer(c, OVL_ANTENNA, px, py, s, pal, mood, false, false);
        if (p->stage == STAGE_ADULT) {
            blit_layer(c, OVL_CREST[p->adult_family], px, py, s, pal, mood, false, false);
            blit_layer(c, OVL_WINGS, px, py, s, pal, mood, false, false);
        }
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
        gfx_text(c, FONT_BIG, right - 3 * s, y, pal->accent, dots, 0);
        break;
    }
    case MOOD_HAPPY:
        if ((tick / 8) % 2) gfx_text(c, FONT_BODY, right - 3 * s, y, COL_CHEEK, "<3", 0);
        break;
    default:
        break;
    }
}

static void stat_line(Canvas *c, int x, int y, const char *label, const char *value) {
    gfx_text(c, FONT_BODY, x, y, DIM, label, 0);
    gfx_text(c, FONT_BODY, x + 120, y, TEXT, value, 0);
}

double pet_xp_progress(const Pet *p, char *buf, size_t len) {
    int64_t next = stage_threshold(p->stage);
    char have[16], need[16];
    format_tokens(p->xp, have, sizeof have);
    if (!next) {
        snprintf(buf, len, "%s tok", have);
        return 1;
    }
    int64_t prev = p->stage == STAGE_EGG ? 0 : stage_threshold((Stage)(p->stage - 1));
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

void pet_screen_draw(Canvas *c, const App *app, int64_t now_ms) {
    if (!app->pet_ready) {
        gfx_text(c, FONT_BODY, 20, 200, DIM, "Waiting for a bridge (pet runs on its clock)...", 0);
        return;
    }
    const Pet *p = &app->pet;
    int64_t now = app_now(app);
    static const int SCALE[N_STAGES] = {9, 9, 10, 11, 12};
    int s = SCALE[p->stage], size = 16 * s;
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
    stat_line(c, x, y + 28, "Today", val);
    format_tokens(p->best_day_tokens, tok, sizeof tok);
    stat_line(c, x, y + 52, "Best day", tok);
    snprintf(val, sizeof val, "%lld", (long long)p->lifetime_turns);
    stat_line(c, x, y + 76, "Turns ever", val);

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
    const char *hint = p->stage == STAGE_ADULT ? "Fully grown. Shape set by its favorite food."
                                               : "Its favorite food decides its adult form.";
    gfx_text(c, FONT_BODY, 24, y + 6, FAINT, hint, 0);
}

const char *pet_screen_hints(const App *app) {
    (void)app;
    return "X pat  L/R switch  START clear  MENU quit";
}

bool pet_screen_handle(App *app, Button b, int64_t now_ms) {
    (void)app, (void)b, (void)now_ms;
    return false;
}

void pet_draw_den_decor(Canvas *c, const App *app, int64_t now_ms) { (void)c, (void)app, (void)now_ms; }
