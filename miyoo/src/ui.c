#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "pet_view.h"
#include "scene.h"
#include "theme.h"

#define STRIP_Y 40
#define STRIP_H 124
#define LIST_Y 168
#define ROW_H 46
#define LIST_ROWS 6

/* Always-on flair: the screen sits on a desk for hours, so nothing stays put for long. */
#define ORBIT_STEP_MS 150000 /* whole-UI pixel orbit: one 1 px step every 2.5 minutes */
#define TITLE_IDLE_MS 170000 /* one title flourish per slot: letters wave, or the pet peeks in */
#define TITLE_IDLE_AT 40000  /* ...this far into the slot */
#define WAVE_MS 1400
#define PEEK_MS 4200
#define SHIMMER_MS 9000 /* selection bar sheen, one pass per period */
#define SHIMMER_PASS 1600

/* ---- chrome -------------------------------------------------------------------------- */

/* Title; in even slots its letters wave once (skipped when calm at night: it draws at ~5 fps). */
static void draw_title(Canvas *c, const App *app, int64_t now_ms) {
    static const char title[] = "PASEO PAGER";
    int64_t age = now_ms % TITLE_IDLE_MS - TITLE_IDLE_AT;
    bool wave = (now_ms / TITLE_IDLE_MS) % 2 == 0 && !app->night_dim && age >= 0 && age < WAVE_MS;
    if (!wave) {
        gfx_text(c, FONT_BODY, 14, 10, TEXT, title, 0);
        return;
    }
    for (int i = 0; title[i]; i++) {
        int p = (int)age - i * 70, dy = 0;
        if (p >= 0 && p < 360) dy = -(3 * 4 * p * (360 - p)) / (360 * 360);
        gfx_text(c, FONT_BODY, 14 + i * gfx_font_w(FONT_BODY), 10 + dy, TEXT, title + i, 1);
    }
}

/* In odd slots the pet peeks up from behind the header's bottom edge. Drawn before the screen
 * body, which covers everything below HEADER_H, so only call it for screens that fill that band. */
static void draw_peek(Canvas *c, const App *app, int64_t now_ms) {
    int64_t slot = now_ms / TITLE_IDLE_MS, age = now_ms % TITLE_IDLE_MS - TITLE_IDLE_AT;
    if (slot % 2 == 0 || age < 0 || age >= PEEK_MS || !app->pet_ready || app->n_alerts) return;
    const int full = 20;
    int rise = age < 600 ? (int)age * full / 600 : age > PEEK_MS - 600 ? (int)(PEEK_MS - age) * full / 600 : full;
    uint32_t h = (uint32_t)slot * 2654435761u;
    int x = 170 + (int)((h >> 16) % 150);
    ui_draw_pet(c, app, x, HEADER_H - rise, 2, now_ms);
}

/* Shifts the finished frame by (dx, dy) in place, filling the uncovered edges with BG. One memmove:
 * pixels that wrap across a row end land in the edge columns, which get filled afterwards. */
static void orbit_shift(Canvas *c, int dx, int dy) {
    long off = (long)dy * SCREEN_W + dx;
    if (!off) return;
    size_t n = (size_t)(SCREEN_W * SCREEN_H - (off < 0 ? -off : off));
    if (off > 0) memmove(c->px + off, c->px, n * sizeof c->px[0]);
    else memmove(c->px, c->px - off, n * sizeof c->px[0]);
    if (dy > 0) gfx_rect(c, 0, 0, SCREEN_W, dy, BG);
    if (dy < 0) gfx_rect(c, 0, SCREEN_H + dy, SCREEN_W, -dy, BG);
    if (dx > 0) gfx_rect(c, 0, 0, dx, SCREEN_H, BG);
    if (dx < 0) gfx_rect(c, SCREEN_W + dx, 0, -dx, SCREEN_H, BG);
}

/* A closed loop over [-2, 2]^2 moving one pixel (Chebyshev) per step. */
static const int8_t ORBIT[][2] = {{0, 0},  {1, 0},   {2, 1},   {2, 2},   {1, 2},  {0, 1},  {-1, 2}, {-2, 2}, {-2, 1},
                                  {-1, 0}, {-2, -1}, {-2, -2}, {-1, -2}, {0, -1}, {1, -2}, {2, -2}, {1, -1}};

