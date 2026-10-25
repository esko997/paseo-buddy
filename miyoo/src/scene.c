/* The den: a cozy pixel-art room where every visible agent is a little robot at a desk and the
 * pet wanders the rug eating the token crumbs they produce. An ATM in the corner shows today's
 * tokens and estimated cost; a wall panel takes turns between tok/min and subscription fuel, and
 * robots whose provider runs low on fuel look tired. Everything is gfx_rect/gfx_text level
 * drawing; animation state lives in a fixed particle pool, positions are analytic in age so
 * drawing never mutates the scene.
 * The den is on screen for hours on a desk, so something small always moves: ambient events every
 * few minutes (a bird or cat at the window, the pet tidying, a robot stretching) and a sunlight
 * patch that drifts with the time of day. With app->night_dim everything gets calmer and sparser.
 * Equipped wallpaper, window view and per-family robot costumes come from appearance.c; with none
 * equipped the den is drawn exactly as before. */
#include "scene.h"
#include "pet_view.h"
#include "appearance.h"
#include "morphology.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"
#include "usage.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sprites.h"
#pragma GCC diagnostic pop

#define WHITE 0xf4f4f8
#define GOLD 0xffd166

/* ---- layout ------------------------------------------------------------------------------ */

#define DEN_Y HEADER_H
#define DEN_B FOOTER_Y
#define WALL_B 160
#define DESK_D0 190   /* desk-top y of the first of two rows */
#define DESK_SOLO 236 /* desk-top y when one row is enough */
#define ROW_STEP 122
#define SLOT_W 160
#define DESKS_PER_ROW 4
#define MAX_DESKS 8
#define WS 3 /* worker sprite scale */
#define PET_S 5
#define PET_Y 371
#define PET_MIN_X 106 /* stays on the rug, clear of the plant and ATM (unless on an errand) */
#define PET_MAX_X 464
#define BED_X 548
#define NEST_X 280
#define PLANT_X 72 /* pot left edge; tidying nudges it by Scene.plant_dx */

/* ATM in the left floor corner; the receipt hangs from its slot until the pet tears it off. */
#define ATM_X 6
#define ATM_Y 372
#define ATM_W 62
#define ATM_H 78
#define ATM_CYCLE_MS 4000      /* screen alternates today's tokens / estimated cost */
#define RECEIPT_MIN 5000       /* EV_TOKENS chunk that prints a receipt */
#define RECEIPT_GAP_MS 300000  /* at most one receipt per 5 minutes */
#define RECEIPT_PRINT_MS 1440  /* 16 px, one per 90 ms */
#define RECEIPT_KEEP_MS 45000  /* nobody came for it: it drops on its own */
#define TEAR_X (ATM_X + 50)    /* pet_x when standing at the receipt */
#define TEAR_WAIT_MS 500

/* Ambient events: at most one per AMBIENT_MS slot, at a hashed offset (every other slot when calm). */
#define AMBIENT_MS 150000
#define TIDY_MS 1800
#define UNLOCK_TAG_MS 4500

#define MAX_PARTICLES 192
#define STALE_MS 4000

typedef enum { P_CRUMB, P_CONFETTI, P_SHELL, P_SPARK, P_SPARKLE, P_HEART, P_RING, P_BIT, P_PAPER } PKind;

typedef struct {
    int64_t born;
    int32_t life;
    uint8_t kind;
    bool alive;
    uint32_t color;
    float x, y;   /* start (or centre) */
    float vx, vy; /* px/s for ballistic kinds; offset vector for sparkles; arc height in vx for crumbs */
    float g;
} Particle;

struct Scene {
    uint64_t seen;
    bool primed;
    uint32_t rng;
    Particle p[MAX_PARTICLES];
    int next;

    float pet_x, wander_x;
    bool resting;
    int64_t pause_until, hold_until, last_ms;
    int64_t nom_until;
    int64_t flash_at;
    char banner[16];
    int64_t banner_at;

    int unlock_id;
    int64_t unlock_at;

    int64_t receipt_at;   /* print start; 0 = no receipt */
    int64_t last_receipt; /* rate limit */
    int64_t arrive_ms;    /* pet reached its receipt/tidy spot (0 = still walking) */
    int64_t tidy_slot;    /* last ambient slot the pet considered for tidying */
    int tidy_kind;        /* AmbKind of the tidy in progress, 0 = none */
    float tidy_x;
    int plant_dx;
    int64_t plant_wiggle_at;
    int64_t ripple_at;
    int ripple_x;
};

/* ---- small helpers ----------------------------------------------------------------------- */

static uint32_t rnd(Scene *s) {
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->rng = x;
}

static int rrange(Scene *s, int lo, int hi) { return lo + (int)(rnd(s) % (uint32_t)(hi - lo + 1)); }

static uint32_t hash_str(const char *str, size_t max, uint32_t h) {
    h ^= 2166136261u;
    for (size_t i = 0; i < max && str[i]; i++) {
        h ^= (uint8_t)str[i];
        h *= 16777619u;
    }
    return h ^ (h >> 15);
}

static uint32_t hash_u(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

/* sin over a 64-step turn, amplitude 64. */
static const int8_t QSIN[17] = {0, 6, 12, 19, 24, 30, 36, 41, 45, 50, 53, 56, 59, 61, 63, 64, 64};
static int isin(int a) {
    a &= 63;
    if (a < 16) return QSIN[a];
    if (a < 32) return QSIN[32 - a];
    if (a < 48) return -QSIN[a - 32];
    return -QSIN[64 - a];
}

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Rect clipped to the den band so effects never spill into ui.c's header/footer. */
static void R(Canvas *c, int x, int y, int w, int h, uint32_t col) {
    if (y < DEN_Y) {
        h -= DEN_Y - y;
        y = DEN_Y;
    }
    if (y + h > DEN_B) h = DEN_B - y;
    if (w > 0 && h > 0) gfx_rect(c, x, y, w, h, col);
}

typedef struct {
    int x0, y0, x1, y1;
} Clip;

static void RC(Canvas *c, const Clip *k, int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < k->x0) x = k->x0;
    if (y < k->y0) y = k->y0;
    if (x1 > k->x1) x1 = k->x1;
    if (y1 > k->y1) y1 = k->y1;
    if (x1 > x && y1 > y) gfx_rect(c, x, y, x1 - x, y1 - y, col);
}

/* Filled disc as one rect per scanline. */
static void disc(Canvas *c, const Clip *k, int cx, int cy, int r, uint32_t col) {
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)isqrt64((uint64_t)(r * r - dy * dy));
        RC(c, k, cx - hw, cy + dy, 2 * hw + 1, 1, col);
    }
}

/* 8x8 one-bit bitmap, bit 7 = leftmost, scaled. */
static void bitmap8(Canvas *c, const uint8_t *rows, int x, int y, int s, uint32_t color) {
    for (int r = 0; r < 8; r++) {
        uint8_t b = rows[r];
        for (int col = 0; col < 8;) {
            if (!(b & (0x80 >> col))) {
                col++;
                continue;
            }
            int run = col;
            while (run < 8 && (b & (0x80 >> run))) run++;
            gfx_rect(c, x + col * s, y + r * s, (run - col) * s, s, color);
            col = run;
        }
    }
}

static const Palette *family_pal(Family f) { return &PALETTES[(unsigned)f < N_FAMILIES ? f : FAM_OTHER]; }


/* Unlock id worn in a slot, -1 when nothing is (or the pet isn't loaded yet). */
static int worn(const App *app, int slot) { return app->pet_ready ? pet_worn(&app->pet, slot) : -1; }

/* ---- geometry ---------------------------------------------------------------------------- */

static int desk_count(const App *app) { return app->n_rows < MAX_DESKS ? app->n_rows : MAX_DESKS; }

static void desk_pos(int i, int n, int *sx, int *d) {
    int row = i / DESKS_PER_ROW;
    int in_row = n - row * DESKS_PER_ROW;
    if (in_row > DESKS_PER_ROW) in_row = DESKS_PER_ROW;
    *sx = (SCREEN_W - in_row * SLOT_W) / 2 + (i % DESKS_PER_ROW) * SLOT_W;
    *d = n <= DESKS_PER_ROW ? DESK_SOLO : DESK_D0 + row * ROW_STEP;
}

static int find_desk(const App *app, int bridge, const char *id) {
    int n = desk_count(app);
    for (int i = 0; i < n; i++)
        if (app->rows[i].bridge == bridge && !strncmp(app->rows[i].agent.id, id, sizeof app->rows[i].agent.id))
            return i;
    return -1;
}

static int pet_y_at(float x) { return x >= BED_X - 4 ? PET_Y - 6 : PET_Y; }

/* ---- ambient schedule -------------------------------------------------------------------- */

typedef enum { AMB_NONE, AMB_CRITTER, AMB_STRETCH, AMB_TIDY_PLANT, AMB_TIDY_RUG } AmbKind;

typedef struct {
    AmbKind kind;
    int64_t slot, t0, age; /* age = now - t0 */
    uint32_t h;
} Ambient;

#define CRITTER_MS 9000
#define STRETCH_MS 2400
#define TIDY_START_MS 20000 /* the pet must be free within this long after t0, or it skips the chore */

/* The slot's event (kind may be AMB_NONE for a skipped slot); purely a function of time. */
static Ambient ambient_slot(int64_t now, bool calm) {
    Ambient a;
    a.slot = now / AMBIENT_MS;
    a.h = hash_u((uint32_t)a.slot * 2654435761u + 0x51edu);
    a.t0 = a.slot * AMBIENT_MS + 15000 + (int64_t)(a.h % 90000);
    a.age = now - a.t0;
    if (calm) a.kind = (a.slot & 1) ? AMB_NONE : (a.h >> 20) & 1 ? AMB_CRITTER : AMB_STRETCH;
    else a.kind = (AmbKind)(AMB_CRITTER + (a.h >> 20) % 4);
    return a;
}

/* The event currently playing out for drawing (tidying is driven by scene_update instead). */
static Ambient ambient_now(int64_t now, bool calm) {
    Ambient a = ambient_slot(now, calm);
    int len = a.kind == AMB_CRITTER ? CRITTER_MS : a.kind == AMB_STRETCH ? STRETCH_MS : 0;
    if (a.age < 0 || a.age >= len) a.kind = AMB_NONE;
    return a;
}

/* ---- particles --------------------------------------------------------------------------- */

static Particle *spawn(Scene *s, PKind kind, int64_t born, int life) {
    Particle *p = NULL;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        int idx = (s->next + i) % MAX_PARTICLES;
        if (!s->p[idx].alive) {
            p = &s->p[idx];
            s->next = (idx + 1) % MAX_PARTICLES;
            break;
        }
    }
    if (!p) { /* pool full: recycle the oldest-ish slot */
        p = &s->p[s->next];
        s->next = (s->next + 1) % MAX_PARTICLES;
    }
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->born = born;
    p->life = life;
    p->alive = true;
    return p;
}

static void burst(Scene *s, PKind kind, int n, float x, float y, int64_t now, int life, int vx_spread, int vy_lo,
                  int vy_hi, float g, const uint32_t *cols, int ncols) {
    for (int i = 0; i < n; i++) {
        Particle *p = spawn(s, kind, now + rrange(s, 0, 60), life + rrange(s, -life / 5, life / 5));
        p->x = x + (float)rrange(s, -6, 6);
        p->y = y;
        p->vx = (float)rrange(s, -vx_spread, vx_spread);
        p->vy = (float)rrange(s, vy_lo, vy_hi);
        p->g = g;
        p->color = cols[rnd(s) % (uint32_t)ncols];
    }
}

static const uint32_t CONFETTI[] = {0xff6b8a, 0xffd166, 0x46c06f, 0x5ac8fa, 0xc58af9, 0xff9f43};
static const uint32_t SPARKS[] = {0xffd166, 0xff9f43, 0xfff3b0};

static void pet_center(const Scene *s, float *x, float *y) {
    *x = s->pet_x + 8 * PET_S;
    *y = (float)pet_y_at(s->pet_x) + 9 * PET_S;
}

