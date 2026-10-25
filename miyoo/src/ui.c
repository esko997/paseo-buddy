#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "scene.h"
#include "sprites.h"

#define BG 0x0f1117
#define PANEL 0x1a1d26
#define PANEL_HI 0x2a3040
#define TEXT 0xe8e8ec
#define DIM 0x8a90a0
#define FAINT 0x4a5060
#define ALERT_COL 0xff6b4a

#define HEADER_H 40
#define STRIP_Y 40
#define STRIP_H 124
#define LIST_Y 168
#define ROW_H 46
#define LIST_ROWS 6
#define FOOTER_Y 452

static const uint32_t BUCKET_COL[] = {0xff6b4a, 0xe5484d, 0x46c06f, 0xf5c542, 0x5b6170};

/* ---- pet ----------------------------------------------------------------------------- */

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

static void bar(Canvas *c, int x, int y, int w, int h, double frac, uint32_t col) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx_rect(c, x, y, w, h, FAINT);
    gfx_rect(c, x, y, (int)(w * frac), h, col);
}

static void stat_line(Canvas *c, int x, int y, const char *label, const char *value) {
    gfx_text(c, FONT_BODY, x, y, DIM, label, 0);
    gfx_text(c, FONT_BODY, x + 120, y, TEXT, value, 0);
}

