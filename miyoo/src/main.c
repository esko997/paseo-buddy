#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "audio.h"
#include "platform.h"
#include "scene.h"

#define PLAUSIBLE_EPOCH 1700000000 /* rejects a bridge reporting an unset clock */
#define SAVE_EVERY_S 60
#define BUBBLE_MS 6000
#define CHARGE_POLL_MS 10000

static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t app_now(const App *app) { return (int64_t)time(NULL) + app->clock_offset; }

static void data_dir(char *buf, size_t len) {
    const char *env = getenv("PAGER_DIR");
    if (env) {
        snprintf(buf, len, "%s", env);
        return;
    }
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) {
        snprintf(buf, len, ".");
        return;
    }
    exe[n] = 0;
    snprintf(buf, len, "%s", dirname(exe));
}

static void say(App *app, const char *msg, int64_t now_ms) {
    snprintf(app->bubble, sizeof app->bubble, "%s", msg);
    app->bubble_until_ms = now_ms + BUBBLE_MS;
}

static void emit(App *app, EventKind kind, int bridge, const char *agent_id, Family family, int64_t amount,
                 int64_t now_ms) {
    Event *e = &app->events[app->n_events % EVENT_RING];
    e->kind = kind;
    e->bridge = bridge;
    snprintf(e->agent_id, sizeof e->agent_id, "%s", agent_id ? agent_id : "");
    e->family = family;
    e->amount = amount;
    e->at_ms = now_ms;
    app->n_events++;
}

/* Sound is opt-out (volume = 0) and stays silent overnight unless quiet_nights = 0. */
static void cue(const App *app, Sfx s) {
    if (app->cfg.volume <= 0) return;
    if (app->cfg.quiet_nights && app->clock_synced && pet_asleep(app_now(app), app->tz_offset)) return;
    audio_play(s);
}

/* ---- token rate history ------------------------------------------------------------- */

static void rate_roll(App *app) {
    if (!app->clock_synced) return;
    int64_t minute = app_now(app) / 60;
    int64_t shift = minute - app->rate_minute;
    if (shift <= 0) return;
    if (shift >= RATE_MINUTES) {
        memset(app->rate, 0, sizeof app->rate);
    } else {
        memmove(app->rate, app->rate + shift, sizeof app->rate[0] * (size_t)(RATE_MINUTES - shift));
        memset(app->rate + RATE_MINUTES - shift, 0, sizeof app->rate[0] * (size_t)shift);
    }
    app->rate_minute = minute;
}

/* ---- rumble patterns ------------------------------------------------------------------ */

static struct {
    int toggles_left;
    bool on;
    int64_t next_ms;
} g_rumble;

static void buzz(const App *app, int pulses, int64_t now_ms) {
    if (!app->cfg.rumble) return;
    g_rumble.toggles_left = pulses * 2;
    g_rumble.on = false;
    g_rumble.next_ms = now_ms;
}

static void rumble_update(int64_t now_ms) {
    if (!g_rumble.toggles_left || now_ms < g_rumble.next_ms) return;
    g_rumble.on = !g_rumble.on;
    plat_rumble(g_rumble.on);
    g_rumble.toggles_left--;
    g_rumble.next_ms = now_ms + (g_rumble.on ? 140 : 110);
}

/* ---- feed merging & alerts ------------------------------------------------------------ */

static Track *find_track(App *app, int bridge, const char *id) {
    for (int i = 0; i < app->n_track; i++)
        if (app->track[i].bridge == bridge && !strcmp(app->track[i].id, id)) return &app->track[i];
    return NULL;
}

static int row_cmp(const void *pa, const void *pb) {
    const Row *a = pa, *b = pb;
    if (a->agent.bucket != b->agent.bucket) return (int)a->agent.bucket - (int)b->agent.bucket;
    if (a->alert != b->alert) return a->alert ? -1 : 1;
    return a->agent.updated < b->agent.updated ? 1 : a->agent.updated > b->agent.updated ? -1 : 0;
}