static void handle_event(Scene *s, const App *app, const Event *e, int64_t now) {
    int desk = e->bridge >= 0 ? find_desk(app, e->bridge, e->agent_id) : -1;
    int sx = 0, d = 0;
    if (desk >= 0) desk_pos(desk, desk_count(app), &sx, &d);
    float pcx, pcy;
    pet_center(s, &pcx, &pcy);
    switch (e->kind) {
    case EV_TOKENS: {
        if (e->amount <= 0) break;
        int n = 0;
        for (int64_t v = e->amount; v > 1; v >>= 1) n++;
        if (n < 1) n = 1;
        if (n > 14) n = 14;
        n /= 1 + (int)usage_family_level(app, e->family); /* a provider low on fuel sheds fewer crumbs */
        if (n < 1) n = 1;
        const Palette *pal = family_pal(e->family);
        int64_t last = now;
        for (int i = 0; i < n; i++) {
            int64_t born = now + i * 70;
            Particle *p = spawn(s, P_CRUMB, born, 760 + rrange(s, 0, 220));
            if (desk >= 0) {
                p->x = (float)(sx + 103 + rrange(s, -10, 10));
                p->y = (float)(d - 28 + rrange(s, -6, 6));
            } else {
                p->x = (float)rrange(s, 40, SCREEN_W - 40);
                p->y = (float)WALL_B;
            }
            p->vx = (float)rrange(s, 50, 110); /* arc height */
            p->color = (i & 1) ? pal->accent : pal->light;
            if (born + p->life > last) last = born + p->life;
        }
        if (last + 300 > s->hold_until) s->hold_until = last + 300;
        /* A big chunk prints an ATM receipt (rarely; never at night). */
        if (e->amount >= RECEIPT_MIN && !app->night_dim && !s->receipt_at &&
            (!s->last_receipt || now - s->last_receipt >= RECEIPT_GAP_MS)) {
            s->receipt_at = now;
            s->last_receipt = now;
        }
        break;
    }
    case EV_FINISHED:
        if (desk >= 0) burst(s, P_CONFETTI, 22, (float)(sx + 80), (float)(d - 34), now, 1300, 95, -230, -120, 430.f,
                             CONFETTI, 6);
        break;
    case EV_NEEDS_YOU:
        if (desk >= 0)
            for (int i = 0; i < 3; i++) {
                Particle *p = spawn(s, P_RING, now + i * 220, 700);
                p->x = (float)(sx + 44);
                p->y = (float)(d - 54);
                p->color = i == 1 ? 0xffd0c0 : ALERT_COL;
            }
        break;
    case EV_FAILED:
        if (desk >= 0) burst(s, P_SPARK, 12, (float)(sx + 103), (float)(d - 40), now, 650, 80, -160, -60, 380.f,
                             SPARKS, 3);
        break;
    case EV_HATCHED:
    case EV_EVOLVED: {
        s->flash_at = now;
        s->banner_at = now;
        if (e->kind == EV_HATCHED) snprintf(s->banner, sizeof s->banner, "Hatched!");
        else snprintf(s->banner, sizeof s->banner, "%s!", stage_name(app->pet.stage));
        Palette pal;
        morphology_palette(&app->pet, &pal);
        uint32_t cols[3] = {0xffffff, GOLD, pal.accent};
        for (int i = 0; i < 30; i++) {
            Particle *p = spawn(s, P_SPARKLE, now + rrange(s, 0, 260), 900 + rrange(s, 0, 500));
            int a = rrange(s, 0, 63), r = rrange(s, 40, 120);
            p->x = pcx;
            p->y = pcy;
            p->vx = (float)(isin(a + 16) * r / 64);
            p->vy = (float)(isin(a) * r / 64) * 0.8f;
            p->color = cols[i % 3];
        }
        if (e->kind == EV_HATCHED) {
            uint32_t shell[2] = {COL_SHELL, 0xd8d4c8};
            burst(s, P_SHELL, 10, pcx, pcy, now, 1100, 140, -260, -140, 520.f, shell, 2);
        }
        break;
    }
    case EV_PAT:
        for (int i = 0; i < 5; i++) {
            Particle *p = spawn(s, P_HEART, now + i * 170, 1500 + rrange(s, 0, 300));
            p->x = pcx + (float)rrange(s, -26, 18);
            p->y = pcy - 30;
            p->vx = (float)rrange(s, 0, 63); /* sway phase */
            p->vy = (float)rrange(s, 38, 56); /* rise speed */
        }
        break;
    case EV_UNLOCKED: {
        s->unlock_id = (int)e->amount;
        s->unlock_at = now;
        Palette pal;
        morphology_palette(&app->pet, &pal);
        uint32_t cols[4] = {0xffffff, GOLD, pal.accent, 0xff9fd0};
        for (int i = 0; i < 22; i++) {
            Particle *p = spawn(s, P_SPARKLE, now + rrange(s, 0, 400), 800 + rrange(s, 0, 500));
            int a = rrange(s, 0, 63), r = rrange(s, 34, 96);
            p->x = pcx;
            p->y = pcy - 10;
            p->vx = (float)(isin(a + 16) * r / 64);
            p->vy = (float)(isin(a) * r / 64) * 0.8f;
            p->color = cols[i % 4];
        }
        break;
    }
    }
}

/* ---- lifecycle --------------------------------------------------------------------------- */

Scene *scene_create(void) {
    Scene *s = calloc(1, sizeof *s);
    if (s) s->rng = 0x9e3779b9u;
    return s;
}

void scene_destroy(Scene *s) { free(s); }

/* The receipt leaves the ATM: torn off by the pet, or it just drops after a while. */
static void receipt_off(Scene *s, int64_t now, bool torn) {
    Particle *p = spawn(s, P_PAPER, now, torn ? 1500 : 1100);
    p->x = (float)(ATM_X + 39);
    p->y = (float)(ATM_Y + 66);
    p->vx = torn ? 46.f : 6.f;
    p->vy = torn ? -110.f : 0.f;
    p->g = torn ? 150.f : 90.f;
    p->color = 0xf4f0e0;
    if (torn) {
        uint32_t cols[2] = {0xffffff, GOLD};
        for (int i = 0; i < 6; i++) {
            Particle *q = spawn(s, P_SPARKLE, now + rrange(s, 0, 160), 500 + rrange(s, 0, 200));
            int a = rrange(s, 0, 63);
            q->x = p->x;
            q->y = p->y - 4;
            q->vx = (float)(isin(a + 16) * 18 / 64);
            q->vy = (float)(isin(a) * 14 / 64);
            q->color = cols[i & 1];
        }
    }
    s->receipt_at = 0;
}

static int first_urgent_desk(const App *app) {
    int n = desk_count(app);
    for (int i = 0; i < n; i++) {
        Bucket b = app->rows[i].agent.bucket;
        if (b == B_NEEDS_INPUT || b == B_FAILED) return i;
    }
    return -1;
}

void scene_update(Scene *s, const App *app, int64_t now_ms) {
    if (!s) return;
    int64_t now_unix = app_now(app);
    bool sleepy = pet_mood(&app->pet, now_unix, app->tz_offset, app->n_alerts, app->n_running) == MOOD_SLEEP;
    if (!s->primed) {
        s->primed = true;
        s->rng ^= (uint32_t)now_ms * 2654435761u;
        if (!s->rng) s->rng = 1;
        s->pet_x = app->pet.stage == STAGE_EGG ? NEST_X : sleepy ? BED_X : (PET_MIN_X + PET_MAX_X) / 2;
        s->wander_x = s->pet_x;
        s->last_ms = now_ms;
    }

    /* Events since last call; older than the ring (or stale) are skipped. */
    uint64_t n = app->n_events;
    if (n < s->seen) s->seen = n;
    if (n - s->seen > EVENT_RING) s->seen = n - EVENT_RING;
    for (; s->seen < n; s->seen++) {
        const Event *e = &app->events[s->seen % EVENT_RING];
        if (now_ms - e->at_ms <= STALE_MS) handle_event(s, app, e, now_ms);
    }

    /* Pet movement. */
    float dt = (float)(now_ms - s->last_ms) / 1000.f;
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    s->last_ms = now_ms;
    float goal, speed = 46.f;
    int urgent = first_urgent_desk(app);
    bool egg = app->pet.stage == STAGE_EGG;
    bool busy = egg || sleepy || now_ms < s->hold_until || urgent >= 0;

    /* The receipt waits for the pet once printed; if nobody comes it drops on its own. */
    bool receipt_ready = s->receipt_at && now_ms >= s->receipt_at + RECEIPT_PRINT_MS;
    if (receipt_ready && now_ms - s->receipt_at >= RECEIPT_PRINT_MS + RECEIPT_KEEP_MS) {
        receipt_off(s, now_ms, false);
        receipt_ready = false;
    }
    /* Tidying chores start with their ambient slot, only if the pet is free soon after. */
    Ambient amb = ambient_slot(now_ms, app->night_dim);
    if (busy) s->tidy_kind = AMB_NONE;
    if ((amb.kind == AMB_TIDY_PLANT || amb.kind == AMB_TIDY_RUG) && amb.slot != s->tidy_slot && amb.age >= 0) {
        if (amb.age > TIDY_START_MS) {
            s->tidy_slot = amb.slot;
        } else if (!busy && !receipt_ready) {
            s->tidy_slot = amb.slot;
            s->tidy_kind = amb.kind;
            s->tidy_x = amb.kind == AMB_TIDY_PLANT ? (float)(PLANT_X + 16 + s->plant_dx)
                                                   : (float)(PET_MIN_X + 40 + (int)(amb.h % 280));
        }
    }

    bool special = false;
    if (egg) {
        goal = NEST_X;
    } else if (sleepy) {
        goal = BED_X;
        speed = 30.f;
    } else if (now_ms < s->hold_until) {
        goal = s->pet_x; /* stand still while crumbs fly in */
    } else if (urgent >= 0) {
        int sx, d;
        desk_pos(urgent, desk_count(app), &sx, &d);
        goal = (float)(sx + 80 - 8 * PET_S);
        if (goal < PET_MIN_X) goal = PET_MIN_X;
        if (goal > PET_MAX_X) goal = PET_MAX_X;
        speed = 95.f;
        s->resting = false;
    } else if (receipt_ready) {
        goal = TEAR_X;
        speed = 70.f;
        special = true;
    } else if (s->tidy_kind) {
        goal = s->tidy_x;
        special = true;
    } else {
        if (s->wander_x > PET_MAX_X) s->wander_x = PET_MAX_X; /* coming back from bed */
        float dist = s->wander_x - s->pet_x;
        if (dist < 1 && dist > -1) {
            if (!s->resting) {
                s->resting = true;
                s->pause_until = now_ms + rrange(s, 1500, 5000);
            } else if (now_ms >= s->pause_until) {
                s->resting = false;
                float nx;
                do nx = (float)rrange(s, PET_MIN_X, PET_MAX_X);
                while (nx - s->pet_x < 60 && s->pet_x - nx < 60);
                s->wander_x = nx;
            }
        }
        goal = s->wander_x;
    }
    float step = speed * dt, dx = goal - s->pet_x;
    if (dx <= step && dx >= -step) s->pet_x = goal;
    else s->pet_x += dx > 0 ? step : -step;

    if (special && s->pet_x == goal) {
        if (!s->arrive_ms) {
            s->arrive_ms = now_ms;
            if (!receipt_ready && s->tidy_kind == AMB_TIDY_PLANT) {
                s->plant_wiggle_at = now_ms;
                static const uint32_t LEAVES[2] = {0x4caf50, 0x388e3c};
                burst(s, P_BIT, 3, (float)(PLANT_X + 16 + s->plant_dx), 384.f, now_ms, 900, 30, -60, -20, 160.f,
                      LEAVES, 2);
            } else if (!receipt_ready && s->tidy_kind == AMB_TIDY_RUG) {
                s->ripple_at = now_ms;
                s->ripple_x = (int)goal + 8 * PET_S;
                static const uint32_t DUST[2] = {0xfff3b0, 0xffffff};
                for (int k = 0; k < 4; k++) {
                    Particle *p = spawn(s, P_SPARKLE, now_ms + 300 + k * 260, 520);
                    p->x = (float)(s->ripple_x + rrange(s, -50, 50));
                    p->y = (float)rrange(s, 392, 436);
                    p->vy = -6.f;
                    p->color = DUST[k & 1];
                }
            }
        }
        int64_t there = now_ms - s->arrive_ms;
        if (receipt_ready && there >= TEAR_WAIT_MS) {
            receipt_off(s, now_ms, true);
            s->hold_until = now_ms + 600; /* admire it for a moment */
            s->arrive_ms = 0;
        } else if (!receipt_ready && there >= TIDY_MS) {
            if (s->tidy_kind == AMB_TIDY_PLANT) {
                int nd;
                do nd = rrange(s, -3, 3);
                while (nd == s->plant_dx);
                s->plant_dx = nd;
            }
            s->tidy_kind = AMB_NONE;
            s->arrive_ms = 0;
        }
    } else {
        s->arrive_ms = 0;
    }

    /* Retire particles; arriving crumbs make the pet chomp. */
    float mx, my;
    pet_center(s, &mx, &my);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &s->p[i];
        if (!p->alive || now_ms < p->born + p->life) continue;
        p->alive = false;
        if (p->kind == P_CRUMB) {
            s->nom_until = now_ms + 450;
            uint32_t col = p->color;
            Particle *b = spawn(s, P_BIT, now_ms, 380);
            b->x = mx + (float)rrange(s, -8, 8);
            b->y = my + 4;
            b->vx = (float)rrange(s, -60, 60);
            b->vy = (float)rrange(s, -120, -60);
            b->g = 600.f;
            b->color = col;
        }
    }
}