static double xp_frac(const Pet *p, char *buf, size_t len) {
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

static const char *mood_line(const App *app, int64_t now) {
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

/* ---- chrome -------------------------------------------------------------------------- */

static void draw_header(Canvas *c, const App *app, int64_t now_ms) {
    bool flash = app->n_alerts && (now_ms / 500) % 2 && now_ms < app->alert_flash_until_ms;
    gfx_rect(c, 0, 0, SCREEN_W, HEADER_H, flash ? ALERT_COL : 0x161922);
    if (app->n_alerts) {
        char buf[48];
        snprintf(buf, sizeof buf, "%d NEW  (START to clear)", app->n_alerts);
        gfx_text(c, FONT_BODY, 14, 10, flash ? BG : ALERT_COL, buf, 0);
    } else {
        gfx_text(c, FONT_BODY, 14, 10, TEXT, "PASEO PAGER", 0);
    }
    time_t t = (time_t)app_now(app) + (app->clock_synced ? app->tz_offset : 0);
    struct tm tm;
    if (app->clock_synced) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
    char clock[8];
    strftime(clock, sizeof clock, "%H:%M", &tm);
    int cx = SCREEN_W - 14 - 5 * gfx_font_w(FONT_BODY);
    gfx_text(c, FONT_BODY, cx, 10, flash ? BG : TEXT, clock, 0);
    for (int i = 0; i < app->cfg.n_bridges; i++) {
        uint32_t col = app->link[i] == LINK_OK ? (app->feeds[i].daemon_up ? 0x46c06f : 0xf5c542)
                       : app->link[i] == LINK_ERROR ? 0xe5484d
                                                    : FAINT;
        gfx_rect(c, cx - 22 - (app->cfg.n_bridges - 1 - i) * 16, 15, 10, 10, col);
    }
    if (app->battery >= 0 || app->charging) {
        char bat[16];
        bool known = app->battery >= 0 && app->battery <= 100;
        if (app->charging && known) snprintf(bat, sizeof bat, "+%d%%", app->battery);
        else if (app->charging) snprintf(bat, sizeof bat, "PLUG");
        else snprintf(bat, sizeof bat, "%d%%", app->battery > 100 ? 100 : app->battery);
        int bx = cx - 22 - app->cfg.n_bridges * 16 - (int)strlen(bat) * gfx_font_w(FONT_BODY);
        uint32_t col = app->charging ? 0x46c06f : app->battery <= 15 ? ALERT_COL : DIM;
        gfx_text(c, FONT_BODY, bx, 10, flash ? BG : col, bat, 0);
    }
}

static void draw_footer(Canvas *c, const char *hints) {
    gfx_rect(c, 0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, 0x161922);
    gfx_text(c, FONT_BODY, 14, FOOTER_Y + 4, DIM, hints, 0);
}

static void ago(int64_t secs, char *buf, size_t len) {
    if (secs < 60) snprintf(buf, len, "now");
    else if (secs < 3600) snprintf(buf, len, "%lldm", (long long)(secs / 60));
    else if (secs < 86400) snprintf(buf, len, "%lldh", (long long)(secs / 3600));
    else snprintf(buf, len, "%lldd", (long long)(secs / 86400));
}

static const char *short_model(const char *model) {
    const char *slash = strrchr(model, '/');
    return slash ? slash + 1 : model;
}

/* ---- screens ------------------------------------------------------------------------- */

static void draw_strip(Canvas *c, const App *app, int64_t now_ms) {
    gfx_rect(c, 0, STRIP_Y, SCREEN_W, STRIP_H, PANEL);
    if (!app->pet_ready) {
        gfx_text(c, FONT_BODY, 20, STRIP_Y + 50, DIM, "Waiting for a bridge (pet runs on its clock)...", 0);
        return;
    }
    const Pet *p = &app->pet;
    ui_draw_pet(c, app, 16, STRIP_Y + 14, 6, now_ms);
    int x = 136, y = STRIP_Y + 8;
    char line[64];
    snprintf(line, sizeof line, "%s the %s", p->name, stage_name(p->stage));
    gfx_text(c, FONT_BODY, x, y, TEXT, line, 0);
    const char *say = now_ms < app->bubble_until_ms ? app->bubble : mood_line(app, app_now(app));
    gfx_text_fit(c, FONT_BODY, x, y + 24, now_ms < app->bubble_until_ms ? 0xffd166 : DIM, say, SCREEN_W - x - 12);
    char xp[40];
    double frac = xp_frac(p, xp, sizeof xp);
    gfx_text(c, FONT_BODY, x, y + 52, DIM, "GROW", 0);
    bar(c, x + 50, y + 57, 150, 10, frac, 0x8b7fc7);
    gfx_text(c, FONT_BODY, x, y + 78, DIM, xp, 0);
    /* Token throughput, last RATE_MINUTES minutes. */
    gfx_text(c, FONT_BODY, x + 230, y + 52, DIM, "tok/min", 0);
    scene_draw_sparkline(c, app, x + 310, y + 50, SCREEN_W - x - 310 - 14, 48, now_ms);
}

static void draw_agents(Canvas *c, const App *app, int64_t now_ms) {
    draw_strip(c, app, now_ms);
    int64_t now = app_now(app);
    if (app->n_rows == 0) {
        int y = LIST_Y + 20;
        for (int i = 0; i < app->cfg.n_bridges; i++, y += 26) {
            const Bridge *b = &app->cfg.bridges[i];
            char line[160];
            if (app->link[i] == LINK_ERROR)
                snprintf(line, sizeof line, "%s:%d  %s", b->host, b->port, app->link_err[i]);
            else if (app->link[i] == LINK_OK)
                snprintf(line, sizeof line, "%s: %s", app->feeds[i].host,
                         app->feeds[i].daemon_up ? "no agents" : "paseo daemon unreachable");
            else
                snprintf(line, sizeof line, "%s:%d  connecting...", b->host, b->port);
            gfx_text_fit(c, FONT_BODY, 20, y, DIM, line, SCREEN_W - 40);
        }
        return;
    }
    for (int i = 0; i < LIST_ROWS && app->scroll + i < app->n_rows; i++) {
        int idx = app->scroll + i;
        const Row *row = &app->rows[idx];
        const Agent *a = &row->agent;
        int y = LIST_Y + i * ROW_H;
        bool selected = idx == app->sel;
        if (selected) gfx_rect(c, 0, y, SCREEN_W, ROW_H - 2, PANEL_HI);
        gfx_rect(c, 0, y, 8, ROW_H - 2, BUCKET_COL[a->bucket]);
        scene_draw_agent_icon(c, a, row->alert, 14, y + 4, now_ms);
        char tok[16];
        format_tokens(a->out_tokens, tok, sizeof tok);
        int tok_w = (int)strlen(tok) * gfx_font_w(FONT_BODY);
        gfx_text(c, FONT_BODY, SCREEN_W - 14 - tok_w, y + 2, DIM, tok, 0);
        gfx_text_fit(c, FONT_BODY, 36, y + 2, TEXT, a->title, SCREEN_W - 36 - tok_w - 30);
        char when[24], meta[200];
        ago(now - a->updated, when, sizeof when);
        if (app->cfg.n_bridges > 1)
            snprintf(meta, sizeof meta, "%-9s %s  @%.10s  %s", bucket_label(a->bucket), when,
                     app->feeds[row->bridge].host, short_model(a->model));
        else
            snprintf(meta, sizeof meta, "%-9s %s  %s", bucket_label(a->bucket), when, short_model(a->model));
        gfx_text(c, FONT_BODY, 36, y + 22, BUCKET_COL[a->bucket], meta, 9);
        gfx_text_fit(c, FONT_BODY, 36 + 10 * gfx_font_w(FONT_BODY), y + 22, DIM, meta + 10,
                     SCREEN_W - 48 - 10 * gfx_font_w(FONT_BODY));
    }
    if (app->n_rows > LIST_ROWS) {
        int track_h = LIST_ROWS * ROW_H - 4;
        int thumb = track_h * LIST_ROWS / app->n_rows;
        int pos = track_h * app->scroll / app->n_rows;
        gfx_rect(c, SCREEN_W - 4, LIST_Y + pos, 3, thumb, FAINT);
    }
}

static const Row *detail_row(const App *app) {
    for (int i = 0; i < app->n_rows; i++)
        if (app->rows[i].bridge == app->detail_bridge && !strcmp(app->rows[i].agent.id, app->detail_key))
            return &app->rows[i];
    return NULL;
}

static void draw_detail(Canvas *c, const App *app) {
    const Row *row = detail_row(app);
    if (!row) {
        gfx_text(c, FONT_BODY, 20, 80, DIM, "That agent is gone.", 0);
        return;
    }
    const Agent *a = &row->agent;
    gfx_rect(c, 0, HEADER_H, 10, FOOTER_Y - HEADER_H, BUCKET_COL[a->bucket]);
    int x = 26, y = HEADER_H + 12;
    gfx_text(c, FONT_BIG, x, y, BUCKET_COL[a->bucket], bucket_label(a->bucket), 0);
    y += 40;
    y += 22 * gfx_text_wrap(c, FONT_BODY, x, y, SCREEN_W - x - 16, 5, TEXT, a->title) + 14;
    char tok[16], when[24], line[200];
    format_tokens(a->out_tokens, tok, sizeof tok);
    ago(app_now(app) - a->updated, when, sizeof when);
    const char *rows[][2] = {
        {"Host", app->feeds[row->bridge].host}, {"Agent", a->id}, {"Model", a->model},
        {"Status", a->status},                  {"Dir", a->cwd},  {"Wants", a->attention},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++, y += 24) {
        gfx_text(c, FONT_BODY, x, y, DIM, rows[i][0], 0);
        gfx_text_fit(c, FONT_BODY, x + 80, y, TEXT, rows[i][1], SCREEN_W - x - 96);
    }
    snprintf(line, sizeof line, "%s out  $%d.%02d  ctx %d%%  updated %s%s", tok, a->cost_cents / 100,
             a->cost_cents % 100, a->ctx_pct, when, strcmp(when, "now") ? " ago" : "");
    gfx_text(c, FONT_BODY, x, y, DIM, "Usage", 0);
    gfx_text_fit(c, FONT_BODY, x + 80, y, TEXT, line, SCREEN_W - x - 96);
    gfx_text(c, FONT_BODY, x, FOOTER_Y - 30, FAINT, "Read-only. Reply from the Paseo app.", 0);
}

static void draw_pet_screen(Canvas *c, const App *app, int64_t now_ms) {
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
    const char *say = now_ms < app->bubble_until_ms ? app->bubble : mood_line(app, now);
    gfx_text_wrap(c, FONT_BODY, x, y, SCREEN_W - x - 12, 2, 0xffd166, say);
    y += 52;
    char xp[40], val[48], tok[16];
    double frac = xp_frac(p, xp, sizeof xp);
    gfx_text(c, FONT_BODY, x, y, DIM, "GROW", 0);
    bar(c, x + 50, y + 5, 150, 10, frac, 0x8b7fc7);
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
        char tok[16];
        format_tokens(p->diet[i], tok, sizeof tok);
        gfx_text(c, FONT_BODY, 24, y, DIM, family_name((Family)i), 0);
        bar(c, 110, y + 5, 360, 10, (double)p->diet[i] / (double)most, PALETTES[i].body);
        gfx_text(c, FONT_BODY, 484, y, TEXT, tok, 0);
    }
    const char *hint = p->stage == STAGE_ADULT ? "Fully grown. Shape set by its favorite food."
                                               : "Its favorite food decides its adult form.";
    gfx_text(c, FONT_BODY, 24, y + 6, FAINT, hint, 0);
}

void ui_draw(const App *app, const Scene *scene, Canvas *c, int64_t now_ms) {
    gfx_clear(c, BG);
    draw_header(c, app, now_ms);
    switch (app->screen) {
    case SCREEN_AGENTS:
        draw_agents(c, app, now_ms);
        draw_footer(c, "A open  X pat  L/R den+pet  START clear");
        break;
    case SCREEN_DEN:
        scene_draw_den(scene, app, c, now_ms);
        draw_footer(c, "X pat  L/R switch  START clear  MENU quit");
        break;
    case SCREEN_PET:
        draw_pet_screen(c, app, now_ms);
        draw_footer(c, "X pat  L/R switch  START clear  MENU quit");
        break;
    case SCREEN_DETAIL:
        draw_detail(c, app);
        draw_footer(c, "B back  UP/DOWN next agent");
        break;
    }
}