static void rebuild_rows(App *app) {
    char keep_id[8] = {0};
    int keep_bridge = -1;
    if (app->sel < app->n_rows) {
        snprintf(keep_id, sizeof keep_id, "%s", app->rows[app->sel].agent.id);
        keep_bridge = app->rows[app->sel].bridge;
    }
    app->n_rows = app->n_running = app->n_alerts = 0;
    for (int b = 0; b < app->cfg.n_bridges; b++) {
        if (!app->have_feed[b]) continue;
        for (int i = 0; i < app->feeds[b].n_agents && app->n_rows < MAX_ROWS; i++) {
            Row *r = &app->rows[app->n_rows++];
            r->bridge = b;
            r->agent = app->feeds[b].agents[i];
            const Track *t = find_track(app, b, r->agent.id);
            r->alert = t && t->alert;
            if (r->alert) app->n_alerts++;
            if (r->agent.bucket == B_RUNNING) app->n_running++;
        }
    }
    qsort(app->rows, (size_t)app->n_rows, sizeof app->rows[0], row_cmp);
    app->sel = 0;
    for (int i = 0; i < app->n_rows; i++)
        if (app->rows[i].bridge == keep_bridge && !strcmp(app->rows[i].agent.id, keep_id)) app->sel = i;
    if (app->scroll > app->sel) app->scroll = app->sel;
    if (app->sel >= app->scroll + 6) app->scroll = app->sel - 5;
    if (app->scroll > 0 && app->scroll > app->n_rows - 6) app->scroll = app->n_rows > 6 ? app->n_rows - 6 : 0;
}

/* The pet only lives on bridge time: the Miyoo's own clock may be days off. */
static void ensure_pet(App *app) {
    if (app->pet_ready || !app->clock_synced) return;
    if (!pet_load(&app->pet, app->pet_path)) pet_init(&app->pet, app->cfg.pet_name, app_now(app));
    snprintf(app->pet.name, sizeof app->pet.name, "%s", app->cfg.pet_name);
    app->pet_ready = true;
}

typedef struct {
    int raised;
    Bucket worst; /* most urgent bucket among newly raised alerts */
} FeedOutcome;

static FeedOutcome apply_feed(App *app, int bi, const Feed *f, int64_t now_ms) {
    FeedOutcome out = {0, B_DONE};
    bool first = !app->have_feed[bi];
    const Agent *first_alert = NULL;
    for (int i = 0; i < app->n_track; i++)
        if (app->track[i].bridge == bi) app->track[i].present = false;
    for (int i = 0; i < f->n_agents; i++) {
        const Agent *a = &f->agents[i];
        bool urgent = a->bucket == B_NEEDS_INPUT || a->bucket == B_FAILED;
        Track *t = find_track(app, bi, a->id);
        bool raise = false;
        if (!t) {
            if (app->n_track == MAX_ROWS) continue;
            t = &app->track[app->n_track++];
            memset(t, 0, sizeof *t);
            t->bridge = bi;
            snprintf(t->id, sizeof t->id, "%s", a->id);
            t->out_tokens = a->out_tokens;
            raise = !first && urgent;
        } else {
            if (a->out_tokens > t->out_tokens) {
                /* Live throughput only: the pet's first meal (a bridge's whole backlog) is not a rate. */
                int64_t delta = a->out_tokens - t->out_tokens;
                emit(app, EV_TOKENS, bi, a->id, a->family, delta, now_ms);
                rate_roll(app);
                app->rate[RATE_MINUTES - 1] += delta;
            }
            bool finished = t->bucket == B_RUNNING && a->bucket != B_RUNNING;
            raise = (urgent && t->bucket != a->bucket) || finished;
            /* Back to work means you already handled it (e.g. replied at the desk). */
            if (a->bucket == B_RUNNING && t->bucket != B_RUNNING) t->alert = false;
            t->out_tokens = a->out_tokens;
        }
        if (raise) {
            EventKind k = a->bucket == B_NEEDS_INPUT ? EV_NEEDS_YOU : a->bucket == B_FAILED ? EV_FAILED : EV_FINISHED;
            emit(app, k, bi, a->id, a->family, 0, now_ms);
        }
        t->bucket = a->bucket;
        t->present = true;
        if (raise && !t->alert) {
            t->alert = true;
            out.raised++;
            if (a->bucket < out.worst) out.worst = a->bucket;
            if (!first_alert) first_alert = a;
        }
    }
    int keep = 0;
    for (int i = 0; i < app->n_track; i++)
        if (app->track[i].bridge != bi || app->track[i].present) app->track[keep++] = app->track[i];
    app->n_track = keep;

    app->feeds[bi] = *f;
    app->have_feed[bi] = true;
    if (f->now > PLAUSIBLE_EPOCH) {
        app->clock_offset = f->now - (int64_t)time(NULL);
        app->tz_offset = f->tz_offset;
        app->clock_synced = true;
    }
    ensure_pet(app);
    if (app->pet_ready) {
        FeedResult r = pet_feed(&app->pet, f, app_now(app), app->tz_offset);
        char msg[64];
        if (r.hatched) {
            snprintf(msg, sizeof msg, "Hatched! Hi, I'm %s!", app->pet.name);
            say(app, msg, now_ms);
            emit(app, EV_HATCHED, -1, NULL, pet_favorite(&app->pet), 0, now_ms);
            cue(app, SFX_HATCH);
        } else if (r.evolved) {
            snprintf(msg, sizeof msg, "Evolved into a %s!", stage_name(app->pet.stage));
            say(app, msg, now_ms);
            emit(app, EV_EVOLVED, -1, NULL, pet_favorite(&app->pet), app->pet.stage, now_ms);
            cue(app, SFX_EVOLVE);
        } else if (r.tokens > 0 && !first_alert) {
            char tok[16];
            format_tokens(r.tokens, tok, sizeof tok);
            snprintf(msg, sizeof msg, "Nom! +%s tokens", tok);
            say(app, msg, now_ms);
            if (!out.raised) cue(app, SFX_NOM);
        }
    }
    if (first_alert) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s: %.40s", bucket_label(first_alert->bucket), first_alert->title);
        say(app, msg, now_ms);
        cue(app, out.worst == B_NEEDS_INPUT ? SFX_NEEDS_YOU : out.worst == B_FAILED ? SFX_FAILED : SFX_FINISHED);
    }
    rebuild_rows(app);
    return out;
}