/* ---- time of day ------------------------------------------------------------------------- */

/* Per-frame drawing context: lighting plus the calm flag and the ambient event playing now. */
typedef struct {
    int minute;
    int day; /* 0 night .. 256 full daylight */
    uint32_t sky_top, sky_bot;
    uint32_t wall, wall_hi, trim, floor, plank, shadow, rug, rug_edge;
    bool calm; /* app->night_dim: slower, sparser effects (the app draws at ~5 fps) */
    Ambient amb;
} Light;

static const struct {
    int m;
    uint32_t top, bot;
    int day;
} SKY[] = {
    {0, 0x060a20, 0x121838, 0},       {300, 0x0a0e2a, 0x1c1c44, 0},      {360, 0x3a3a7a, 0xc0707a, 90},
    {420, 0x5a8ad8, 0xf8b070, 190},   {480, 0x4a9be8, 0xa8d8ff, 256},    {1020, 0x4a9be8, 0xa8d8ff, 256},
    {1110, 0x5a5aa8, 0xf89a5a, 180},  {1170, 0x3a2a6a, 0xd0605a, 90},    {1230, 0x121838, 0x2a2050, 10},
    {1290, 0x060a20, 0x121838, 0},    {1440, 0x060a20, 0x121838, 0},
};

static Light light_for(const App *app, int64_t now_ms) {
    Light L;
    int64_t local = app_now(app) + (app->clock_synced ? app->tz_offset : 0);
    int m = (int)(((local % 86400) + 86400) % 86400 / 60);
    L.minute = m;
    int k = 0;
    while (SKY[k + 1].m < m) k++;
    int span = SKY[k + 1].m - SKY[k].m;
    int t = span ? (m - SKY[k].m) * 256 / span : 0;
    L.sky_top = lerp_col(SKY[k].top, SKY[k + 1].top, t);
    L.sky_bot = lerp_col(SKY[k].bot, SKY[k + 1].bot, t);
    L.day = SKY[k].day + (SKY[k + 1].day - SKY[k].day) * t / 256;
    L.wall = lerp_col(0x1b1e2c, 0x2c3247, L.day);
    L.wall_hi = lerp_col(0x20243a, 0x343b54, L.day);
    L.trim = lerp_col(0x111219, 0x1a1d26, L.day);
    L.floor = lerp_col(0x2b1f1a, 0x4b3528, L.day);
    L.plank = lerp_col(0x221813, 0x3a291f, L.day);
    L.shadow = lerp_col(0x1a1310, 0x2c1f18, L.day);
    L.rug = lerp_col(0x34213a, 0x553457, L.day);
    L.rug_edge = lerp_col(0x4a2e44, 0x7a4a6e, L.day);
    L.calm = app->night_dim;
    L.amb = ambient_now(now_ms, L.calm);
    return L;
}

/* ---- background -------------------------------------------------------------------------- */

#define WIN_X 216
#define WIN_Y 48
#define WIN_W 208
#define WIN_H 100

/* The default view: sky by time of day, stars, sun or moon, clouds and the ambient critter. */
static void draw_sky(Canvas *c, const Light *L, int64_t now) {
    const Clip k = {WIN_X + 6, WIN_Y + 6, WIN_X + WIN_W - 6, WIN_Y + WIN_H - 6};
    int iw = k.x1 - k.x0, ih = k.y1 - k.y0;
    /* sky gradient */
    const int bands = 10;
    for (int i = 0; i < bands; i++) {
        int y0 = k.y0 + i * ih / bands, y1 = k.y0 + (i + 1) * ih / bands;
        RC(c, &k, k.x0, y0, iw, y1 - y0, lerp_col(L->sky_top, L->sky_bot, i * 256 / (bands - 1)));
    }
    int m = L->minute;
    /* stars + shooting star */
    if (L->day < 120) {
        for (int i = 0; i < 18; i++) {
            uint32_t h = hash_u((uint32_t)i * 7919u + 17u);
            int x = k.x0 + 3 + (int)(h % (uint32_t)(iw - 6)), y = k.y0 + 3 + (int)((h >> 12) % (uint32_t)(ih * 2 / 3));
            int ph = (int)((now / 160 + (h >> 20)) % 28);
            if (ph == 27) continue;
            uint32_t col = ph < 3 ? 0xfffbe8 : ((h >> 8) & 1) ? 0xb0b8e0 : 0x8088b8;
            RC(c, &k, x, y, 1 + (ph < 3 || (h & 3) == 0), 1 + (ph < 3 || (h & 3) == 0), col);
            if (ph == 1) {
                RC(c, &k, x - 2, y, 2, 1, 0xd8dcff);
                RC(c, &k, x + 2, y, 2, 1, 0xd8dcff);
                RC(c, &k, x, y - 2, 1, 2, 0xd8dcff);
                RC(c, &k, x, y + 2, 1, 2, 0xd8dcff);
            }
        }
        int64_t st = L->calm ? 19000 : now % 19000; /* no shooting stars when calm */
        if (st < 650) {
            int hx = k.x0 + 30 + (int)(st * 140 / 650), hy = k.y0 + 8 + (int)(st * 30 / 650);
            for (int j = 0; j < 6; j++)
                RC(c, &k, hx - j * 5, hy - j, 3 - j / 2 > 1 ? 3 - j / 2 : 1, 1, lerp_col(0xffffff, L->sky_top, j * 42));
        }
    }
    /* sun 6:00..19:30, moon 19:30..6:00 along an arc */
    bool sun = m >= 360 && m < 1170;
    int u = sun ? (m - 360) * 256 / 810 : ((m - 1170 + 1440) % 1440) * 256 / 630;
    int bx = k.x0 + 14 + u * (iw - 28) / 256;
    int by = k.y1 - 12 - isin(u * 32 / 256) * (ih - 30) / 64;
    if (sun) {
        int pulse = (int)(now / 300 % 4);
        for (int a = 0; a < 8; a++) {
            int rr = 13 + (pulse == a % 4 ? 2 : 0);
            RC(c, &k, bx + isin(a * 8 + 16) * rr / 64 - 1, by + isin(a * 8) * rr / 64 - 1, 3, 3, 0xffe27a);
        }
        disc(c, &k, bx, by, 9, 0xffc93a);
        disc(c, &k, bx - 1, by - 1, 6, 0xffe27a);
        RC(c, &k, bx - 4, by - 4, 3, 2, 0xfff6c8);
    } else {
        int band = (by - k.y0) * 10 / ih;
        uint32_t behind = lerp_col(L->sky_top, L->sky_bot, band * 256 / 9);
        disc(c, &k, bx, by, 8, 0xf2eccf);
        RC(c, &k, bx - 3, by + 1, 2, 2, 0xd6cfae);
        RC(c, &k, bx - 1, by + 4, 2, 2, 0xd6cfae);
        disc(c, &k, bx + 5, by - 3, 7, behind);
    }
    /* clouds */
    if (L->day > 100) {
        uint32_t cc = lerp_col(lerp_col(WHITE, L->sky_bot, 70), 0xffffff, (L->day - 100) * 256 / 156);
        for (int i = 0; i < 3; i++) {
            int x = k.x0 - 40 + (int)((now / 250 + i * 97 + i * i * 31) % (iw + 80));
            int y = k.y0 + 10 + i * 20;
            RC(c, &k, x, y + 5, 34, 8, cc);
            RC(c, &k, x + 6, y, 14, 7, cc);
            RC(c, &k, x + 17, y + 2, 11, 5, cc);
        }
    }
    /* ambient critter: a bird by day, a cat strolling the outside ledge by night */
    if (L->amb.kind == AMB_CRITTER) {
        int age = (int)L->amb.age;
        if (L->day >= 128) {
            static const uint8_t BIRD[2][3] = {{0x82, 0x44, 0x38}, {0x38, 0x44, 0x82}};
            if (age < 5500) {
                const uint8_t *f = BIRD[(age / (L->calm ? 400 : 140)) % 2];
                int x = k.x0 - 16 + age * (iw + 32) / 5500;
                int y = k.y0 + 12 + (int)(L->amb.h % 24) - age * 10 / 5500 + isin(age / 50) * 3 / 64;
                uint32_t col = lerp_col(L->sky_top, 0x1a1d26, 210);
                for (int r = 0; r < 3; r++)
                    for (int b = 0; b < 7; b++)
                        if (f[r] & (0x80 >> b)) RC(c, &k, x + b * 2, y + r * 2, 2, 2, col);
            }
        } else {
            int xm = k.x0 + iw * 3 / 4 - 8, by = k.y1, x; /* sits mid right pane */
            bool sit = false;
            if (age < 3500) x = k.x1 + 4 + (xm - k.x1 - 4) * age / 3500;
            else if (age < 5500) x = xm, sit = true;
            else x = xm + (k.x0 - 34 - xm) * (age - 5500) / 3500;
            uint32_t col = lerp_col(L->sky_bot, 0x000000, 180);
            bool blink = (age / 1700) % 5 == 2;
            int hy = sit ? by - 18 : by - 14;
            RC(c, &k, x + 1, hy - 2, 2, 3, col);
            RC(c, &k, x + 6, hy - 2, 2, 3, col);
            RC(c, &k, x, hy, 9, 7, col);
            if (!blink) {
                RC(c, &k, x + 2, hy + 3, 1, 1, 0xd8f070);
                RC(c, &k, x + 5, hy + 3, 1, 1, 0xd8f070);
            }
            if (sit) {
                RC(c, &k, x + 3, by - 12, 11, 12, col);
                RC(c, &k, x + 12, by - 3, 8, 2, col);
                RC(c, &k, x + 19, by - 5 + (age / 500) % 2, 2, 3, col);
            } else {
                int f = (age / (L->calm ? 400 : 200)) % 2;
                RC(c, &k, x + 6, by - 10, 16, 6, col);
                RC(c, &k, x + 7 + f, by - 4, 2, 4, col);
                RC(c, &k, x + 18 - f, by - 4, 2, 4, col);
                RC(c, &k, x + 21, by - 16, 2, 7, col);
                RC(c, &k, x + 22, by - 18, 2, 3, col);
            }
        }
    }
}

/* view: an equipped WEAR_WINDOW id replacing the sky inside the frame, or -1. */
static void draw_window(Canvas *c, const Light *L, int view, int64_t now) {
    const Clip k = {WIN_X + 6, WIN_Y + 6, WIN_X + WIN_W - 6, WIN_Y + WIN_H - 6};
    int iw = k.x1 - k.x0, ih = k.y1 - k.y0;
    /* curtains */
    for (int side = 0; side < 2; side++) {
        int cx = side ? WIN_X + WIN_W - 2 : WIN_X - 12;
        R(c, cx, WIN_Y - 4, 14, WIN_H + 8, lerp_col(0x4a1f2c, 0x8a3b4f, L->day));
        R(c, cx + 4, WIN_Y - 4, 2, WIN_H + 8, lerp_col(0x3a1822, 0x6a2b3c, L->day));
        R(c, cx + 9, WIN_Y - 4, 2, WIN_H + 8, lerp_col(0x3a1822, 0x6a2b3c, L->day));
    }
    R(c, WIN_X - 16, WIN_Y - 7, WIN_W + 32, 4, 0x3a2a20); /* rod */
    R(c, WIN_X, WIN_Y, WIN_W, WIN_H, 0x5a4232);
    R(c, WIN_X + 2, WIN_Y + 2, WIN_W - 4, 2, 0x7a5a44);
    if (view >= 0) appearance_draw_window(c, view, k.x0, k.y0, iw, ih, L->day, L->calm ? now / 4 : now);
    else draw_sky(c, L, now);
    /* mullions + sill */
    R(c, WIN_X + WIN_W / 2 - 2, k.y0, 4, ih, 0x5a4232);
    R(c, k.x0, WIN_Y + WIN_H / 2 - 2, iw, 3, 0x5a4232);
    R(c, WIN_X - 6, WIN_Y + WIN_H, WIN_W + 12, 6, 0x6b5040);
    R(c, WIN_X - 6, WIN_Y + WIN_H, WIN_W + 12, 2, 0x8a6a52);
}