static void draw_header(Canvas *c, const App *app, int64_t now_ms) {
    bool flash = app->n_alerts && (now_ms / 500) % 2 && now_ms < app->alert_flash_until_ms;
    gfx_rect(c, 0, 0, SCREEN_W, HEADER_H, flash ? ALERT_COL : CHROME);
    if (app->n_alerts) {
        char buf[48];
        snprintf(buf, sizeof buf, "%d NEW  (START to clear)", app->n_alerts);
        gfx_text(c, FONT_BODY, 14, 10, flash ? BG : ALERT_COL, buf, 0);
    } else {
        draw_title(c, app, now_ms);
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
    gfx_rect(c, 0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, CHROME);
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
    const char *say = now_ms < app->bubble_until_ms ? app->bubble : pet_mood_line(app, app_now(app));
    gfx_text_fit(c, FONT_BODY, x, y + 24, now_ms < app->bubble_until_ms ? SAY_COL : DIM, say, SCREEN_W - x - 12);
    char xp[40];
    double frac = pet_xp_progress(p, xp, sizeof xp);
    gfx_text(c, FONT_BODY, x, y + 52, DIM, "GROW", 0);
    gfx_bar(c, x + 50, y + 57, 150, 10, frac, FAINT, GROW_COL);
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
        if (selected) {
            gfx_rect(c, 0, y, SCREEN_W, ROW_H - 2, PANEL_HI);
            /* now and then a slow, slanted sheen sweeps across the selection while all is calm */
            int64_t p = now_ms % SHIMMER_MS;
            if (p < SHIMMER_PASS && !app->n_alerts && !app->night_dim) {
                int cx = -60 + (int)(p * (SCREEN_W + 120) / SHIMMER_PASS);
                uint32_t s1 = lerp_col(PANEL_HI, 0x6a78a0, 50), s2 = lerp_col(PANEL_HI, 0x8a98c8, 90);
                for (int r = 0; r < ROW_H - 2; r += 2) {
                    int sx = cx + (ROW_H - r) / 2;
                    gfx_rect(c, sx - 12, y + r, 24, 2, s1);
                    gfx_rect(c, sx - 5, y + r, 10, 2, s2);
                }
            }
        }
        gfx_rect(c, 0, y, 8, ROW_H - 2, BUCKET_COL[a->bucket]);
        scene_draw_agent_icon(c, a, row->alert, 14, y + 4, now_ms);
        char tok[16];
        format_tokens(a->out_tokens, tok, sizeof tok);
        int tok_w = (int)strlen(tok) * gfx_font_w(FONT_BODY);
        gfx_text(c, FONT_BODY, SCREEN_W - 14 - tok_w, y + 2, DIM, tok, 0);
        /* context window fill under the token count */
        gfx_bar(c, SCREEN_W - 14 - 24, y + 27, 24, 4, a->ctx_pct / 100.0, FAINT, ctx_col(a->ctx_pct));
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
                     SCREEN_W - 48 - 34 - 10 * gfx_font_w(FONT_BODY));
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
    snprintf(line, sizeof line, "%s out  $%d.%02d  updated %s%s", tok, a->cost_cents / 100, a->cost_cents % 100, when,
             strcmp(when, "now") ? " ago" : "");
    gfx_text(c, FONT_BODY, x, y, DIM, "Usage", 0);
    gfx_text_fit(c, FONT_BODY, x + 80, y, TEXT, line, SCREEN_W - x - 96);
    y += 24;
    gfx_text(c, FONT_BODY, x, y, DIM, "Context", 0);
    gfx_bar(c, x + 80, y + 5, 200, 10, a->ctx_pct / 100.0, FAINT, ctx_col(a->ctx_pct));
    snprintf(line, sizeof line, "%d%%", a->ctx_pct);
    gfx_text(c, FONT_BODY, x + 292, y, a->ctx_pct >= CTX_WARN ? ctx_col(a->ctx_pct) : TEXT, line, 0);
    gfx_text(c, FONT_BODY, x, FOOTER_Y - 30, FAINT, "Read-only. Reply from the Paseo app.", 0);
}

void ui_draw(const App *app, const Scene *scene, Canvas *c, int64_t now_ms) {
    gfx_clear(c, BG);
    draw_header(c, app, now_ms);
    switch (app->screen) {
    case SCREEN_AGENTS:
        draw_peek(c, app, now_ms);
        draw_agents(c, app, now_ms);
        draw_footer(c, "A open  X pat  L/R den+pet  START clear");
        break;
    case SCREEN_DEN:
        draw_peek(c, app, now_ms);
        scene_draw_den(scene, app, c, now_ms);
        draw_footer(c, "X pat  L/R switch  START clear  MENU quit");
        break;
    case SCREEN_PET:
        pet_screen_draw(c, app, now_ms);
        draw_footer(c, pet_screen_hints(app));
        break;
    case SCREEN_DETAIL:
        draw_detail(c, app);
        draw_footer(c, "B back  UP/DOWN next agent");
        break;
    }
    if (app->offline != ONLINE) {
        /* Connection-lost banner over the top of the body, on every screen. */
        const char *msg = app->offline == OFFLINE_WIFI ? "Wi-Fi is off: switch it on in Onion's menu"
                                                       : "Can't reach any bridge (Wi-Fi is on)";
        int y = HEADER_H + 6, h = 30;
        bool pulse = (now_ms / 700) % 2;
        gfx_rect(c, 12, y, SCREEN_W - 24, h, pulse ? 0x5a1f1a : 0x3d1714);
        gfx_frame(c, 12, y, SCREEN_W - 24, h, 2, ALERT_COL);
        gfx_text_fit(c, FONT_BODY, 24, y + 5, TEXT, msg, SCREEN_W - 48);
    }
    /* anti-ghosting: the whole frame drifts around a +-2 px orbit */
    const int8_t *o = ORBIT[now_ms / ORBIT_STEP_MS % (int64_t)(sizeof ORBIT / sizeof ORBIT[0])];
    orbit_shift(c, o[0], o[1]);
}