static void ack_all(App *app) {
    for (int i = 0; i < app->n_track; i++) app->track[i].alert = false;
    rebuild_rows(app);
}

static void ack_one(App *app, int bridge, const char *id) {
    Track *t = find_track(app, bridge, id);
    if (t && t->alert) {
        t->alert = false;
        rebuild_rows(app);
    }
}

static void open_detail(App *app, int idx) {
    if (idx < 0 || idx >= app->n_rows) return;
    snprintf(app->detail_key, sizeof app->detail_key, "%s", app->rows[idx].agent.id);
    app->detail_bridge = app->rows[idx].bridge;
    app->screen = SCREEN_DETAIL;
    ack_one(app, app->detail_bridge, app->detail_key);
    for (int i = 0; i < app->n_rows; i++)
        if (app->rows[i].bridge == app->detail_bridge && !strcmp(app->rows[i].agent.id, app->detail_key)) app->sel = i;
}

static void move_sel(App *app, int delta) {
    if (!app->n_rows) return;
    app->sel += delta;
    if (app->sel < 0) app->sel = 0;
    if (app->sel >= app->n_rows) app->sel = app->n_rows - 1;
    if (app->sel < app->scroll) app->scroll = app->sel;
    if (app->sel >= app->scroll + 6) app->scroll = app->sel - 5;
}

static void pat(App *app, int64_t now_ms) {
    if (!app->pet_ready) return;
    static const char *REPLIES[] = {"<3", "Hehe!", "*happy wiggle*", "Thanks!", "^_^"};
    static unsigned n;
    if (app->pet.stage == STAGE_EGG) {
        say(app, "*the egg wiggles*", now_ms);
    } else {
        pet_pat(&app->pet, app_now(app));
        say(app, REPLIES[n++ % (sizeof REPLIES / sizeof REPLIES[0])], now_ms);
    }
    emit(app, EV_PAT, -1, NULL, pet_favorite(&app->pet), 0, now_ms);
    cue(app, SFX_PAT);
}

static const Screen CYCLE[] = {SCREEN_AGENTS, SCREEN_DEN, SCREEN_PET};

static void cycle_screen(App *app, int dir) {
    int n = (int)(sizeof CYCLE / sizeof CYCLE[0]), cur = 0;
    for (int i = 0; i < n; i++)
        if (CYCLE[i] == app->screen) cur = i;
    app->screen = CYCLE[(cur + dir + n) % n];
}