static void draw_hud(Canvas *c, const App *app, int64_t now) {
    if (!app->pet_ready) return;
    const Pet *p = &app->pet;
    Palette pal;
    morphology_palette(p, &pal);
    gfx_text_fit(c, FONT_BIG, 14, 46, TEXT, p->name[0] ? p->name : "Pet", 186);
    char xp[16], line[40];
    format_tokens(p->life_xp, xp, sizeof xp);
    int w = gfx_text(c, FONT_BODY, 14, 80, pal.light, stage_name(p->stage), 0);
    snprintf(line, sizeof line, " %s tok", xp);
    gfx_text(c, FONT_BODY, 14 + w, 80, DIM, line, 0);

    int64_t next = stage_threshold(p->stage);
    int64_t prev = p->stage == STAGE_EGG ? 0 : stage_threshold((Stage)(p->stage - 1));
    if (p->generation > 1 && p->stage == STAGE_BABY) prev = 0;
    if (p->stage == STAGE_ADULT) {
        int available = pet_forms_available(p);
        next = pet_form_threshold(available);
        prev = pet_form_threshold(available - 1);
    }
    int segs = 12, full = segs, partial = 0;
    if (next > prev) {
        int64_t num = (p->life_xp - prev) * segs * 4;
        int64_t q = num / (next - prev);
        if (q < 0) q = 0;
        if (q > segs * 4) q = segs * 4;
        full = (int)(q / 4);
        partial = (int)(q % 4);
    }
    R(c, 12, 104, segs * 10 + 2, 14, 0x0e1016);
    for (int i = 0; i < segs; i++) {
        int x = 14 + i * 10;
        uint32_t col = 0x303648;
        if (i < full) col = pal.body;
        else if (i == full && (partial || (now / 400) % 2)) col = lerp_col(0x303648, pal.light, 90 + partial * 40);
        R(c, x, 106, 9, 10, col);
        if (i < full) R(c, x, 106, 9, 2, pal.light);
    }
    /* Next stage or inherited adult form target beside the bar. */
    if (next > prev) format_tokens(next, line, sizeof line);
    else snprintf(line, sizeof line, "max");
    gfx_text_fit(c, FONT_BODY, 22 + segs * 10, 101, 0x6a7080, line, 60);
}

static void draw_rate_panel(Canvas *c, const App *app, int64_t now) {
    int x = 444, y = 46, w = 140, h = 104; /* x 590..634 is the unlockable wall-decor spot */
    R(c, x, y, w, h, 0x2a3040);
    R(c, x + 2, y + 2, w - 4, h - 4, 0x12151d);
    int64_t lag = app_now(app) / 60 - app->rate_minute;
    char cur[16];
    format_tokens(lag == 0 ? app->rate[RATE_MINUTES - 1] : 0, cur, sizeof cur);
    int cw = (int)strlen(cur) * gfx_font_w(FONT_BODY);
    if (cw <= w - 16 - 74) gfx_text(c, FONT_BODY, x + 8, y + 4, DIM, "tok/min", 0);
    gfx_text(c, FONT_BODY, x + w - 8 - cw, y + 4, TEXT, cur, 0);
    scene_draw_sparkline(c, app, x + 8, y + 28, w - 16, h - 36, now);
}

/* Window light on the floor: the panes projected as a parallelogram that slides with the sun
 * (morning light falls to the right, evening to the left) and a faint one under the moon. Only
 * bare floor/plank pixels are recoloured, so it is cheap and sits under everything. */
static void draw_window_light(Canvas *c, const Light *L) {
    int m = L->minute, u, strength;
    uint32_t tint;
    if (m >= 380 && m < 1150 && L->day > 110) {
        u = (m - 360) * 256 / 810;
        strength = (L->day - 110) * 64 / 146; /* up to 25% toward warm light */
        tint = 0xe8b878;
    } else if ((m >= 1200 || m < 330) && L->day < 40) {
        u = ((m - 1170 + 1440) % 1440) * 256 / 630;
        strength = 30;
        tint = 0x8090c8;
    } else {
        return;
    }
    uint32_t f0 = L->floor, f1 = lerp_col(f0, tint, strength);
    uint32_t p0 = L->plank, p1 = lerp_col(p0, tint, strength);
    const int top = WALL_B + 4, hgt = 84;
    int shift = (128 - u) * 150 / 128;
    for (int r = 0; r < hgt; r++) {
        if (r >= 40 && r < 44) continue; /* transom shadow */
        int w = WIN_W - 16 + r * 36 / hgt;
        int x0 = WIN_X + 8 - r * 18 / hgt + shift * r / hgt;
        int mid = x0 + w / 2;
        uint32_t *row = c->px + (top + r) * SCREEN_W;
        for (int pane = 0; pane < 2; pane++) {
            int a = pane ? mid + 3 : x0, b = pane ? x0 + w : mid - 3;
            if (a < 0) a = 0;
            if (b > SCREEN_W) b = SCREEN_W;
            for (int x = a; x < b; x++) {
                if (row[x] == f0) row[x] = f1;
                else if (row[x] == p0) row[x] = p1;
            }
        }
    }
}

static void draw_wall_panel(Canvas *c, const App *app, const Light *L, int64_t now);
static void draw_room(Canvas *c, const Light *L, const Scene *s, const App *app, int64_t now) {
    R(c, 0, DEN_Y, SCREEN_W, WALL_B - DEN_Y, L->wall);
    for (int x = 20; x < SCREEN_W; x += 40) R(c, x, DEN_Y, 2, WALL_B - DEN_Y - 8, L->wall_hi);
    int wall = worn(app, WEAR_WALL);
    if (wall >= 0) appearance_draw_wall(c, wall, 0, DEN_Y, SCREEN_W, WALL_B - 8 - DEN_Y, L->day);
    R(c, 0, WALL_B - 8, SCREEN_W, 8, L->trim);
    R(c, 0, WALL_B - 8, SCREEN_W, 1, L->wall_hi);
    draw_window(c, L, worn(app, WEAR_WINDOW), now);
    draw_hud(c, app, now);
    draw_wall_panel(c, app, L, now);

    R(c, 0, WALL_B, SCREEN_W, DEN_B - WALL_B, L->floor);
    for (int k = 0, y = WALL_B + 18; y < DEN_B; k++, y += 20) {
        R(c, 0, y, SCREEN_W, 2, L->plank);
        for (int sx = (k * 97) % 160; sx < SCREEN_W; sx += 160) R(c, sx, y - 18, 2, 18, L->plank);
    }
    draw_window_light(c, L);
    /* rug */
    int rx = 104, ry = 382, rw = 432, rh = 66;
    R(c, rx + 6, ry, rw - 12, rh, L->rug_edge);
    R(c, rx, ry + 6, rw, rh - 12, L->rug_edge);
    R(c, rx + 8, ry + 4, rw - 16, rh - 8, L->rug);
    R(c, rx + 4, ry + 8, rw - 8, rh - 16, L->rug);
    for (int x = rx + 16; x < rx + rw - 16; x += 16) {
        R(c, x, ry + 8, 4, 4, L->rug_edge);
        R(c, x + 8, ry + rh - 12, 4, 4, L->rug_edge);
    }
    /* pet bed (front lip is drawn over the pet in draw_pet) */
    uint32_t rim = lerp_col(0x3a3566, 0x5e5aa8, L->day), cush = lerp_col(0x4e4a80, 0x8a86d8, L->day);
    R(c, BED_X - 2, 424, 88, 24, rim);
    R(c, BED_X + 2, 420, 80, 6, rim);
    R(c, BED_X + 4, 421, 76, 2, cush);
    R(c, BED_X + 6, 428, 72, 12, cush);
    R(c, BED_X - 2, 447, 88, 3, L->shadow);
    /* plant (the pet nudges it around when tidying; it wiggles while being fussed with) */
    int px = PLANT_X - 12 + (s ? s->plant_dx : 0);
    int64_t wa = s ? now - s->plant_wiggle_at : -1;
    bool wiggle = s && s->plant_wiggle_at && wa >= 0 && wa < TIDY_MS;
    int sway = wiggle ? isin((int)(now / 30)) / 16 : isin((int)(now / (L->calm ? 270 : 90))) / 32;
    R(c, px + 14, 420, 28, 26, 0xa4553a);
    R(c, px + 12, 416, 32, 6, 0xc06a4a);
    R(c, px + 14, 444, 28, 3, L->shadow);
    uint32_t leaf = lerp_col(0x2e6a3a, 0x4caf50, L->day), leaf2 = lerp_col(0x24502c, 0x388e3c, L->day);
    R(c, px + 26, 384, 4, 32, leaf2);
    R(c, px + 14 + sway, 388, 12, 6, leaf);
    R(c, px + 30 + sway, 380, 14, 6, leaf);
    R(c, px + 18 + sway, 400, 10, 5, leaf2);
    R(c, px + 30 - sway, 396, 12, 5, leaf);
    R(c, px + 24 + sway, 372, 8, 10, leaf);
}

/* The pet smoothing the rug: a fold runs out both ways across it (drawn over rug decor). */
static void draw_rug_ripple(Canvas *c, const Scene *s, const Light *L, int64_t now) {
    const int rx = 104, ry = 382, rw = 432, rh = 66;
    int64_t ra = now - s->ripple_at;
    if (!s->ripple_at || ra < 0 || ra >= 1600) return;
    uint32_t fold = lerp_col(L->rug, 0xffffff, 40), shade = lerp_col(L->rug, 0x000000, 60);
    uint32_t hi = lerp_col(L->rug_edge, 0xffffff, 60);
    for (int dir = -1; dir <= 1; dir += 2) {
        int bx = s->ripple_x + dir * (int)(ra * 18 / 100);
        if (bx < rx + 14 || bx > rx + rw - 14) continue;
        int lift = ra < 1100 ? 4 : 2;
        R(c, bx - 3, ry + 4, 6, rh - 8, fold);
        R(c, bx + 3 * dir - (dir < 0 ? 2 : 0), ry + 4, 2, rh - 8, shade);
        R(c, bx - 8, ry - lift, 16, lift, L->rug_edge);
        R(c, bx - 6, ry - lift, 12, 1, hi);
        R(c, bx - 8, ry + rh, 16, lift - 1, L->rug_edge);
    }
}

/* ---- ATM --------------------------------------------------------------------------------- */

/* 3x5 pixel font for the ATM and fuel panel: rows are w-bit (bit w-1 = left); '.' is one column wide. */
static const struct {
    char ch;
    uint8_t w;
    uint8_t rows[5];
} TINY[] = {
    {'0', 3, {7, 5, 5, 5, 7}}, {'1', 3, {2, 6, 2, 2, 7}}, {'2', 3, {7, 1, 7, 4, 7}}, {'3', 3, {7, 1, 3, 1, 7}},
    {'4', 3, {5, 5, 7, 1, 1}}, {'5', 3, {7, 4, 7, 1, 7}}, {'6', 3, {7, 4, 7, 5, 7}}, {'7', 3, {7, 1, 1, 2, 2}},
    {'8', 3, {7, 5, 7, 5, 7}}, {'9', 3, {7, 5, 7, 1, 7}}, {'.', 1, {0, 0, 0, 0, 1}}, {'k', 3, {4, 5, 6, 5, 5}},
    {'M', 3, {5, 7, 7, 5, 5}}, {'B', 3, {6, 5, 6, 5, 6}}, {'$', 3, {3, 6, 2, 3, 6}}, {'~', 3, {0, 6, 3, 0, 0}},
    {'T', 3, {7, 2, 2, 2, 2}}, {'O', 3, {7, 5, 5, 5, 7}}, {'D', 3, {6, 5, 5, 5, 6}}, {'A', 3, {2, 5, 7, 5, 5}},
    {'Y', 3, {5, 5, 2, 2, 2}}, {'K', 3, {5, 5, 6, 5, 5}}, {'E', 3, {7, 4, 6, 4, 7}}, {'N', 3, {6, 5, 5, 5, 5}},
    {'S', 3, {3, 4, 2, 1, 6}}, {'C', 3, {7, 4, 4, 4, 7}}, {' ', 2, {0, 0, 0, 0, 0}}, {'F', 3, {7, 4, 6, 4, 4}},
    {'U', 3, {5, 5, 5, 5, 7}}, {'L', 3, {4, 4, 4, 4, 7}}, {'h', 3, {4, 4, 7, 5, 5}}, {'d', 3, {1, 1, 7, 5, 7}},
    {'o', 3, {0, 0, 7, 5, 7}}, {'m', 5, {0, 0, 30, 21, 21}}, {'w', 5, {0, 0, 17, 21, 10}}, {'y', 3, {0, 5, 5, 2, 4}},
    {'a', 3, {0, 6, 1, 7, 7}}, {'%', 3, {5, 1, 2, 4, 5}}, {'?', 3, {7, 1, 3, 0, 2}}, {'!', 1, {1, 1, 1, 0, 1}},
};

