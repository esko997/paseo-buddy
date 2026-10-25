/* The den: a cozy pixel-art room where every visible agent is a little robot at a desk and the
 * pet wanders the rug eating the token crumbs they produce. Everything is gfx_rect/gfx_text
 * level drawing; animation state lives in a fixed particle pool, positions are analytic in age so
 * drawing never mutates the scene. */
#include "scene.h"
#include "pet_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sprites.h"
#pragma GCC diagnostic pop

#define TEXT 0xe8e8ec
#define DIM 0x8a90a0
#define ALERT_COL 0xff6b4a
#define WHITE 0xf4f4f8
#define GOLD 0xffd166

static const uint32_t BUCKET_COL[] = {0xff6b4a, 0xe5484d, 0x46c06f, 0xf5c542, 0x5b6170};

/* ---- layout ------------------------------------------------------------------------------ */

#define DEN_Y 40
#define DEN_B 452
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
#define PET_MIN_X 56
#define PET_MAX_X 464
#define BED_X 548
#define NEST_X 280

#define MAX_PARTICLES 192
#define STALE_MS 4000

typedef enum { P_CRUMB, P_CONFETTI, P_SHELL, P_SPARK, P_SPARKLE, P_HEART, P_RING, P_BIT } PKind;

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

static uint32_t lerp_col(uint32_t a, uint32_t b, int t /* 0..256 */) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255;
    int br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return RGB(ar + (br - ar) * t / 256, ag + (bg - ag) * t / 256, ab + (bb - ab) * t / 256);
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

static const Palette *pet_pal(const Pet *p) {
    if (p->stage == STAGE_ADULT) return family_pal(p->adult_family);
    int64_t fed = p->diet[0] + p->diet[1] + p->diet[2] + p->diet[3];
    return &PALETTES[fed ? (int)pet_favorite(p) : PAL_UNFED];
}

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
        const Palette *pal = pet_pal(&app->pet);
        uint32_t cols[3] = {0xffffff, GOLD, pal->accent};
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
    }
}

/* ---- lifecycle --------------------------------------------------------------------------- */

Scene *scene_create(void) {
    Scene *s = calloc(1, sizeof *s);
    if (s) s->rng = 0x9e3779b9u;
    return s;
}