/* Returns false to quit. */
static bool handle(App *app, Button b, int64_t now_ms) {
    if (b == PAD_QUIT || b == PAD_MENU) return false;
    if (b == PAD_START) {
        ack_all(app);
        return true;
    }
    if (app->screen == SCREEN_DETAIL) {
        if (b == PAD_B || b == PAD_A) app->screen = SCREEN_AGENTS;
        else if (b == PAD_UP) open_detail(app, app->sel - 1);
        else if (b == PAD_DOWN) open_detail(app, app->sel + 1);
        return true;
    }
    if (b == PAD_X || b == PAD_Y) pat(app, now_ms);
    else if (b == PAD_R1) cycle_screen(app, 1);
    else if (b == PAD_L1) cycle_screen(app, -1);
    else if (app->screen == SCREEN_AGENTS) {
        if (b == PAD_UP) move_sel(app, -1);
        else if (b == PAD_DOWN) move_sel(app, 1);
        else if (b == PAD_L2) move_sel(app, -6);
        else if (b == PAD_R2) move_sel(app, 6);
        else if (b == PAD_A) open_detail(app, app->sel);
        else if (b == PAD_RIGHT) cycle_screen(app, 1);
    } else {
        if (b == PAD_B) app->screen = SCREEN_AGENTS;
        else if (b == PAD_RIGHT) cycle_screen(app, 1);
        else if (b == PAD_LEFT) cycle_screen(app, -1);
        else if (b == PAD_A) pat(app, now_ms);
    }
    return true;
}

/* ---- --shot smoke mode: fetch once, simulate, write one frame as PPM ------------------ */

static int write_ppm(const Canvas *c, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        unsigned char rgb[3] = {(unsigned char)(c->px[i] >> 16), (unsigned char)(c->px[i] >> 8),
                                (unsigned char)c->px[i]};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f);
}

static int run_shot(App *app, const char *out, const char *screen, int sel, int sim_ms) {
    int64_t t0 = mono_ms();
    for (int i = 0; i < app->cfg.n_bridges; i++) {
        char *body = NULL;
        Feed *f = malloc(sizeof *f);
        if (!bridge_fetch_once(&app->cfg.bridges[i], &body, app->link_err[i], sizeof app->link_err[i])) {
            app->link[i] = LINK_ERROR;
            fprintf(stderr, "bridge %d: %s\n", i, app->link_err[i]);
        } else if (feed_parse(body, f)) {
            app->link[i] = LINK_OK;
            apply_feed(app, i, f, t0);
        }
        free(body);
        free(f);
    }
    app->battery = plat_battery();
    if (!strcmp(screen, "pet")) app->screen = SCREEN_PET;
    else if (!strcmp(screen, "den")) app->screen = SCREEN_DEN;
    else if (!strcmp(screen, "detail")) open_detail(app, sel);
    if (strcmp(screen, "detail") != 0) move_sel(app, sel);
    /* Step the scene on a fake clock so animations have something to show. */
    Scene *scene = scene_create();
    for (int t = 0; t <= sim_ms; t += 33) scene_update(scene, app, t0 + t);
    static Canvas canvas;
    ui_draw(app, scene, &canvas, t0 + sim_ms);
    scene_destroy(scene);
    if (write_ppm(&canvas, out) != 0) {
        fprintf(stderr, "cannot write %s: %s\n", out, strerror(errno));
        return 1;
    }
    printf("rows=%d running=%d stage=%s xp=%lld bubble=%s\n", app->n_rows, app->n_running,
           app->pet_ready ? stage_name(app->pet.stage) : "-", (long long)app->pet.xp, app->bubble);
    return 0;
}