/* Draws at scale sc, clipped to k; returns the advance width (gap after each glyph). */
static int tiny_text(Canvas *c, const Clip *k, int x, int y, int sc, uint32_t col, const char *str) {
    int x0 = x;
    for (; *str; str++) {
        int g = 0;
        while (g < (int)(sizeof TINY / sizeof TINY[0]) && TINY[g].ch != *str) g++;
        if (g == (int)(sizeof TINY / sizeof TINY[0])) continue;
        int w = TINY[g].w;
        for (int r = 0; r < 5; r++) {
            uint8_t bits = TINY[g].rows[r];
            for (int b = 0; b < w; b++)
                if (bits & (1 << (w - 1 - b))) RC(c, k, x + b * sc, y + r * sc, sc, sc, col);
        }
        x += (w + 1) * sc;
    }
    return x - x0;
}

int scene_tiny_text(Canvas *c, int x, int y, int sc, uint32_t col, const char *str) {
    static const Clip screen = {0, 0, SCREEN_W, SCREEN_H};
    return tiny_text(c, &screen, x, y, sc, col, str);
}

int scene_tiny_width(int sc, const char *str) {
    int w = 0;
    for (; *str; str++)
        for (int g = 0; g < (int)(sizeof TINY / sizeof TINY[0]); g++)
            if (TINY[g].ch == *str) {
                w += (TINY[g].w + 1) * sc;
                break;
            }
    return w;
}

/* ---- fuel panel (subscription limits) ------------------------------------------------------ */

/* Robot head (APPEARANCE_ROBOT rows 1..8) at scale 2, 24x16, its eyes showing how much fuel is left. */
static void fuel_head(Canvas *c, const Palette *pal, int x, int y, UsageLevel lvl, int64_t now, uint32_t dim_to,
                      int dim) {
    for (int r = 1; r < 9; r++)
        for (int col = 0; col < 12;) {
            char ch = APPEARANCE_ROBOT[r][col];
            int run = col + 1;
            while (run < 12 && APPEARANCE_ROBOT[r][run] == ch) run++;
            uint32_t color = ch == 'o' ? COL_OUTLINE : ch == 'b' ? pal->body : ch == 'l' ? pal->light
                           : ch == 'd'   ? pal->dark
                           : ch == 'v'   ? APPEARANCE_VISOR
                                         : 0;
            if (color) R(c, x + col * 2, y + (r - 1) * 2, (run - col) * 2, 2, lerp_col(color, dim_to, dim));
            col = run;
        }
    uint32_t glow = lerp_col(pal->accent, dim_to, dim);
    bool blink = lvl == USAGE_OK && (now / 130) % 41 == 0;
    for (int e = 0; e < 2; e++) {
        int ex = x + 7 + e * 7;
        if (lvl == USAGE_FULL || blink) R(c, ex, y + 10, 4, 1, glow); /* asleep */
        else if (lvl == USAGE_WARN) {                               /* heavy lids */
            R(c, ex, y + 9, 4, 1, lerp_col(pal->dark, dim_to, dim));
            R(c, ex + 1, y + 10, 2, 2, glow);
        } else R(c, ex + 1, y + 7, 2, 4, glow);
    }
}

#define WP_X 444
#define WP_Y 46
#define WP_W 140 /* x 590..634 is the unlockable wall-decor spot */
#define WP_H 104
#define WP_CYCLE_MS 10000 /* the wall panel alternates tok/min and fuel */
#define WP_FLIP_MS 280

/* One row per provider (up to four): robot head in its family colours, a fuel bar that drains as
 * the worst window fills, and what is left (or, when empty, how long until it refills). The title
 * row counts down to the reset of the most-used window. */
static void draw_fuel_panel(Canvas *c, const App *app, const UsageSummary *u, int n, UsageState st, const Light *L,
                            int64_t now) {
    const int x = WP_X, y = WP_Y, w = WP_W;
    const uint32_t bg = 0x12151d;
    const int dim = st == USAGE_STALE ? 130 : 0;
    R(c, x, y, w, WP_H, 0x2a3040);
    R(c, x + 2, y + 2, w - 4, WP_H - 4, bg);
    const Clip k = {x + 2, y + 2, x + w - 2, y + WP_H - 2};
    tiny_text(c, &k, x + 8, y + 6, 2, lerp_col(GOLD, bg, dim), "FUEL");
    int64_t t = app_now(app);
    int wi = 0;
    for (int i = 1; i < n; i++)
        if (u[i].worst_pm > u[wi].worst_pm) wi = i;
    char eta[16];
    if (st == USAGE_STALE) {
        tiny_text(c, &k, x + w - 8 - scene_tiny_width(2, "OLD"), y + 6, 2, DIM, "OLD");
    } else if (n && u[wi].worst_reset > t && u[wi].worst_pm > 0) {
        usage_format_eta(u[wi].worst_reset - t, eta, sizeof eta);
        int ew = scene_tiny_width(2, eta);
        uint32_t col = u[wi].level == USAGE_OK ? DIM : usage_level_col(u[wi].level);
        tiny_text(c, &k, x + w - 8 - ew, y + 6, 2, col, eta);
        /* family swatch: whose window that is */
        const Palette *pal = family_pal(u[wi].family);
        R(c, x + w - 8 - ew - 10, y + 7, 6, 8, pal->body);
        R(c, x + w - 8 - ew - 10, y + 7, 6, 2, pal->light);
    }
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) {
        int ry = y + 21 + i * 20;
        fuel_head(c, family_pal(u[i].family), x + 7, ry + 2, u[i].level, now, bg, dim);
        int left = 1000 - u[i].worst_pm;
        uint32_t col = lerp_col(usage_level_col(u[i].level), bg, dim);
        char txt[16];
        if (u[i].level == USAGE_FULL && u[i].worst_reset > t) usage_format_eta(u[i].worst_reset - t, txt, sizeof txt);
        else snprintf(txt, sizeof txt, "%d%%", (left + 5) / 10);
        int tw = (int)strlen(txt) * gfx_font_w(FONT_BODY);
        gfx_text(c, FONT_BODY, x + w - 6 - tw, ry, u[i].level == USAGE_OK ? lerp_col(TEXT, bg, dim) : col, txt, 0);
        /* tank: outline, drained track, fuel, quarter ticks */
        int bx = x + 36, bw = x + w - 6 - tw - 6 - bx, by = ry + 6;
        uint32_t rim = 0x3a4058;
        if (u[i].level == USAGE_FULL && st == USAGE_FRESH && !L->calm) /* a slow, gentle breath */
            rim = lerp_col(0x5a2024, 0xa83a3e, 128 + isin((int)(now / 32)) * 2);
        R(c, bx - 1, by - 1, bw + 2, 10, lerp_col(rim, bg, dim));
        R(c, bx, by, bw, 8, 0x1c1f28);
        int fw = left * bw / 1000;
        if (left > 0 && fw < 2) fw = 2;
        if (fw > 0) {
            R(c, bx, by, fw, 8, col);
            R(c, bx, by, fw, 2, lerp_col(lerp_col(col, 0xffffff, 90), bg, dim));
        }
        for (int q = 1; q < 4; q++) R(c, bx + bw * q / 4, by + 5, 1, 3, 0x0e1016);
    }
}

/* Wall panel at WP_X: tok/min and fuel take turns (fuel stays up while nothing is running and the
 * chart is empty); a switch reveals the new panel from its middle out, like an old CRT. */
static void draw_wall_panel(Canvas *c, const App *app, const Light *L, int64_t now) {
    UsageSummary u[MAX_USAGE_PROVIDERS];
    UsageState st = usage_state(app);
    int n = usage_summarize(app, u, MAX_USAGE_PROVIDERS);
    if (!n) {
        draw_rate_panel(c, app, now);
        return;
    }
    /* the chart shows rate[lag..]; nothing there and nobody running: keep the fuel up */
    bool quiet = app->n_running == 0;
    int64_t lag = app_now(app) / 60 - app->rate_minute;
    for (int64_t i = lag < 0 ? 0 : lag; quiet && i < RATE_MINUTES; i++)
        if (app->rate[i]) quiet = false;
    bool fuel = quiet || (now / WP_CYCLE_MS) % 2;
    if (fuel) draw_fuel_panel(c, app, u, n, st, L, now);
    else draw_rate_panel(c, app, now);
    int64_t age = now % WP_CYCLE_MS;
    if (quiet || L->calm || age >= WP_FLIP_MS) return;
    int half = (WP_H - 4) / 2, open = (int)(age * half / WP_FLIP_MS);
    int mid = WP_Y + 2 + half;
    R(c, WP_X + 2, WP_Y + 2, WP_W - 4, half - open, 0x12151d);
    R(c, WP_X + 2, mid + open, WP_W - 4, half - open, 0x12151d);
    R(c, WP_X + 2, mid - open - 1, WP_W - 4, 1, 0x5a6a8a);
    R(c, WP_X + 2, mid + open, WP_W - 4, 1, 0x5a6a8a);
}

static void format_cost(int64_t mc, char *buf, size_t len) {
    int64_t cents = (mc + 500) / 1000;
    if (cents < 0) cents = 0;
    if (cents < 10000) snprintf(buf, len, "~$%lld.%02lld", (long long)(cents / 100), (long long)(cents % 100));
    else if (cents < 1000000) snprintf(buf, len, "~$%lld", (long long)(cents / 100));
    else snprintf(buf, len, "~$%lldk", (long long)(cents / 100000));
}

static void draw_atm(Canvas *c, const Scene *s, const App *app, const Light *L, int64_t now) {
    const int x = ATM_X, y = ATM_Y, w = ATM_W, h = ATM_H;
    uint32_t body = lerp_col(0x24434c, 0x3c7684, L->day), side = lerp_col(0x1a3038, 0x2c5862, L->day);
    uint32_t hi = lerp_col(0x2e5560, 0x5a9aa8, L->day);
    R(c, x - 2, y + h - 2, w + 6, 3, L->shadow);
    /* topper sign (lit, so it glows at night too) */
    R(c, x + 2, y, w - 4, 17, COL_OUTLINE);
    R(c, x + 4, y + 2, w - 8, 13, 0x2a2140);
    const Clip all = {x, y, x + w, y + h};
    tiny_text(c, &all, x + (w - 38) / 2, y + 4, 2, (now / 1200) % 7 == 0 && !L->calm ? 0xfff3b0 : GOLD, "TODAY");
    /* cabinet */
    R(c, x, y + 16, w, h - 16, COL_OUTLINE);
    R(c, x + 1, y + 17, w - 2, h - 18, body);
    R(c, x + w - 6, y + 17, 5, h - 18, side);
    R(c, x + 1, y + 17, w - 2, 1, hi);
    /* screen: today's tokens and estimated cost, alternating, values slide in */
    R(c, x + 2, y + 19, w - 4, 26, 0x101218);
    const Clip scr = {x + 3, y + 20, x + w - 3, y + 44};
    RC(c, &scr, scr.x0, scr.y0, scr.x1 - scr.x0, scr.y1 - scr.y0, 0x0b1a14);
    bool cost = (now / ATM_CYCLE_MS) % 2;
    int64_t in = now % ATM_CYCLE_MS;
    int slide = L->calm || in >= 240 ? 0 : (int)(240 - in) / 20;
    char val[48]; /* roomy enough that gcc 8's -Wformat-truncation sees any int64 fits */
    if (cost) format_cost(app->day_cost_mc, val, sizeof val);
    else format_tokens(app->day_tokens, val, sizeof val);
    uint32_t lcd = 0x7ee787;
    int tw = tiny_text(c, &scr, scr.x0 + 2, scr.y0 + 3 + slide, 2, lcd, val);
    if ((now / 500) % 2 && scr.x0 + 2 + tw + 2 <= scr.x1) RC(c, &scr, scr.x0 + 2 + tw, scr.y0 + 3 + slide, 2, 10, 0x3a8a50);
    tiny_text(c, &scr, scr.x0 + 2, scr.y0 + 17 + slide, 1, 0x3a8a50, cost ? "EST COST" : "TOKENS");
    /* keypad, card slot */
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 3; k++) R(c, x + 6 + k * 8, y + 48 + r * 6, 6, 4, k == 2 && r == 1 ? 0x46c06f : 0xc8ccd6);
    R(c, x + 34, y + 50, 20, 3, 0x101218);
    bool printing = s && s->receipt_at && now - s->receipt_at < RECEIPT_PRINT_MS;
    R(c, x + 50, y + 55, 3, 2, printing && (now / 120) % 2 ? 0xffd166 : 0x2a6a40);
    /* receipt slot + plinth */
    R(c, x + 28, y + 60, 24, 3, 0x101218);
    R(c, x + 1, y + 66, w - 2, h - 67, side);
    R(c, x + 4, y + 69, w - 8, 2, body);
    /* receipt, printed in jerks (one row per 90 ms), hangs until torn off */
    if (s && s->receipt_at && now >= s->receipt_at) {
        int len = (int)((now - s->receipt_at) / 90);
        if (len > 16) len = 16;
        int rx = x + 31, ry = y + 62;
        R(c, rx, ry, 18, len, 0xf4f0e0);
        uint32_t hsh = hash_u((uint32_t)s->receipt_at);
        for (int k = 3; k < len - 1; k += 3) R(c, rx + 2, ry + k, 6 + (int)((hsh >> k) % 9), 1, 0xa8a090);
        if (len >= 16)
            for (int k = 0; k < 18; k += 4) R(c, rx + k, ry + 16, 2, 1, 0xf4f0e0); /* zigzag edge */
    }
}