void scene_destroy(Scene *s) { free(s); }

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
    if (app->pet.stage == STAGE_EGG) {
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
        speed = 95.f;
        s->resting = false;
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
    if (goal < PET_MIN_X && goal != NEST_X) goal = PET_MIN_X;
    if (goal > PET_MAX_X && goal != BED_X) goal = PET_MAX_X;
    float step = speed * dt, dx = goal - s->pet_x;
    if (dx <= step && dx >= -step) s->pet_x = goal;
    else s->pet_x += dx > 0 ? step : -step;

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

typedef struct {
    int minute;
    int day; /* 0 night .. 256 full daylight */
    uint32_t sky_top, sky_bot;
    uint32_t wall, wall_hi, trim, floor, plank, shadow, rug, rug_edge;
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

static Light light_for(const App *app) {
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
    return L;
}

/* ---- background -------------------------------------------------------------------------- */

#define WIN_X 216
#define WIN_Y 48
#define WIN_W 208
#define WIN_H 100

static void draw_window(Canvas *c, const Light *L, int64_t now) {
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
        int64_t st = now % 19000;
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
    /* mullions + sill */
    R(c, WIN_X + WIN_W / 2 - 2, k.y0, 4, ih, 0x5a4232);
    R(c, k.x0, WIN_Y + WIN_H / 2 - 2, iw, 3, 0x5a4232);
    R(c, WIN_X - 6, WIN_Y + WIN_H, WIN_W + 12, 6, 0x6b5040);
    R(c, WIN_X - 6, WIN_Y + WIN_H, WIN_W + 12, 2, 0x8a6a52);
}

static void draw_hud(Canvas *c, const App *app, int64_t now) {
    if (!app->pet_ready) return;
    const Pet *p = &app->pet;
    const Palette *pal = pet_pal(p);
    gfx_text_fit(c, FONT_BIG, 14, 46, TEXT, p->name[0] ? p->name : "Pet", 186);
    char xp[16], line[40];
    format_tokens(p->xp, xp, sizeof xp);
    int w = gfx_text(c, FONT_BODY, 14, 80, pal->light, stage_name(p->stage), 0);
    snprintf(line, sizeof line, " %s tok", xp);
    gfx_text(c, FONT_BODY, 14 + w, 80, DIM, line, 0);

    int64_t next = stage_threshold(p->stage);
    int64_t prev = p->stage == STAGE_EGG ? 0 : stage_threshold((Stage)(p->stage - 1));
    int segs = 12, full = segs, partial = 0;
    if (next > prev) {
        int64_t num = (p->xp - prev) * segs * 4;
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
        if (i < full) col = pal->body;
        else if (i == full && (partial || (now / 400) % 2)) col = lerp_col(0x303648, pal->light, 90 + partial * 40);
        R(c, x, 106, 9, 10, col);
        if (i < full) R(c, x, 106, 9, 2, pal->light);
    }
    /* next stage target beside the bar */
    if (next > prev) format_tokens(next, line, sizeof line);
    else snprintf(line, sizeof line, "max");
    gfx_text_fit(c, FONT_BODY, 22 + segs * 10, 101, 0x6a7080, line, 60);
}

static void draw_rate_panel(Canvas *c, const App *app, int64_t now) {
    int x = 444, y = 46, w = 184, h = 104;
    R(c, x, y, w, h, 0x2a3040);
    R(c, x + 2, y + 2, w - 4, h - 4, 0x12151d);
    gfx_text(c, FONT_BODY, x + 8, y + 4, DIM, "tok/min", 0);
    int64_t lag = app_now(app) / 60 - app->rate_minute;
    char cur[16];
    format_tokens(lag == 0 ? app->rate[RATE_MINUTES - 1] : 0, cur, sizeof cur);
    int cw = (int)strlen(cur) * gfx_font_w(FONT_BODY);
    gfx_text(c, FONT_BODY, x + w - 8 - cw, y + 4, TEXT, cur, 0);
    scene_draw_sparkline(c, app, x + 8, y + 28, w - 16, h - 36, now);
}

static void draw_room(Canvas *c, const Light *L, const App *app, int64_t now) {
    R(c, 0, DEN_Y, SCREEN_W, WALL_B - DEN_Y, L->wall);
    for (int x = 20; x < SCREEN_W; x += 40) R(c, x, DEN_Y, 2, WALL_B - DEN_Y - 8, L->wall_hi);
    R(c, 0, WALL_B - 8, SCREEN_W, 8, L->trim);
    R(c, 0, WALL_B - 8, SCREEN_W, 1, L->wall_hi);
    draw_window(c, L, now);
    draw_hud(c, app, now);
    draw_rate_panel(c, app, now);

    R(c, 0, WALL_B, SCREEN_W, DEN_B - WALL_B, L->floor);
    for (int k = 0, y = WALL_B + 18; y < DEN_B; k++, y += 20) {
        R(c, 0, y, SCREEN_W, 2, L->plank);
        for (int sx = (k * 97) % 160; sx < SCREEN_W; sx += 160) R(c, sx, y - 18, 2, 18, L->plank);
    }
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
    /* plant */
    int sway = isin((int)(now / 90)) / 32;
    R(c, 14, 420, 28, 26, 0xa4553a);
    R(c, 12, 416, 32, 6, 0xc06a4a);
    R(c, 14, 444, 28, 3, L->shadow);
    uint32_t leaf = lerp_col(0x2e6a3a, 0x4caf50, L->day), leaf2 = lerp_col(0x24502c, 0x388e3c, L->day);
    R(c, 26, 384, 4, 32, leaf2);
    R(c, 14 + sway, 388, 12, 6, leaf);
    R(c, 30 + sway, 380, 14, 6, leaf);
    R(c, 18 + sway, 400, 10, 5, leaf2);
    R(c, 30 - sway, 396, 12, 5, leaf);
    R(c, 24 + sway, 372, 8, 10, leaf);
}

/* ---- workers ----------------------------------------------------------------------------- */

static const char *WORKER[12] = {
    "......a.....", ".....oo.....", "..oooooooo..", ".olllbbbbbo.", ".olvvvvvvdo.", ".obvvvvvvdo.",
    ".obvvvvvvdo.", ".obbbbbbbdo.", "..oooooooo..", "...obbbbo...", "..obbbbbbo..", "..obbbbbbo..",
};

typedef enum { EYE_OPEN, EYE_WIDE, EYE_CLOSED, EYE_HAPPY, EYE_X } Eyes;
typedef enum { ARM_TYPE, ARM_REST, ARM_UP, ARM_WAVE } Arms;

#define VISOR 0x14161f

static void draw_worker(Canvas *c, const Palette *pal, int x, int y, Eyes eyes, int look, Arms arms, int tick) {
    for (int r = 0; r < 12; r++) {
        const char *row = WORKER[r];
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
            case 'v': color = VISOR; break;
            default: draw = false;
            }
            if (draw) R(c, x + col * WS, y + r * WS, (run - col) * WS, WS, color);
            col = run;
        }
    }
    uint32_t glow = pal->accent;
    int ey = y + 13;
    for (int e = 0; e < 2; e++) {
        int ex = x + 11 + e * 9;
        switch (eyes) {
        case EYE_OPEN: R(c, ex + 1 + look * 2, ey, 3, 6, glow); break;
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
    if (arms == ARM_UP || arms == ARM_WAVE) {
        for (int side = 0; side < 2; side++) {
            if (arms == ARM_WAVE && side == 0) continue;
            int up = (tick / 2 + side) % 2 ? 3 : 0;
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

static void draw_desk(Canvas *c, const App *app, const Light *L, int i, int n, int64_t now) {
    const Row *row = &app->rows[i];
    const Agent *a = &row->agent;
    int sx, d;
    desk_pos(i, n, &sx, &d);
    uint32_t h = hash_str(a->id, sizeof a->id, (uint32_t)row->bridge * 977u);
    int ph = (int)(h % 1000);
    const Palette *pal = family_pal(a->family);
    int wx = sx + 26, wy = d - 30;
    int mx = sx + 80, my = d - 40;
    int tick = (int)((now + ph) / 110);

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

    /* chair back, worker, desk, keyboard, hands, monitor */
    R(c, wx + 1, d - 16, 34, 18, 0x2a2f40);
    R(c, wx + 1, d - 16, 34, 2, 0x3d4459);
    draw_worker(c, pal, wx, wy + dy, eyes, look, arms, tick);
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
    case ARM_UP: break;
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

void scene_draw_den(const Scene *s, const App *app, Canvas *c, int64_t now_ms) {
    Light L = light_for(app);
    draw_room(c, &L, app, now_ms);
    int n = desk_count(app);
    for (int i = 0; i < n; i++) draw_desk(c, app, &L, i, n, now_ms);
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