int main(int argc, char **argv) {
    static App app;
    const char *shot = NULL, *screen = "agents";
    int sel = 0, sim_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) screen = argv[++i];
        else if (!strcmp(argv[i], "--sel") && i + 1 < argc) sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sim-ms") && i + 1 < argc) sim_ms = atoi(argv[++i]);
    }
    char dir[480], cfg_path[600], err[160];
    data_dir(dir, sizeof dir);
    snprintf(cfg_path, sizeof cfg_path, "%s/pager.cfg", dir);
    snprintf(app.pet_path, sizeof app.pet_path, "%s/pet.txt", dir);
    bool cfg_ok = config_load(cfg_path, &app.cfg, err, sizeof err);
    if (!cfg_ok) fprintf(stderr, "config: %s\n", err);
    if (shot) return cfg_ok ? run_shot(&app, shot, screen, sel, sim_ms) : 2;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!plat_init()) return 1;
    static Canvas canvas;
    if (!cfg_ok) {
        /* Nothing to poll; show what's wrong until any button is pressed. */
        gfx_clear(&canvas, 0x0f1117);
        gfx_text(&canvas, FONT_BIG, 20, 40, 0xff6b4a, "PASEO PAGER", 0);
        gfx_text_wrap(&canvas, FONT_BODY, 20, 100, 600, 6, 0xe8e8ec, err);
        gfx_text(&canvas, FONT_BODY, 20, 420, 0x8a90a0, "Copy pager.cfg.example to pager.cfg", 0);
        plat_present(canvas.px);
        while (g_running && plat_poll() == PAD_NONE) usleep(50000);
        plat_shutdown();
        return 2;
    }
    if (app.cfg.volume > 0 && !audio_init(app.cfg.volume)) fprintf(stderr, "audio: no output device\n");
    Scene *scene = scene_create();
    net_start(&app.cfg);

    int64_t last_render = 0, last_tick = 0, last_input = mono_ms(), last_battery = -60000, last_charge = -60000;
    bool screen_on = true;
    while (g_running) {
        int64_t now_ms = mono_ms();
        bool dirty = false;
        for (int i = 0; i < app.cfg.n_bridges; i++) {
            LinkState st;
            char *body = bridge_take(&app.cfg.bridges[i], &app.gen[i], &st, app.link_err[i], sizeof app.link_err[i]);
            if (st != app.link[i]) dirty = true;
            app.link[i] = st;
            if (!body) continue;
            Feed *f = malloc(sizeof *f);
            if (f && feed_parse(body, f) && apply_feed(&app, i, f, now_ms).raised > 0) {
                buzz(&app, 2, now_ms);
                app.alert_flash_until_ms = now_ms + 10000;
                last_input = now_ms;
                if (!screen_on) {
                    plat_backlight(true);
                    screen_on = true;
                }
            }
            free(f);
            free(body);
            dirty = true;
        }
        if (app.pet_ready && now_ms - last_tick >= 1000) {
            rate_roll(&app);
            pet_roll_day(&app.pet, app_now(&app), app.tz_offset);
            last_tick = now_ms;
            if (app_now(&app) - app.last_save >= SAVE_EVERY_S) {
                pet_save(&app.pet, app.pet_path);
                app.last_save = app_now(&app);
            }
        }
        if (now_ms - last_battery >= 30000) {
            app.battery = plat_battery();
            last_battery = now_ms;
        }
        if (now_ms - last_charge >= CHARGE_POLL_MS) {
            bool was = app.charging;
            app.charging = app.cfg.awake_on_power && plat_charging();
            last_charge = now_ms;
            if (app.charging && !was) {
                last_input = now_ms;
                if (!screen_on) {
                    plat_backlight(true);
                    screen_on = true;
                }
            }
            dirty |= app.charging != was;
        }
        for (Button b; (b = plat_poll()) != PAD_NONE;) {
            last_input = now_ms;
            if (!screen_on) { /* the waking press is swallowed */
                plat_backlight(true);
                screen_on = true;
                dirty = true;
                continue;
            }
            if (b == PAD_SELECT) {
                plat_backlight(false);
                screen_on = false;
                continue;
            }
            if (!handle(&app, b, now_ms)) g_running = 0;
            dirty = true;
        }
        rumble_update(now_ms);
        scene_update(scene, &app, now_ms);
        bool idle_timeout = app.cfg.screen_timeout_s > 0 && now_ms - last_input > app.cfg.screen_timeout_s * 1000LL;
        if (screen_on && !app.charging && idle_timeout) {
            plat_backlight(false);
            screen_on = false;
        }
        int frame_ms = app.screen == SCREEN_DEN ? 33 : 66;
        if (screen_on && (dirty || now_ms - last_render >= frame_ms)) {
            ui_draw(&app, scene, &canvas, now_ms);
            plat_present(canvas.px);
            last_render = now_ms;
        }
        usleep(screen_on ? 10000 : 50000);
    }
    if (app.pet_ready) pet_save(&app.pet, app.pet_path);
    scene_destroy(scene);
    audio_shutdown();
    plat_rumble(false);
    plat_backlight(true);
    plat_shutdown();
    return 0;
}