/* ---- workers ----------------------------------------------------------------------------- */

typedef enum { EYE_OPEN, EYE_WIDE, EYE_CLOSED, EYE_HAPPY, EYE_X, EYE_TIRED } Eyes;
typedef enum { ARM_TYPE, ARM_REST, ARM_UP, ARM_WAVE, ARM_STRETCH } Arms;

/* For ARM_STRETCH, `tick` is how far the arms are raised (px) instead of an animation clock.
 * costume: the family's equipped WEAR_BOT_* id or -1, drawn over the body under the eyes and arms. */
static void draw_worker(Canvas *c, const Palette *pal, int x, int y, Eyes eyes, int look, Arms arms, int tick,
                        int costume) {
    for (int r = 0; r < 12; r++) {
        const char *row = APPEARANCE_ROBOT[r];
        for (int col = 0; col < 12;) {
            char ch = row[col];
            int run = col + 1;
            while (run < 12 && row[run] == ch) run++;
            uint32_t color = 0;
            bool draw = true;
            switch (ch) {
            case 'o': color = COL_OUTLINE; break;
            case 'b': color = pal->body; break;
            case 'l': color = pal->light; break;
            case 'd': color = pal->dark; break;
            case 'a': color = pal->accent; break;
            case 'v': color = APPEARANCE_VISOR; break;
            default: draw = false;
            }
            if (draw) R(c, x + col * WS, y + r * WS, (run - col) * WS, WS, color);
            col = run;
        }
    }
    appearance_draw_robot(c, costume, x, y, WS);
    uint32_t glow = pal->accent;
    int ey = y + 13;
    for (int e = 0; e < 2; e++) {
        int ex = x + 11 + e * 9;
        switch (eyes) {
        case EYE_OPEN: R(c, ex + 1 + look * 2, ey, 3, 6, glow); break;
        case EYE_TIRED: /* heavy lids over half-open eyes */
            R(c, ex + look * 2, ey + 1, 5, 2, pal->dark);
            R(c, ex + 1 + look * 2, ey + 3, 3, 3, glow);
            break;
        case EYE_WIDE:
            R(c, ex, ey - 1, 5, 7, glow);
            R(c, ex + 1, ey, 2, 2, 0xffffff);
            break;
        case EYE_CLOSED: R(c, ex, ey + 4, 5, 2, glow); break;
        case EYE_HAPPY:
            R(c, ex, ey + 3, 2, 2, glow);
            R(c, ex + 2, ey + 1, 2, 2, glow);
            R(c, ex + 4, ey + 3, 2, 2, glow);
            break;
        case EYE_X:
            R(c, ex, ey, 2, 2, 0xff5a5a);
            R(c, ex + 4, ey, 2, 2, 0xff5a5a);
            R(c, ex + 2, ey + 2, 2, 2, 0xff5a5a);
            R(c, ex, ey + 4, 2, 2, 0xff5a5a);
            R(c, ex + 4, ey + 4, 2, 2, 0xff5a5a);
            break;
        }
    }
    /* raised arms live above the desk, so draw them with the body */
    if (arms == ARM_UP || arms == ARM_WAVE || arms == ARM_STRETCH) {
        for (int side = 0; side < 2; side++) {
            if (arms == ARM_WAVE && side == 0) continue;
            int up = arms == ARM_STRETCH ? tick : (tick / 2 + side) % 2 ? 3 : 0;
            int ax = side ? x + 11 * WS : x - 1;
            R(c, ax, y + 4 * WS - up, WS + 1, 6 * WS, COL_OUTLINE);
            R(c, ax + 1, y + 4 * WS - up + 1, WS - 1, 6 * WS - 2, pal->dark);
            R(c, ax - 1, y + 2 * WS - up, WS + 3, WS + 2, COL_OUTLINE);
            R(c, ax, y + 2 * WS - up + 1, WS + 1, WS, pal->light);
        }
    }
}

static void draw_hand(Canvas *c, const Palette *pal, int x, int y) {
    R(c, x, y, 8, 6, COL_OUTLINE);
    R(c, x + 1, y + 1, 6, 4, pal->light);
}

static const uint8_t BM_CHECK[8] = {0x00, 0x03, 0x07, 0x8e, 0xdc, 0xf8, 0x70, 0x20};
static const uint8_t BM_X[8] = {0xc3, 0xe7, 0x7e, 0x3c, 0x3c, 0x7e, 0xe7, 0xc3};
static const uint8_t BM_BANG[8] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00};
static const uint8_t BM_Z[8] = {0x00, 0x7c, 0x0c, 0x18, 0x30, 0x7c, 0x00, 0x00};

static void draw_screen(Canvas *c, const Agent *a, const Palette *pal, int x, int y, uint32_t h, int64_t now) {
    const int w = 40, hh = 24;
    int ph = (int)(h % 997);
    switch (a->bucket) {
    case B_RUNNING: {
        R(c, x, y, w, hh, 0x0b1a14);
        static const uint32_t code_cols[4] = {0x7ee787, 0x79c0ff, 0xd2a8ff, 0x8a90a0};
        int64_t t = now + ph * 3;
        int scroll = (int)(t / 240);
        for (int k = 0; k < 5; k++) {
            uint32_t lh = hash_u((uint32_t)(scroll + k) * 2654435761u ^ h);
            int indent = (int)(lh % 3) * 4;
            int lw = 6 + (int)((lh >> 4) % 22);
            if (indent + lw > w - 4) lw = w - 4 - indent;
            if (k == 4) lw = lw * (int)(t % 240) / 240;
            int ly = y + 2 + k * 4 + 1;
            int split = lw / 3;
            R(c, x + 2 + indent, ly, split, 2, code_cols[(lh >> 9) & 3]);
            R(c, x + 3 + indent + split, ly, lw - split, 2, k == 4 ? pal->light : code_cols[(lh >> 11) & 3]);
            if (k == 4 && (t / 200) % 2) R(c, x + 4 + indent + lw, ly - 1, 2, 4, WHITE);
        }
        break;
    }
    case B_NEEDS_INPUT:
        R(c, x, y, w, hh, 0x2e1a0c);
        gfx_text(c, FONT_BODY, x + 15, y + 2, (now / 350) % 2 ? ALERT_COL : 0xffb08a, "?", 0);
        R(c, x + 4, y + hh - 4, 8, 2, 0x8a5a3a);
        R(c, x + 28, y + hh - 4, 8, 2, 0x8a5a3a);
        break;
    case B_FAILED:
        R(c, x, y, w, hh, (now / 280) % 3 ? 0x7a1820 : 0x5a1016);
        bitmap8(c, BM_X, x + 12, y + 4, 2, 0xffb0b0);
        break;
    case B_ATTENTION:
        R(c, x, y, w, hh, 0x0f2416);
        bitmap8(c, BM_CHECK, x + 8, y, 3, 0x46c06f);
        break;
    case B_DONE:
    default: {
        R(c, x, y, w, hh, 0x14161c);
        R(c, x + 3, y + 3, 6, 2, 0x22252e);
        R(c, x + 3, y + 5, 2, 4, 0x22252e);
        /* screensaver dot drifting around */
        int t = (int)((now + ph * 7) / 120);
        int dx = t % (2 * (w - 4)), dy = (t * 3 / 4) % (2 * (hh - 4));
        if (dx >= w - 4) dx = 2 * (w - 4) - dx - 1;
        if (dy >= hh - 4) dy = 2 * (hh - 4) - dy - 1;
        R(c, x + 1 + dx, y + 1 + dy, 2, 2, 0x3a4058);
        break;
    }
    }
}

static void draw_bubble(Canvas *c, int x, int y, int w, int h, uint32_t fill) {
    R(c, x + 1, y, w - 2, h, COL_OUTLINE);
    R(c, x, y + 1, w, h - 2, COL_OUTLINE);
    R(c, x + 2, y + 1, w - 4, h - 2, fill);
    R(c, x + 1, y + 2, w - 2, h - 4, fill);
    R(c, x + w / 2 - 3, y + h - 1, 6, 3, COL_OUTLINE);
    R(c, x + w / 2 - 2, y + h - 1, 4, 2, fill);
    R(c, x + w / 2 - 1, y + h + 2, 2, 2, COL_OUTLINE);
}

/* stretch_age: ms into an ambient stretch for this worker, -1 for none. fatigue: how spent the
 * worker's provider is (usage_family_level): WARN looks tired, FULL sleepy with an empty battery. */
static void draw_desk(Canvas *c, const App *app, const Light *L, int i, int n, int stretch_age, UsageLevel fatigue,
                      int64_t now) {
    const Row *row = &app->rows[i];
    const Agent *a = &row->agent;
    int sx, d;
    desk_pos(i, n, &sx, &d);
    uint32_t h = hash_str(a->id, sizeof a->id, (uint32_t)row->bridge * 977u);
    int ph = (int)(h % 1000);
    const Palette *pal = family_pal(a->family);
    int wx = sx + 26, wy = d - 30;
    int mx = sx + 80, my = d - 40;
    int tick = (int)((now + ph) / ((L->calm ? 330 : 110) * (1 + (int)fatigue))); /* tired hands type slower */

    int dy = 0, look = 0;
    Eyes eyes = EYE_OPEN;
    Arms arms = ARM_REST;
    bool blink = ((now + ph * 13) / 110) % 37 == 0;
    switch (a->bucket) {
    case B_RUNNING:
        look = 1;
        arms = ARM_TYPE;
        dy = ((now + ph) / 440) % 2 ? 0 : -1;
        if (blink) eyes = EYE_CLOSED;
        break;
    case B_NEEDS_INPUT: {
        eyes = EYE_WIDE;
        arms = ARM_WAVE;
        int p = (int)((now + ph) % 760);
        if (p < 420) dy = -(10 * 4 * p * (420 - p)) / (420 * 420);
        break;
    }
    case B_FAILED:
        eyes = EYE_X;
        dy = 3;
        break;
    case B_ATTENTION: {
        eyes = EYE_HAPPY;
        arms = ARM_UP;
        int p = (int)((now + ph) % 900);
        if (p < 300) dy = -(5 * 4 * p * (300 - p)) / (300 * 300);
        break;
    }
    case B_DONE:
    default:
        eyes = EYE_CLOSED;
        dy = 2;
        break;
    }
    /* out of subscription fuel: droopy at WARN, dozing at FULL (urgent states keep their faces) */
    if (fatigue != USAGE_OK && (a->bucket == B_RUNNING || a->bucket == B_ATTENTION || a->bucket == B_DONE)) {
        if (fatigue == USAGE_FULL) eyes = EYE_CLOSED;
        else if (eyes == EYE_OPEN) eyes = EYE_TIRED;
        if (a->bucket != B_DONE) dy += (int)fatigue;
    }
    int raise = 0;
    if (stretch_age >= 0) { /* arms up, eyes shut, a long reach */
        arms = ARM_STRETCH;
        eyes = EYE_CLOSED;
        dy = -3;
        raise = stretch_age < 400 ? stretch_age * 9 / 400 : stretch_age > 2000 ? (2400 - stretch_age) * 9 / 400 : 9;
    }

    /* chair back, worker, desk, keyboard, hands, monitor */
    R(c, wx + 1, d - 16, 34, 18, 0x2a2f40);
    R(c, wx + 1, d - 16, 34, 2, 0x3d4459);
    draw_worker(c, pal, wx, wy + dy, eyes, look, arms, arms == ARM_STRETCH ? raise : tick,
                worn(app, pet_robot_slot(a->family)));
    if (fatigue == USAGE_FULL && (a->bucket == B_RUNNING || a->bucket == B_DONE)) {
        /* empty battery over the head (clear of the wall panel above row one), last cell breathing */
        int bx = wx + 11, by = wy + dy - 9;
        R(c, bx, by, 13, 7, 0x9aa0b0);
        R(c, bx + 13, by + 2, 2, 3, 0x9aa0b0);
        R(c, bx + 1, by + 1, 11, 5, 0x22252e);
        uint32_t cell = L->calm ? 0xa83a3e : lerp_col(0x6a2024, 0xe5484d, 128 + isin((int)((now + ph) / 24)) * 2);
        R(c, bx + 2, by + 2, 2, 3, cell);
    }
    if (a->ctx_pct >= CTX_WARN) { /* context nearly full: sweat drops */
        int cyc = L->calm ? 1400 : 700;
        for (int k = 0; k < 2; k++) {
            int p = (int)((now + ph + k * cyc / 2) % cyc);
            int fx = k ? wx - 3 : wx + 37, fy = wy + dy + 8 + p * 12 / cyc;
            R(c, fx, fy, 2, 3, 0x7ac8ff);
            R(c, fx, fy, 1, 1, 0xe8f6ff);
            R(c, fx + (k ? -1 : 2), fy - 2, 1, 2, 0x7ac8ff);
        }
    }
    R(c, sx + 14, d + 31, 132, 3, L->shadow);
    R(c, sx + 18, d + 18, 5, 14, 0x3e2a1c);
    R(c, sx + 137, d + 18, 5, 14, 0x3e2a1c);
    R(c, sx + 14, d, 132, 6, 0x8a5e3e);
    R(c, sx + 14, d, 132, 1, 0xa87650);
    R(c, sx + 16, d + 6, 128, 12, 0x64432c);
    R(c, sx + 74, d + 10, 12, 2, 0x3e2a1c);
    R(c, wx + 4, d - 3, 28, 3, 0x4a5060);
    for (int k = 0; k < 6; k++) R(c, wx + 6 + k * 4, d - 3, 2, 1, 0x8a90a0);
    switch (arms) {
    case ARM_TYPE: {
        int alt = tick % 2;
        draw_hand(c, pal, wx + 2, d - 6 - (alt ? 2 : 0));
        draw_hand(c, pal, wx + 26, d - 6 - (alt ? 0 : 2));
        break;
    }
    case ARM_REST: draw_hand(c, pal, wx + 2, d - 5), draw_hand(c, pal, wx + 26, d - 5); break;
    case ARM_WAVE: draw_hand(c, pal, wx + 2, d - 5); break;
    case ARM_UP:
    case ARM_STRETCH: break;
    }
    if (a->ctx_pct >= CTX_WARN) {
        bool sleeping = a->bucket == B_DONE ||
                        (fatigue == USAGE_FULL && (a->bucket == B_RUNNING || a->bucket == B_ATTENTION));
        uint32_t rim = 0xf5c542;
        if (sleeping) {
            /* Four-second breath (eight at night), never fully dark. */
            int phase = (int)(((now + ph) % (L->calm ? 8000 : 4000)) * 64 / (L->calm ? 8000 : 4000));
            rim = lerp_col(0x7a6a2a, 0xc5a33a, 128 + isin(phase) * 2);
        } else if (a->bucket == B_RUNNING) {
            /* Slow, regular blink; urgent states keep a steady context rim. */
            if (((now + ph) / (L->calm ? 2000 : 1000)) % 2) rim = 0x7a6a2a;
        }
        R(c, mx - 2, my - 2, 50, 36, rim);
    }
    R(c, mx, my, 46, 32, 0x2b2f3a);
    R(c, mx, my, 46, 1, 0x454b5c);
    R(c, mx + 19, my + 32, 8, 5, 0x2b2f3a);
    R(c, mx + 13, d - 3, 20, 3, 0x3a3f4c);
    draw_screen(c, a, pal, mx + 3, my + 3, h, now);
    uint32_t led = a->bucket == B_DONE ? 0x3a3f4c : BUCKET_COL[a->bucket];
    if (a->bucket == B_RUNNING && (tick / 3) % 2) led = 0x2a6a40;
    R(c, mx + 40, my + 28, 3, 2, led);
    if ((h >> 5) & 1) { /* mug */
        uint32_t mug = (h >> 6) & 1 ? 0xd0d4de : 0xc8644a;
        R(c, sx + 131, d - 9, 8, 9, mug);
        R(c, sx + 139, d - 7, 3, 2, mug);
        R(c, sx + 140, d - 5, 2, 2, mug);
        R(c, sx + 132, d - 8, 6, 1, 0x4a2a1a);
        if (a->bucket == B_RUNNING) {
            int st = (int)((now + ph) / 150);
            for (int k = 0; k < 3; k++) {
                int yy = d - 12 - ((st + k * 3) % 9) * 2;
                R(c, sx + 133 + isin(st * 4 + k * 20) / 32, yy, 2, 2, 0x8a90a0);
            }
        }
    }
    if (a->ctx_pct >= CTX_FULL) { /* tiny warning sign on the desk */
        int cx = sx + 68, top = d - 11;
        for (int r = 0; r < 11; r++) {
            int half = r * 6 / 10;
            R(c, cx - half - 1, top + r, 2 * half + 3, 1, COL_OUTLINE);
            if (r > 0 && r < 10) R(c, cx - half, top + r, 2 * half + 1, 1, 0xf5c542);
        }
        uint32_t bang = !L->calm && (now / 500) % 2 ? 0xc0302a : 0x2a1a10;
        R(c, cx, top + 3, 1, 4, bang);
        R(c, cx, top + 8, 1, 1, bang);
    }

    /* mood effects */
    switch (a->bucket) {
    case B_NEEDS_INPUT:
        if ((now + ph) / 320 % 4 != 3) {
            int by = d - 66;
            draw_bubble(c, wx + 6, by, 24, 24, WHITE);
            bitmap8(c, BM_BANG, wx + 10, by + 4, 2, ALERT_COL);
        }
        break;
    case B_ATTENTION: {
        int bob = ((now + ph) / 300) % 2 ? -2 : 0;
        int bx = wx + 8, by = d - 62 + bob;
        R(c, bx + 1, by, 20, 22, COL_OUTLINE);
        R(c, bx, by + 1, 22, 20, COL_OUTLINE);
        R(c, bx + 2, by + 2, 18, 18, 0xf5c542);
        R(c, bx + 2, by + 2, 18, 3, 0xffe08a);
        bitmap8(c, BM_CHECK, bx + 3, by + 3, 2, 0x2a4a1a);
        break;
    }
    case B_FAILED:
        for (int k = 0; k < 3; k++) {
            int p = (int)((now + ph + k * 700) % 2100);
            int rise = p * 36 / 2100, sz = 4 + p * 6 / 2100;
            uint32_t col = p < 700 ? 0x8a8f9a : p < 1400 ? 0x6a6e7a : 0x4a4e5c;
            int px = mx + 20 + k * 3 - sz / 2 + isin(p / 40 + k * 21) / 24;
            R(c, px, my - 2 - rise - sz, sz, sz, col);
            R(c, px + sz / 2, my - 2 - rise - sz - sz / 3, sz * 2 / 3, sz * 2 / 3, col);
        }
        break;
    case B_DONE:
    default:
        if (a->bucket == B_DONE)
            for (int k = 0; k < 2; k++) {
                int p = (int)((now + ph + k * 1100) % 2200);
                int zx = wx + 32 + p * 8 / 2200, zy = d - 36 - p * 22 / 2200;
                if (p < 1900) {
                    if (k) bitmap8(c, BM_Z, zx, zy, 1, DIM);
                    else gfx_text(c, FONT_BODY, zx, zy - 10, DIM, "z", 0);
                }
            }
        break;
    case B_RUNNING: break;
    }

    /* label: status icon + first 8 chars of the title */
    const char *label = a->title[0] ? a->title : a->id;
    int len = 0;
    while (len < 8 && label[len]) len++;
    while (len > 0 && label[len - 1] == ' ') len--;
    int total = 20 + len * gfx_font_w(FONT_BODY);
    int lx = sx + SLOT_W / 2 - total / 2;
    scene_draw_agent_icon(c, a, row->alert, lx, d + 37, now);
    gfx_text(c, FONT_BODY, lx + 20, d + 35, a->bucket == B_DONE ? DIM : TEXT, label, len);
}

/* ---- particles + pet --------------------------------------------------------------------- */

static const uint8_t BM_HEART[8] = {0x6c, 0xfe, 0xfe, 0x7c, 0x38, 0x10, 0x00, 0x00};

static void draw_particles(Canvas *c, const Scene *s, int64_t now) {
    float tx, ty;
    pet_center(s, &tx, &ty);
    ty += 1;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &s->p[i];
        if (!p->alive || now < p->born) continue;
        int64_t age = now - p->born;
        if (age >= p->life) continue;
        float u = (float)age / (float)p->life, t = (float)age / 1000.f;
        switch (p->kind) {
        case P_CRUMB: {
            for (int tr = 2; tr >= 0; tr--) {
                float uu = u - (float)tr * 0.06f;
                if (uu < 0) continue;
                float e = uu * (0.6f + 0.4f * uu); /* starts brisk, lands softly */
                int x = (int)(p->x + (tx - p->x) * e), y = (int)(p->y + (ty - p->y) * e - p->vx * 4 * uu * (1 - uu));
                if (tr) {
                    int sz = tr == 1 ? 4 : 2;
                    R(c, x - sz / 2, y - sz / 2, sz, sz, lerp_col(p->color, 0x302430, tr * 80));
                    continue;
                }
                R(c, x - 4, y - 4, 8, 8, COL_OUTLINE);
                R(c, x - 3, y - 3, 6, 6, p->color);
                R(c, x - 1, y + 1, 4, 2, lerp_col(p->color, 0x000000, 70));
                R(c, x - 3, y - 3, 2, 2, 0xffffff);
            }
            break;
        }
        case P_CONFETTI:
        case P_SHELL:
        case P_SPARK:
        case P_BIT: {
            int x = (int)(p->x + p->vx * t), y = (int)(p->y + p->vy * t + 0.5f * p->g * t * t);
            if (p->kind == P_CONFETTI) {
                bool flip = ((age / 90) + i) % 2;
                R(c, x, y, flip ? 4 : 2, flip ? 2 : 4, p->color);
            } else if (p->kind == P_SHELL) {
                R(c, x, y, 5, 4, COL_OUTLINE);
                R(c, x + 1, y + 1, 3, 2, p->color);
            } else {
                int sz = u < 0.5f ? 3 : 2;
                R(c, x, y, sz, sz, p->color);
            }
            break;
        }
        case P_SPARKLE: {
            float e = 1 - (1 - u) * (1 - u);
            int x = (int)(p->x + p->vx * e), y = (int)(p->y + p->vy * e);
            int arm = u < 0.3f ? 3 : u < 0.7f ? 2 : 1;
            bool twinkle = ((age / 70) + i) % 3 != 0;
            R(c, x - 1, y - 1, 3, 3, p->color);
            if (twinkle) {
                R(c, x - 1 - arm, y, arm, 1, p->color);
                R(c, x + 2, y, arm, 1, p->color);
                R(c, x, y - 1 - arm, 1, arm, p->color);
                R(c, x, y + 2, 1, arm, p->color);
            }
            break;
        }
        case P_PAPER: { /* the torn receipt flutters away, turning over as it goes */
            int x = (int)(p->x + p->vx * t) + isin((int)(age / 40)) * 4 / 64;
            int y = (int)(p->y + p->vy * t + 0.5f * p->g * t * t);
            int f = (int)(age / 130) % 4, pw = f == 1 || f == 3 ? 4 : 10;
            R(c, x - pw / 2, y, pw, 12, p->color);
            if (pw > 4)
                for (int k = 3; k < 12; k += 3) R(c, x - pw / 2 + 2, y + k, pw - 5, 1, 0xa8a090);
            break;
        }
        case P_HEART: {
            int x = (int)p->x + isin((int)p->vx + (int)(age / 45)) / 10;
            int y = (int)(p->y - p->vy * t);
            int sc = u < 0.7f ? 3 : 2;
            bitmap8(c, BM_HEART, x - 1, y, sc, COL_OUTLINE);
            bitmap8(c, BM_HEART, x + 1, y, sc, COL_OUTLINE);
            bitmap8(c, BM_HEART, x, y - 1, sc, COL_OUTLINE);
            bitmap8(c, BM_HEART, x, y + 1, sc, COL_OUTLINE);
            bitmap8(c, BM_HEART, x, y, sc, COL_CHEEK);
            R(c, x + sc, y + sc, sc, sc, 0xffd6de);
            break;
        }
        case P_RING: {
            int r = 8 + (int)(u * 26);
            int th = u < 0.6f ? 2 : 1;
            int x = (int)p->x, y = (int)p->y;
            R(c, x - r, y - r, 2 * r, th, p->color);
            R(c, x - r, y + r - th, 2 * r, th, p->color);
            R(c, x - r, y - r, th, 2 * r, p->color);
            R(c, x + r - th, y - r, th, 2 * r, p->color);
            break;
        }
        }
    }
}

static void draw_pet(Canvas *c, const Scene *s, const App *app, const Light *L, int64_t now) {
    if (!app->pet_ready) return;
    int px = (int)s->pet_x, py = pet_y_at(s->pet_x);
    bool egg = app->pet.stage == STAGE_EGG;
    if (egg) { /* nest back */
        R(c, px + 8, py + 62, 64, 10, 0x6a4f2a);
        R(c, px + 12, py + 60, 56, 4, 0x8a6a3a);
    } else if (py == PET_Y) {
        R(c, px + 16, py + 74, 48, 4, L->shadow);
    }
    ui_draw_pet(c, app, px, py, PET_S, now);
    R(c, BED_X - 2, 437, 88, 10, lerp_col(0x3a3566, 0x5e5aa8, L->day));
    R(c, BED_X - 2, 437, 88, 2, lerp_col(0x4e4a80, 0x8a86d8, L->day));
    if (egg) { /* nest front lip over the egg's base */
        R(c, px + 4, py + 68, 72, 10, 0x8a6a3a);
        R(c, px + 8, py + 76, 64, 3, 0x5a4020);
        for (int k = 0; k < 7; k++) R(c, px + 8 + k * 10, py + 69 + (k & 1) * 3, 7, 2, 0xb08a4a);
    }
    if (now < s->nom_until) {
        int hop = (int)((s->nom_until - now) / 110 % 2) * 2;
        gfx_text(c, FONT_BODY, px + 16 * PET_S - 6, py + 18 - hop, GOLD, "nom", 0);
    }
}

static void draw_banner(Canvas *c, const Scene *s, int64_t now) {
    int64_t age = now - s->banner_at;
    if (!s->banner[0] || s->banner_at == 0 || age < 0 || age > 2600) return;
    int w = (int)strlen(s->banner) * gfx_font_w(FONT_BIG);
    int x = (int)s->pet_x + 8 * PET_S - w / 2;
    if (x < 8) x = 8;
    if (x + w > SCREEN_W - 8) x = SCREEN_W - 8 - w;
    int y = PET_Y - 44 - (age < 240 ? (int)(240 - age) / 8 : 0);
    R(c, x - 8, y - 2, w + 16, 36, COL_OUTLINE);
    R(c, x - 6, y, w + 12, 32, 0x2a2140);
    gfx_text(c, FONT_BIG, x + 2, y + 2, 0x1b1b24, s->banner, 0);
    gfx_text(c, FONT_BIG, x, y, (age / 120) % 2 ? GOLD : 0xfff3b0, s->banner, 0);
}

static void draw_flash(Canvas *c, const Scene *s, int64_t now) {
    int64_t age = now - s->flash_at;
    if (s->flash_at == 0 || age < 0 || age > 420) return;
    if (age < 50) {
        R(c, 0, DEN_Y, SCREEN_W, DEN_B - DEN_Y, 0xfff6dc);
        return;
    }
    int th = (int)(420 - age) / 22;
    if (th <= 0) return;
    uint32_t col = lerp_col(0xfff6dc, GOLD, (int)(age - 50) * 256 / 370);
    R(c, 0, DEN_Y, SCREEN_W, th, col);
    R(c, 0, DEN_B - th, SCREEN_W, th, col);
    R(c, 0, DEN_Y, th, DEN_B - DEN_Y, col);
    R(c, SCREEN_W - th, DEN_Y, th, DEN_B - DEN_Y, col);
}

/* "NEW! <cosmetic>" over the pet after EV_UNLOCKED (above the hatch/evolve banner if both). */
static void draw_unlock_tag(Canvas *c, const Scene *s, int64_t now) {
    int64_t age = now - s->unlock_at;
    if (!s->unlock_at || age < 0 || age > UNLOCK_TAG_MS) return;
    const char *name = pet_unlock_name(s->unlock_id);
    int cw = gfx_font_w(FONT_BODY), nlen = (int)strlen(name);
    if (nlen > 16) nlen = 16;
    int tag_w = 4 * cw + 8, w = tag_w + (nlen ? nlen * cw + 10 : 0);
    int x = (int)s->pet_x + 8 * PET_S - w / 2;
    if (x < 8) x = 8;
    if (x + w > SCREEN_W - 8) x = SCREEN_W - 8 - w;
    bool banner = s->banner_at && now - s->banner_at >= 0 && now - s->banner_at <= 2600;
    int y = (banner ? PET_Y - 84 : PET_Y - 30) - (age < 200 ? (int)(200 - age) / 10 : 0) - ((age / 600) % 2 ? 1 : 0);
    R(c, x - 1, y - 1, w + 2, 24, COL_OUTLINE);
    R(c, x, y, w, 22, 0x2a2140);
    R(c, x, y, tag_w, 22, (age / 300) % 2 ? GOLD : 0xfff3b0);
    gfx_text(c, FONT_BODY, x + 4, y + 1, 0x2a1a10, "NEW!", 0);
    if (nlen) gfx_text(c, FONT_BODY, x + tag_w + 5, y + 1, TEXT, name, nlen);
}

void scene_draw_den(const Scene *s, const App *app, Canvas *c, int64_t now_ms) {
    Light L = light_for(app, now_ms);
    draw_room(c, &L, s, app, now_ms);
    pet_draw_den_decor(c, app, now_ms);
    if (s) draw_rug_ripple(c, s, &L, now_ms);
    draw_atm(c, s, app, &L, now_ms);
    int n = desk_count(app);
    /* an ambient stretch goes to a dozing robot (none if nobody is) */
    int stretcher = -1;
    if (L.amb.kind == AMB_STRETCH && n > 0)
        for (int k = 0; k < n && stretcher < 0; k++) {
            int j = (int)((L.amb.h + (uint32_t)k) % (uint32_t)n);
            if (app->rows[j].agent.bucket == B_DONE) stretcher = j;
        }
    UsageLevel fatigue[N_FAMILIES];
    for (int f = 0; f < N_FAMILIES; f++) fatigue[f] = usage_family_level(app, (Family)f);
    for (int i = 0; i < n; i++) {
        Family f = app->rows[i].agent.family;
        draw_desk(c, app, &L, i, n, i == stretcher ? (int)L.amb.age : -1,
                  fatigue[(unsigned)f < N_FAMILIES ? f : FAM_OTHER], now_ms);
    }
    if (n == 0) {
        const char *msg = "No agents right now";
        int w = (int)strlen(msg) * gfx_font_w(FONT_BODY);
        gfx_text(c, FONT_BODY, (SCREEN_W - w) / 2, 250, DIM, msg, 0);
    } else if (app->n_rows > n) {
        char more[24];
        snprintf(more, sizeof more, "+%d more", app->n_rows - n);
        gfx_text(c, FONT_BODY, SCREEN_W - 8 - (int)strlen(more) * gfx_font_w(FONT_BODY), 400, DIM, more, 0);
    }
    if (s) {
        draw_pet(c, s, app, &L, now_ms);
        draw_particles(c, s, now_ms);
        draw_banner(c, s, now_ms);
        draw_unlock_tag(c, s, now_ms);
        draw_flash(c, s, now_ms);
    }
}

/* ---- list widgets ------------------------------------------------------------------------ */

void scene_draw_agent_icon(Canvas *c, const Agent *a, bool alert, int x, int y, int64_t now_ms) {
    bool pulse = alert && (now_ms / 400) % 2;
    switch (a->bucket) {
    case B_RUNNING: {
        static const int8_t DOT[8][2] = {{0, -6}, {4, -4}, {6, 0}, {4, 4}, {0, 6}, {-4, 4}, {-6, 0}, {-4, -4}};
        int head = (int)(now_ms / 90 % 8);
        for (int k = 0; k < 8; k++) {
            int age = (head - k + 8) % 8;
            uint32_t col = age == 0 ? 0xa6f5bf : age == 1 ? 0x46c06f : age == 2 ? 0x2d7a47 : 0x26302e;
            gfx_rect(c, x + 7 + DOT[k][0], y + 7 + DOT[k][1], 3, 3, col);
        }
        break;
    }
    case B_NEEDS_INPUT: {
        uint32_t bg = alert && !pulse ? 0xffa080 : ALERT_COL;
        gfx_rect(c, x + 1, y, 14, 16, bg);
        gfx_rect(c, x, y + 1, 16, 14, bg);
        bitmap8(c, BM_BANG, x, y + 1, 2, alert && !pulse ? ALERT_COL : WHITE);
        break;
    }
    case B_FAILED: {
        int shake = alert && (now_ms / 80) % 6 == 0 ? 1 : 0;
        bitmap8(c, BM_X, x + shake, y, 2, pulse ? 0xff8a8d : BUCKET_COL[B_FAILED]);
        break;
    }
    case B_ATTENTION: {
        int bob = alert && (now_ms / 250) % 2 ? -1 : 0;
        bitmap8(c, BM_CHECK, x, y + 1 + bob, 2, pulse ? 0xffe08a : BUCKET_COL[B_ATTENTION]);
        break;
    }
    case B_DONE:
    default: {
        int rise = (int)(now_ms / 500 % 3);
        bitmap8(c, BM_Z, x + 1, y + 2 - rise, 2, BUCKET_COL[B_DONE]);
        break;
    }
    }
    if (pulse) gfx_frame(c, x - 2, y - 2, 20, 20, 1, BUCKET_COL[a->bucket < B_DONE ? a->bucket : B_DONE]);
}

void scene_draw_sparkline(Canvas *c, const App *app, int x, int y, int w, int h, int64_t now_ms) {
    int cw = w / RATE_MINUTES;
    if (cw < 1) cw = 1;
    int bw = cw > 2 ? cw - 1 : cw;
    int x0 = x + w - cw * RATE_MINUTES;
    /* main shifts rate[] when tokens arrive; minutes since then are empty */
    int64_t lag = app_now(app) / 60 - app->rate_minute;
    if (lag < 0) lag = 0;
    if (lag > RATE_MINUTES) lag = RATE_MINUTES;
    int64_t top = 200;
    for (int i = 0; i < RATE_MINUTES; i++)
        if (app->rate[i] > top) top = app->rate[i];
    uint32_t stop = isqrt64((uint64_t)top);
    int seg = h >= 24 ? 4 : 2;
    int base = y + h - 1;
    int segs = (h - 1 - seg) / seg; /* leave room for the live cap */
    if (segs < 1) segs = 1;
    gfx_rect(c, x0, base, cw * RATE_MINUTES - (cw - bw), 1, 0x2a3040);
    int cur_top = base;
    for (int col = 0; col < RATE_MINUTES; col++) {
        int src = col + (int)lag;
        int64_t v = src < RATE_MINUTES ? app->rate[src] : 0;
        int bx = x0 + col * cw;
        bool cur = col == RATE_MINUTES - 1;
        if (v <= 0) {
            if (cur) cur_top = base;
            continue;
        }
        int n = (int)(((uint64_t)isqrt64((uint64_t)v) * (uint64_t)segs + stop - 1) / stop);
        if (n < 1) n = 1;
        if (n > segs) n = segs;
        uint32_t body = cur ? 0x46c06f : lerp_col(0x1f4a3c, 0x2f8a6a, col * 256 / (RATE_MINUTES - 1));
        uint32_t tip = cur ? 0xa6f5bf : lerp_col(0x2f6a54, 0x56c89a, col * 256 / (RATE_MINUTES - 1));
        for (int k = 0; k < n; k++)
            gfx_rect(c, bx, base - (k + 1) * seg, bw, seg - 1, k == n - 1 ? tip : body);
        if (cur) cur_top = base - n * seg;
    }
    if (app->n_running > 0) {
        static const int BOUNCE[6] = {0, 1, 2, 3, 2, 1};
        int up = BOUNCE[now_ms / 140 % 6] * (seg > 2 ? 1 : 0);
        int bx = x0 + (RATE_MINUTES - 1) * cw;
        int cy = cur_top - seg - up;
        if (cy < y) cy = y;
        gfx_rect(c, bx, cy, bw, seg - 1 > 1 ? seg - 2 : 1, (now_ms / 280) % 2 ? WHITE : 0xa6f5bf);
    }
}
