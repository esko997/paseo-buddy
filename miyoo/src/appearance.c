/* Special appearance unlocks: six den wallpapers, six window views, twelve robot costumes and six
 * coat palettes (one of each per Ocean, Woodland, Celestial, Workshop, Candy and Winter set). The
 * den and the wardrobe share these renderers; every rect goes through a Pen clipped to the caller's
 * box, which also fades colours for locked previews. */
#include "appearance.h"

#include <string.h>

#include "theme.h"

/* Theme block t (0..5) holds ids U_DIVER_HAT + 10 t + offset. */
#define THEME_BASE U_DIVER_HAT
#define N_THEMES 6
#define OFF_WALL 4
#define OFF_WINDOW 5
#define OFF_BOT_A 6
#define OFF_BOT_B 7
#define OFF_COAT 8
_Static_assert(U_OCEAN_WALL == THEME_BASE + OFF_WALL && U_FROST_WALL == U_OCEAN_WALL + 50, "wall ids");
_Static_assert(U_REEF_VIEW == THEME_BASE + OFF_WINDOW && U_AURORA_VIEW == U_REEF_VIEW + 50, "window ids");
_Static_assert(U_DIVER_BOT == THEME_BASE + OFF_BOT_A && U_SNOW_BOT == U_SUB_BOT + 50, "robot ids");
_Static_assert(U_LAGOON_COAT == THEME_BASE + OFF_COAT && U_FROST_COAT == U_LAGOON_COAT + 50, "coat ids");

#define SILHOUETTE 0x2e3344 /* locked robots, as pet_view's locked cosmetics */
#define OBSCURED 0x1a1d26   /* locked walls and windows fade toward this */
#define OBSCURE_FADE 200

const char *const APPEARANCE_ROBOT[12] = {
    "......a.....", ".....oo.....", "..oooooooo..", ".olllbbbbbo.", ".olvvvvvvdo.", ".obvvvvvvdo.",
    ".obvvvvvvdo.", ".obbbbbbbdo.", "..oooooooo..", "...obbbbo...", "..obbbbbbo..", "..obbbbbbo..",
};

static int theme_of(int id, int offset) {
    int rel = id - THEME_BASE;
    if (rel < 0 || rel >= N_THEMES * 10 || rel % 10 != offset) return -1;
    return rel / 10;
}

static int robot_of(int id) {
    int t = theme_of(id, OFF_BOT_A);
    if (t >= 0) return t * 2;
    t = theme_of(id, OFF_BOT_B);
    return t >= 0 ? t * 2 + 1 : -1;
}

/* ---- pen: clipped rects, optionally faded toward one colour --------------------------------- */

typedef struct {
    Canvas *c;
    int x0, y0, x1, y1;
    int fade; /* 0 true colours .. 256 everything `to` (a silhouette) */
    uint32_t to;
} Pen;

static Pen pen_box(Canvas *c, int x, int y, int w, int h) {
    Pen p = {c, x, y, x + w, y + h, 0, 0};
    if (p.x0 < 0) p.x0 = 0;
    if (p.y0 < 0) p.y0 = 0;
    if (p.x1 > SCREEN_W) p.x1 = SCREEN_W;
    if (p.y1 > SCREEN_H) p.y1 = SCREEN_H;
    return p;
}

static void fill(const Pen *p, int x, int y, int w, int h, uint32_t col) {
    int x1 = x + w, y1 = y + h;
    if (x < p->x0) x = p->x0;
    if (y < p->y0) y = p->y0;
    if (x1 > p->x1) x1 = p->x1;
    if (y1 > p->y1) y1 = p->y1;
    if (x1 > x && y1 > y) gfx_rect(p->c, x, y, x1 - x, y1 - y, p->fade ? lerp_col(col, p->to, p->fade) : col);
}

static uint32_t hash_u(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

/* sin over a 64-step turn, amplitude 64 (as scene.c). */
static const int8_t QSIN[17] = {0, 6, 12, 19, 24, 30, 36, 41, 45, 50, 53, 56, 59, 61, 63, 64, 64};
static int isin(int a) {
    a &= 63;
    if (a < 16) return QSIN[a];
    if (a < 32) return QSIN[32 - a];
    if (a < 48) return -QSIN[a - 32];
    return -QSIN[64 - a];
}

static int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* Filled disc, one rect per scanline. */
static void disc(const Pen *p, int cx, int cy, int r, uint32_t col) {
    if (r <= 0) {
        fill(p, cx, cy, 1, 1, col);
        return;
    }
    for (int dy = -r; dy <= r; dy++) {
        int hw = isqrt(r * r - dy * dy);
        fill(p, cx - hw, cy + dy, 2 * hw + 1, 1, col);
    }
}

static uint32_t dl(uint32_t night, uint32_t day, int daylight) { return lerp_col(night, day, daylight); }

/* ---- coats ------------------------------------------------------------------------------------ */

static const Palette COATS[N_THEMES] = {
    {0x2fb3c4, 0x8ae6ee, 0x1a7a8e, 0xffe08a}, /* lagoon: aqua with a sandy accent */
    {0x5a7d4a, 0x93b57e, 0x3b5432, 0xc89a5a}, /* moss: sage green, bark accent */
    {0x6a4cc8, 0xb08ef0, 0x3f2a86, 0xff7ad8}, /* nebula: violet, pink accent */
    {0xb4643a, 0xf0b080, 0x6e3418, 0x5ad8c0}, /* copper: with a verdigris accent */
    {0xf59ab8, 0xffd0e0, 0xc86a90, 0xfff08a}, /* sorbet: raspberry pink, lemon accent */
    {0xa8d8f0, 0xe8f8ff, 0x6aa0c8, 0x5a8ae0}, /* frost: ice blue, deep blue accent */
};

const Palette *appearance_coat_palette(int id) {
    int t = theme_of(id, OFF_COAT);
    return t >= 0 ? &COATS[t] : NULL;
}

/* ---- wallpapers --------------------------------------------------------------------------------
 * Low-contrast patterns (the pet's name and stats sit on the wall), dimmed by daylight like the
 * plain wall. s is the pattern scale: 1 in the den, 2 on big previews. */

static void wall_ocean(const Pen *p, int x, int y, int w, int h, int s, int d) {
    uint32_t grout = dl(0x0b1822, 0x142c3a, d), ta = dl(0x14324a, 0x21506a, d), tb = dl(0x112b40, 0x1b4560, d);
    uint32_t hi = dl(0x1a3e58, 0x2a6280, d), wave = dl(0x1d4a62, 0x347a96, d);
    const int t = 18 * s, ts = t - 2 * s;
    fill(p, x, y, w, h, grout);
    for (int j = 0; j * t < h; j++)
        for (int i = 0; i * t < w; i++) {
            int tx = x + i * t + s, ty = y + j * t + s;
            fill(p, tx, ty, ts, ts, (i + j) & 1 ? tb : ta);
            fill(p, tx, ty, ts, s, hi);
            if (hash_u((uint32_t)i * 31u + (uint32_t)j * 977u + 5u) % 3 == 0) /* a little wave */
                for (int k = 0; k < 4; k++) fill(p, tx + (2 + 3 * k) * s, ty + (8 - (k & 1)) * s, 3 * s, s, wave);
        }
}

static void wall_fern(const Pen *p, int x, int y, int w, int h, int s, int d) {
    uint32_t base = dl(0x13211a, 0x203a2a, d), leaf = dl(0x1d3626, 0x30563a, d), leaf2 = dl(0x182e20, 0x29482f, d);
    uint32_t spore = dl(0x1a2c20, 0x2a4632, d);
    const int pitch = 32 * s;
    fill(p, x, y, w, h, base);
    for (int i = 0; i * pitch < w + pitch; i++) {
        int sx = x + i * pitch + 10 * s, off = (i & 1) ? 4 * s : 0;
        fill(p, sx, y, s, h, leaf); /* stem */
        for (int k = 0; off + k * 7 * s < h + 3 * s; k++) {
            int yy = y + off + k * 7 * s + 3 * s;
            uint32_t col = k & 1 ? leaf2 : leaf;
            for (int e = 1; e <= 3; e++) { /* leaflets rising outward on both sides */
                fill(p, sx - 2 * e * s, yy - e * s, 2 * s, s, col);
                fill(p, sx + (2 * e - 1) * s, yy - e * s, 2 * s, s, col);
            }
        }
        for (int k = 0; k * 14 * s < h; k++) fill(p, sx + 16 * s, y + k * 14 * s + ((i & 1) ? 9 : 2) * s, s, s, spore);
    }
}

static void wall_stars(const Pen *p, int x, int y, int w, int h, int s, int d) {
    uint32_t base = dl(0x13142c, 0x20244a, d), star = dl(0x2c3060, 0x4a5290, d), gold = dl(0x3a3628, 0x6e6440, d);
    uint32_t dot = dl(0x1e2042, 0x30366a, d);
    const int cell = 24 * s;
    fill(p, x, y, w, h, base);
    for (int j = 0; j * cell < h; j++)
        for (int i = 0; i * cell < w; i++) {
            uint32_t hh = hash_u((uint32_t)i * 73856093u ^ (uint32_t)j * 19349663u);
            int cx = x + i * cell + 4 * s + (int)(hh % 16) * s, cy = y + j * cell + 4 * s + (int)((hh >> 8) % 16) * s;
            uint32_t col = (hh >> 20) & 1 ? gold : star;
            switch ((hh >> 16) % 4) {
            case 0:
            case 1: /* four-point star */
                fill(p, cx - 2 * s, cy, 5 * s, s, col);
                fill(p, cx, cy - 2 * s, s, 5 * s, col);
                break;
            case 2: /* a pair of dots */
                fill(p, cx, cy, s, s, col);
                fill(p, cx + 5 * s, cy + 3 * s, s, s, dot);
                break;
            default: /* tiny cross */
                fill(p, cx - s, cy, 3 * s, s, dot);
                fill(p, cx, cy - s, s, 3 * s, dot);
            }
        }
}

static void wall_blueprint(const Pen *p, int x, int y, int w, int h, int s, int d) {
    uint32_t base = dl(0x0e223c, 0x173860, d), minor = dl(0x122a48, 0x1d426c, d), major = dl(0x18345a, 0x284f80, d);
    uint32_t ink = dl(0x25446c, 0x41699c, d);
    fill(p, x, y, w, h, base);
    for (int g = 0; g < w; g += 8 * s) fill(p, x + g, y, 1, h, g % (32 * s) ? minor : major);
    for (int g = 0; g < h; g += 8 * s) fill(p, x, y + g, w, 1, g % (32 * s) ? minor : major);
    for (int j = 0; j * 56 * s < h; j++)
        for (int i = 0; i * 96 * s < w; i++) {
            int ox = x + i * 96 * s + 12 * s, oy = y + j * 56 * s + 10 * s;
            if ((i + j) & 1) { /* a gear */
                int cx = ox + 16 * s, cy = oy + 16 * s;
                for (int a = 0; a < 8; a++)
                    fill(p, cx + isin(a * 8 + 16) * 10 * s / 64 - s, cy + isin(a * 8) * 10 * s / 64 - s, 3 * s, 3 * s, ink);
                disc(p, cx, cy, 8 * s, ink);
                disc(p, cx, cy, 6 * s, base);
                disc(p, cx, cy, 2 * s, ink);
            } else { /* a part with a hole and a dimension line */
                int pw = 40 * s, ph = 24 * s;
                fill(p, ox, oy, pw, 1, ink);
                fill(p, ox, oy + ph - 1, pw, 1, ink);
                fill(p, ox, oy, 1, ph, ink);
                fill(p, ox + pw - 1, oy, 1, ph, ink);
                disc(p, ox + 28 * s, oy + 12 * s, 5 * s, ink);
                disc(p, ox + 28 * s, oy + 12 * s, 5 * s - 1, base);
                fill(p, ox + 6 * s, oy + 8 * s, 12 * s, 8 * s, minor);
                fill(p, ox, oy + ph + 6 * s, pw, 1, ink);
                fill(p, ox, oy + ph + 4 * s, 1, 5 * s, ink);
                fill(p, ox + pw - 1, oy + ph + 4 * s, 1, 5 * s, ink);
            }
        }
}

static void wall_candy(const Pen *p, int x, int y, int w, int h, int s, int d) {
    uint32_t a = dl(0x3a1e2e, 0x5e2e48, d), b = dl(0x2c222c, 0x463640, d), mint = dl(0x203634, 0x355856, d);
    const int period = 24 * s, step = 2 * s;
    fill(p, x, y, w, h, b);
    for (int ry = 0; ry < h; ry += step) { /* diagonal stripes, one stair step per 2s rows */
        int shift = ry % period;
        for (int sx = x - shift; sx < x + w; sx += period) {
            fill(p, sx, y + ry, 10 * s, step, a);
            fill(p, sx + 10 * s, y + ry, 2 * s, step, mint);
        }
    }
}

static void flake(const Pen *p, int cx, int cy, int s, uint32_t arm, uint32_t core) {
    fill(p, cx, cy - 6 * s, s, 13 * s, arm);
    fill(p, cx - 6 * s, cy, 13 * s, s, arm);
    for (int k = 1; k <= 4; k++) {
        fill(p, cx - k * s, cy - k * s, s, s, arm);
        fill(p, cx + k * s, cy - k * s, s, s, arm);
        fill(p, cx - k * s, cy + k * s, s, s, arm);
        fill(p, cx + k * s, cy + k * s, s, s, arm);
    }
    fill(p, cx - s, cy - 4 * s, 3 * s, s, arm); /* branch ticks */
    fill(p, cx - s, cy + 4 * s, 3 * s, s, arm);
    fill(p, cx - 4 * s, cy - s, s, 3 * s, arm);
    fill(p, cx + 4 * s, cy - s, s, 3 * s, arm);
    fill(p, cx - s, cy - s, 3 * s, 3 * s, core);
}

static void wall_frost(const Pen *p, int x, int y, int w, int h, int s, int d) {
    uint32_t base = dl(0x13202e, 0x203448, d), arm = dl(0x24384e, 0x3e5c7e, d), core = dl(0x30475f, 0x5a7a9e, d);
    uint32_t dust = dl(0x1c2c3e, 0x2e4460, d);
    const int cw = 40 * s, ch = 36 * s;
    fill(p, x, y, w, h, base);
    for (int j = 0; j * ch < h + ch / 2; j++)
        for (int i = -1; i * cw < w + cw; i++) {
            int cx = x + i * cw + (j & 1 ? cw / 2 : 0) + 10 * s, cy = y + j * ch + 10 * s;
            flake(p, cx, cy, s, arm, core);
            uint32_t hh = hash_u((uint32_t)(i + 1) * 2654435761u ^ (uint32_t)j * 40503u);
            for (int k = 0; k < 3; k++, hh >>= 9) /* frost sparkle */
                fill(p, cx + 9 * s + (int)(hh % 12) * s, cy + 8 * s + (int)((hh >> 4) % 14) * s, s, s, dust);
        }
}

typedef void (*WallFn)(const Pen *, int, int, int, int, int, int);
static const WallFn WALLS[N_THEMES] = {wall_ocean, wall_fern, wall_stars, wall_blueprint, wall_candy, wall_frost};

void appearance_draw_wall(Canvas *c, int id, int x, int y, int w, int h, int daylight) {
    int t = theme_of(id, OFF_WALL);
    if (t < 0) return;
    Pen p = pen_box(c, x, y, w, h);
    WALLS[t](&p, x, y, w, h, 1, daylight);
}

/* ---- window views --------------------------------------------------------------------------------
 * Drawn in a 196x88 design space (the den's pane); other boxes scale it to cover and crop. */

#define VW 196
#define VH 88

typedef struct {
    Pen p;
    int cx, cy, k; /* design (VW/2, VH/2) lands on (cx, cy); k: pixels per design unit, x256 */
} View;

static View view_box(Canvas *c, int x, int y, int w, int h) {
    View v = {pen_box(c, x, y, w, h), x + w / 2, y + h / 2, 256};
    int kx = w * 256 / VW, ky = h * 256 / VH;
    v.k = kx > ky ? kx : ky;
    if (v.k < 1) v.k = 1;
    return v;
}

static int fdiv256(int a) { return a >= 0 ? a / 256 : -((255 - a) / 256); }
static int vx(const View *v, int dx) { return v->cx + fdiv256((dx - VW / 2) * v->k); }
static int vy(const View *v, int dy) { return v->cy + fdiv256((dy - VH / 2) * v->k); }

static void vrect(const View *v, int dx, int dy, int dw, int dh, uint32_t col) {
    if (dw <= 0 || dh <= 0) return;
    int x0 = vx(v, dx), x1 = vx(v, dx + dw), y0 = vy(v, dy), y1 = vy(v, dy + dh);
    if (x1 <= x0) x1 = x0 + 1; /* thin details survive small previews */
    if (y1 <= y0) y1 = y0 + 1;
    fill(&v->p, x0, y0, x1 - x0, y1 - y0, col);
}

static void vdisc(const View *v, int dx, int dy, int r, uint32_t col) { disc(&v->p, vx(v, dx), vy(v, dy), r * v->k / 256, col); }

/* Landscape colour at night: two thirds of the way to deep blue. */
static uint32_t lit(uint32_t col, int d) { return lerp_col(lerp_col(col, 0x0a1020, 170), col, d); }

static uint32_t grad(uint32_t top, uint32_t bot, int dy, int bottom) {
    int b = dy * 8 / bottom;
    return lerp_col(top, bot, (b < 0 ? 0 : b > 7 ? 7 : b) * 256 / 7);
}

/* Eight-band gradient from 0 down to `bottom`. */
static void bands(const View *v, uint32_t top, uint32_t bot, int bottom) {
    for (int i = 0; i < 8; i++) {
        int y0 = i * bottom / 8, y1 = (i + 1) * bottom / 8;
        vrect(v, 0, y0, VW, y1 - y0, lerp_col(top, bot, i * 256 / 7));
    }
}

/* Twinkling stars above ymax, gone by day. */
static void stars(const View *v, int n, uint32_t seed, int ymax, int d, uint32_t top, uint32_t bot, int64_t t) {
    if (d >= 160) return;
    int fade = d * 256 / 160;
    for (int i = 0; i < n; i++) {
        uint32_t h = hash_u((uint32_t)i * 7919u + seed);
        int sx = (int)(h % VW), sy = (int)((h >> 10) % (uint32_t)ymax);
        int ph = (int)((t / 170 + (h >> 20)) % 24);
        uint32_t col = ph < 2 ? 0xfffbe8 : (h >> 8) & 1 ? 0xb0b8e0 : 0x8088b8;
        vrect(v, sx, sy, 1, 1, lerp_col(col, grad(top, bot, sy, ymax), fade));
    }
}

static void sun_moon(const View *v, int dx, int dy, int d, uint32_t sky) {
    if (d >= 128) {
        vdisc(v, dx, dy, 7, 0xffc93a);
        vdisc(v, dx - 1, dy - 1, 5, 0xffe27a);
    } else {
        vdisc(v, dx, dy, 6, 0xf2eccf);
        vdisc(v, dx + 4, dy - 2, 5, sky);
    }
}

/* Stacked-tier pine: tip at base - height. */
static void pine(const View *v, int cx, int base, int height, uint32_t col, uint32_t trunk) {
    int top = base - height;
    for (int i = 0; i < height - 4; i += 2) {
        int hw = 1 + i / 3 - ((i / 2) % 4 == 3 ? 2 : 0);
        if (hw < 1) hw = 1;
        vrect(v, cx - hw, top + i, 2 * hw + 1, 2, col);
    }
    vrect(v, cx - 1, base - 4, 3, 4, trunk);
}

static void win_reef(const View *v, int d, int64_t t) {
    uint32_t top = lerp_col(0x0a2a48, 0x3ab8d8, d), bot = lerp_col(0x041428, 0x0e5a8a, d);
    bands(v, top, bot, VH);
    if (d > 96) { /* light shafts */
        uint32_t ray = lerp_col(top, 0xffffff, (d - 96) * 48 / 160);
        int sway = isin((int)(t / 60)) * 4 / 64;
        for (int r = 0; r < 3; r++)
            for (int dy = 0; dy < 64; dy += 4) vrect(v, 26 + r * 62 + sway + dy / 3, dy, 7 - dy / 12, 4, ray);
    }
    uint32_t weed = lit(0x3a9a5a, d), weed2 = lit(0x2a7a48, d);
    for (int k = 0; k < 3; k++) { /* swaying kelp */
        int bx = k == 0 ? 10 : k == 1 ? 104 : 186;
        for (int g = 0; g < 8 - k; g++) {
            int sw = isin((int)(t / 45) + g * 6 + k * 20) * g / 128;
            vrect(v, bx + sw, 76 - (g + 1) * 5, 3, 5, g & 1 ? weed2 : weed);
        }
    }
    uint32_t pink = lit(0xff7a8a, d), pink2 = lit(0xd85a70, d); /* staghorn */
    vrect(v, 34, 52, 4, 24, pink);
    vrect(v, 26, 60, 8, 3, pink);
    vrect(v, 26, 48, 3, 12, pink);
    vrect(v, 38, 58, 8, 3, pink2);
    vrect(v, 43, 44, 3, 14, pink2);
    vrect(v, 34, 42, 4, 10, pink);
    vrect(v, 30, 40, 2, 6, pink2);
    uint32_t tube = lit(0xf2d04a, d), tube2 = lit(0x8a6a20, d); /* tube coral */
    static const int16_t TUBES[3][2] = {{126, 18}, {133, 26}, {140, 14}};
    for (int i = 0; i < 3; i++) {
        vrect(v, TUBES[i][0], 76 - TUBES[i][1], 6, TUBES[i][1], tube);
        vrect(v, TUBES[i][0] + 1, 76 - TUBES[i][1], 4, 2, tube2);
    }
    uint32_t fan = lit(0xa060d0, d), fan2 = lit(0x7a44a8, d); /* sea fan */
    for (int r = 0; r < 22; r++) {
        int hw = (22 - r) * 10 / 22 + 1;
        vrect(v, 168 - hw, 48 + r, 2 * hw, 1, (r / 2) & 1 ? fan2 : fan);
    }
    vrect(v, 167, 70, 2, 6, fan2);
    uint32_t brain = lit(0xf0a050, d), groove = lit(0xc87a38, d); /* brain coral, sunk in the sand */
    vdisc(v, 76, 77, 12, brain);
    for (int g = 0; g < 3; g++) vrect(v, 68 + g * 2, 69 + g * 3, 16 - g * 4, 1, groove);
    uint32_t sand = lit(0xd8c08a, d), sand2 = lit(0xb89c68, d);
    vrect(v, 0, 76, VW, VH - 76, sand);
    for (int i = 0; i < 7; i++) vrect(v, 8 + i * 28, 79 + (i & 1) * 4, 10, 1, sand2);
    for (int i = 0; i < 5; i++) { /* bubbles */
        int pp = (int)((t / 35 + i * 37) % 80);
        vrect(v, 36 + i * 30 + isin(pp + i * 9) * 2 / 64, 74 - pp, 2 + (i & 1), 2 + (i & 1), lerp_col(top, 0xffffff, 120));
    }
    int64_t per = 9000; /* clownfish one way, a little blue tang the other */
    int fx = (int)(t % per * (VW + 40) / per) - 20, fy = 28 + isin((int)(t / 90)) * 4 / 64;
    uint32_t orange = lit(0xff8a2a, d), white = lit(0xf8f4f0, d);
    vrect(v, fx - 3, fy - 1, 3, 2, orange);
    vrect(v, fx - 3, fy + 4, 3, 2, orange);
    vrect(v, fx, fy, 9, 5, orange);
    vrect(v, fx + 1, fy - 1, 6, 7, orange);
    vrect(v, fx + 4, fy - 1, 1, 7, white);
    vrect(v, fx + 7, fy + 1, 1, 1, 0x101018);
    int gx = VW + 20 - (int)((t + 4000) % 12000 * (VW + 40) / 12000), gy = 52 + isin((int)(t / 120) + 20) * 3 / 64;
    uint32_t blue = lit(0x3a6af0, d);
    vrect(v, gx, gy, 6, 3, blue);
    vrect(v, gx + 6, gy - 1, 2, 5, lit(0xf0d040, d));
    vrect(v, gx + 1, gy, 1, 1, 0x101018);
    if (d < 100) /* night: bioluminescent specks on the reef */
        for (int i = 0; i < 6; i++) {
            uint32_t h = hash_u((uint32_t)i * 4099u + 3u);
            int pulse = (int)((t / 200 + i * 7) % 16);
            vrect(v, 20 + (int)(h % 160), 44 + (int)((h >> 8) % 30), 1, 1,
                  lerp_col(0x40e0c0, bot, pulse < 8 ? pulse * 30 : (16 - pulse) * 30));
        }
}

static void win_forest(const View *v, int d, int64_t t) {
    uint32_t top = lerp_col(0x0a1430, 0x6aaee0, d), bot = lerp_col(0x1c2a4a, 0xcfe8f4, d);
    bands(v, top, bot, 60);
    stars(v, 14, 11u, 40, d, top, bot, t);
    sun_moon(v, 150, 16, d, top);
    vdisc(v, 40, 84, 36, lit(0x5a8a90, d)); /* hazy hills */
    vdisc(v, 150, 90, 44, lit(0x4e7e88, d));
    uint32_t far = lit(0x3a6a5a, d), trunk = lit(0x4a3222, d);
    for (int i = 0; i < 12; i++) {
        uint32_t h = hash_u((uint32_t)i * 613u + 9u);
        pine(v, 6 + i * 16 + (int)(h % 6), 62, 14 + (int)((h >> 8) % 8), far, far);
    }
    uint32_t grass = lit(0x4a8a3a, d), grass2 = lit(0x3a7230, d), path = lit(0xc8a870, d), pebble = lit(0xa88a58, d);
    vrect(v, 0, 60, VW, VH - 60, grass);
    for (int i = 0; i < 10; i++) vrect(v, 6 + i * 19, 66 + (i * 7) % 18, 3, 2, grass2);
    for (int r = 0; r < VH - 60; r++) { /* the trail winds toward the horizon */
        int hw = 2 + r * 20 / 28;
        int c0 = 98 + isin(r * 2 + 8) * (28 - r) / 128;
        vrect(v, c0 - hw, 60 + r, 2 * hw, 1, path);
    }
    vrect(v, 92, 70, 2, 1, pebble);
    vrect(v, 104, 78, 3, 1, pebble);
    vrect(v, 88, 84, 2, 1, pebble);
    uint32_t near = lit(0x1e4a2a, d), near2 = lit(0x285a32, d);
    pine(v, 14, 84, 62, near, trunk);
    pine(v, 38, 88, 44, near2, trunk);
    pine(v, 180, 84, 64, near, trunk);
    pine(v, 158, 88, 40, near2, trunk);
    if (d < 110) { /* fireflies */
        for (int i = 0; i < 7; i++) {
            uint32_t h = hash_u((uint32_t)i * 977u + 41u);
            if ((t / 150 + (h >> 16)) % 20 >= 12) continue;
            int fx = 24 + (int)(h % 150) + isin((int)(t / 80) + i * 9) * 6 / 64;
            int fy = 48 + (int)((h >> 8) % 30) + isin((int)(t / 110) + i * 13) * 4 / 64;
            vrect(v, fx, fy, 1, 1, 0xe8f070);
        }
    } else { /* a leaf drifting down */
        int pp = (int)(t % 7000);
        vrect(v, 60 + pp * 60 / 7000 + isin(pp / 60) * 6 / 64, -4 + pp * 80 / 7000, 3, 2, 0xe0a040);
    }
}

static void win_nebula(const View *v, int d, int64_t t) {
    uint32_t top = lerp_col(0x06040f, 0x100a24, d / 3), bot = lerp_col(0x120a28, 0x1c1040, d / 3);
    bands(v, top, bot, VH);
    int drift = isin((int)(t / 400)) * 2 / 64;
    static const struct {
        int16_t x, y, r;
        uint32_t col;
    } CLOUD[] = {
        {40, 30, 22, 0x2a1442}, {70, 44, 18, 0x3a1a5a}, {58, 30, 12, 0x5a2a7a}, {104, 40, 20, 0x1a2a5a},
        {124, 52, 16, 0x1a4a5a}, {96, 34, 10, 0x6a2a7a}, {118, 46, 8, 0x2a6a8a}, {62, 36, 5, 0x9a4aa0},
        {110, 40, 4, 0x6a9ad8},
    };
    for (unsigned i = 0; i < sizeof CLOUD / sizeof CLOUD[0]; i++)
        vdisc(v, CLOUD[i].x + (i & 1 ? drift : -drift), CLOUD[i].y, CLOUD[i].r, lerp_col(top, CLOUD[i].col, 200 + d / 8));
    for (int i = 0; i < 40; i++) {
        uint32_t h = hash_u((uint32_t)i * 2246822519u + 77u);
        int sx = (int)(h % VW), sy = (int)((h >> 10) % VH), ph = (int)((t / 140 + (h >> 20)) % 28);
        bool big = i < 5;
        uint32_t col = ph < 3 ? 0xffffff : (h >> 8) & 1 ? 0xc8d0ff : 0x8890c8;
        vrect(v, sx, sy, 1, 1, col);
        if (big && ph < 14) { /* spikes on the bright ones */
            vrect(v, sx - 2, sy, 2, 1, 0x8890c8);
            vrect(v, sx + 1, sy, 2, 1, 0x8890c8);
            vrect(v, sx, sy - 2, 1, 2, 0x8890c8);
            vrect(v, sx, sy + 1, 1, 2, 0x8890c8);
        }
    }
    const int px = 156, py = 62, rx = 19, ry = 4; /* ringed planet: back of the ring, planet, front */
    uint32_t ring = 0xd8c8a0;
    for (int dx = -rx; dx <= rx; dx++) vrect(v, px + dx, py - isqrt((rx * rx - dx * dx) * ry * ry / (rx * rx)), 1, 1, ring);
    vdisc(v, px, py, 10, 0xd8a070);
    vrect(v, px - 9, py - 4, 18, 2, 0xb8804e);
    vrect(v, px - 10, py + 1, 20, 2, 0xe8c090);
    vrect(v, px - 8, py + 5, 16, 1, 0xb8804e);
    vrect(v, px - 5, py - 7, 3, 2, 0xf8e0b8);
    for (int dx = -rx; dx <= rx; dx++) vrect(v, px + dx, py + isqrt((rx * rx - dx * dx) * ry * ry / (rx * rx)), 1, 1, ring);
    int pp = (int)(t % 16000); /* a comet now and then */
    if (pp < 4000) {
        int hx = -20 + pp * (VW + 40) / 4000, hy = 8 + pp * 20 / 4000;
        for (int j = 5; j > 0; j--) vrect(v, hx - j * 4, hy - j, 4, 1, lerp_col(0xa8e0ff, top, j * 40));
        vrect(v, hx, hy - 1, 2, 2, 0xffffff);
    }
}

static void win_city(const View *v, int d, int64_t t) {
    uint32_t top = lerp_col(0x0a0e28, 0x6aaee8, d), bot = lerp_col(0x221e4a, 0xbcdcf8, d);
    bands(v, top, bot, 82);
    stars(v, 12, 23u, 36, d, top, bot, t);
    sun_moon(v, 36, 16, d, top);
    uint32_t haze = lerp_col(0x1a2040, 0x8a9ab8, d);
    for (int x = 0, i = 0; x < VW; i++) { /* far skyline */
        uint32_t h = hash_u((uint32_t)i * 131u + 7u);
        int bw = 10 + (int)(h % 8), bh = 20 + (int)((h >> 8) % 20);
        vrect(v, x, 82 - bh - 8, bw, bh + 8, haze);
        x += bw;
    }
    uint32_t lamp = 0xffd870, off = 0x1a1e2a, glass = lerp_col(0x2a3448, 0x9ab8d8, d);
    int tallest_x = 0, tallest_top = VH;
    for (int x = 0, i = 0; x < VW; i++) {
        uint32_t h = hash_u((uint32_t)i * 977u + 13u);
        int bw = 14 + (int)(h % 13), bh = 26 + (int)((h >> 8) % 37), btop = 82 - bh;
        vrect(v, x, btop, bw, bh, lit(i & 1 ? 0x4a4a5e : 0x3a4258, d));
        vrect(v, x, btop, bw, 1, lit(0x5a6078, d));
        for (int wy = btop + 4; wy < 78; wy += 6)
            for (int wx = x + 3; wx + 2 < x + bw - 1; wx += 5) {
                uint32_t wh = hash_u(h ^ (uint32_t)(wx * 31 + wy * 977));
                uint32_t col = glass;
                if (d < 140) {
                    bool on = hash_u(wh ^ (uint32_t)((t + (wh & 8191)) / 9000)) % 3 == 0;
                    col = lerp_col(on ? lamp : off, glass, d * 256 / 140);
                } else if (wh % 5 == 0) {
                    col = lit(0x6a88a8, d);
                }
                vrect(v, wx, wy, 2, 3, col);
            }
        if (btop < tallest_top) tallest_top = btop, tallest_x = x + bw / 2;
        x += bw + 1 + (int)((h >> 20) % 3);
    }
    vrect(v, tallest_x, tallest_top - 10, 1, 10, lit(0x6a7088, d)); /* antenna with its blinking light */
    if ((t / 700) % 2) vrect(v, tallest_x - 1, tallest_top - 12, 3, 2, 0xff4040);
    vrect(v, 0, 82, VW, VH - 82, lit(0x2a2c34, d));
    for (int x = 4; x < VW; x += 16) vrect(v, x, 85, 7, 1, lit(0x8a8a70, d));
    int pp = (int)(t % 6000), cx = pp * (VW + 20) / 6000 - 10;
    if (d < 140) {
        vrect(v, cx + 6, 83, 3, 1, 0xfff0b0);
        vrect(v, cx, 83, 2, 1, 0xff4a4a);
    } else {
        vrect(v, cx, 82, 9, 3, 0xd04a3a);
        vrect(v, cx + 2, 81, 5, 1, 0x9ab8d8);
    }
}

static void lollipop(const View *v, int x, int y, uint32_t col, int d) {
    vrect(v, x, y, 2, 82 - y, lit(0xf4f0f0, d));
    vdisc(v, x + 1, y - 4, 6, lit(col, d));
    vdisc(v, x + 1, y - 4, 4, lit(0xfff4f8, d));
    vdisc(v, x + 1, y - 4, 2, lit(col, d));
}

static void win_candy(const View *v, int d, int64_t t) {
    uint32_t top = lerp_col(0x2a1a48, 0xffc0e0, d), bot = lerp_col(0x4a2a5a, 0xfff0d8, d);
    bands(v, top, bot, 70);
    stars(v, 14, 37u, 44, d, top, bot, t);
    if (d >= 128) { /* a candy sun */
        for (int a = 0; a < 8; a++) vrect(v, 40 + isin(a * 8 + 16) * 12 / 64 - 1, 18 + isin(a * 8) * 12 / 64 - 1, 3, 3, 0xff8ab8);
        vdisc(v, 40, 18, 8, 0xffd84a);
        vdisc(v, 40, 18, 4, 0xfff0a0);
    } else {
        sun_moon(v, 40, 18, d, top);
    }
    for (int i = 0; i < 2; i++) { /* cotton-candy clouds */
        int x = (int)((t / 120 + i * 110) % (VW + 50)) - 25, y = 14 + i * 14;
        uint32_t cc = lit(i ? 0xa8d8ff : 0xffb8e0, d);
        vdisc(v, x, y, 5, cc);
        vdisc(v, x + 7, y - 2, 6, cc);
        vdisc(v, x + 14, y, 4, cc);
    }
    vdisc(v, 40, 104, 50, lit(0x9ae8c8, d));
    vdisc(v, 152, 108, 56, lit(0xf8a8c8, d));
    vdisc(v, 98, 118, 46, lit(0xc8b0f0, d));
    for (int i = 0; i < 18; i++) { /* sprinkles */
        uint32_t h = hash_u((uint32_t)i * 5381u + 3u);
        static const uint32_t SPR[4] = {0xff5a8a, 0xffe04a, 0x5ac8fa, 0xffffff};
        vrect(v, (int)(h % VW), 66 + (int)((h >> 8) % 20), 2, 1, lit(SPR[(h >> 16) & 3], d));
    }
    lollipop(v, 24, 62, 0xff5a8a, d);
    lollipop(v, 128, 68, 0x5ac8fa, d);
    uint32_t cane = lit(0xf4f0f0, d), red = lit(0xe5484d, d); /* candy cane */
    vrect(v, 172, 58, 2, 24, cane);
    vrect(v, 172, 56, 7, 2, cane);
    vrect(v, 177, 58, 2, 4, cane);
    for (int y = 60; y < 82; y += 4) vrect(v, 172, y, 2, 2, red);
    vrect(v, 175, 56, 2, 2, red);
    static const uint32_t GUM[3] = {0x46c06f, 0xff9f43, 0xc58af9}; /* gumdrops */
    for (int i = 0; i < 3; i++) {
        vdisc(v, 70 + i * 16, 84, 4, lit(GUM[i], d));
        vrect(v, 69 + i * 16, 81, 2, 1, lit(0xffffff, d));
    }
}

static void win_aurora(const View *v, int d, int64_t t) {
    uint32_t top = lerp_col(0x04121e, 0x8abce0, d), bot = lerp_col(0x0c2a3a, 0xdaeef8, d);
    bands(v, top, bot, 64);
    stars(v, 20, 53u, 44, d, top, bot, t);
    int a = d < 170 ? (170 - d) * 230 / 170 : 0;
    if (a > 0) { /* aurora ribbons, waving slowly */
        static const uint32_t RIB[3] = {0x46e0a0, 0x3ac8d8, 0xa070e0};
        for (int k = 0; k < 3; k++)
            for (int dx = 0; dx < VW; dx += 2) {
                int yc = 8 + k * 8 + isin(dx / 2 + (int)(t / 50) + k * 21) * 6 / 64 + isin(dx / 6 + k * 11) * 4 / 64;
                int len = 10 - k * 2 + isin(dx + (int)(t / 70)) * 3 / 64;
                uint32_t under = grad(top, bot, yc, 64);
                vrect(v, dx, yc, 2, 2, lerp_col(under, 0xe0fff0, a * 3 / 4));
                vrect(v, dx, yc + 2, 2, len, lerp_col(under, RIB[k], a));
            }
    }
    uint32_t rock = lit(0x8aa0c0, d), cap = lit(0xf4f8ff, d);
    static const int16_t PEAK[3][3] = {{40, 30, 26}, {96, 24, 32}, {156, 32, 24}}; /* x, height, half-width */
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < PEAK[i][1]; r++) {
            int hw = r * PEAK[i][2] / PEAK[i][1];
            vrect(v, PEAK[i][0] - hw, 70 - PEAK[i][1] + r, 2 * hw + 1, 1, r < PEAK[i][1] / 3 ? cap : rock);
        }
    uint32_t snow = lit(0xe8f0f8, d), shade = lit(0xb8cce0, d);
    vrect(v, 0, 66, VW, VH - 66, snow);
    vdisc(v, 30, 74, 14, snow);
    vdisc(v, 130, 76, 18, snow);
    vrect(v, 0, 84, VW, 1, shade);
    uint32_t tree = lit(0x1e3a3a, d);
    pine(v, 12, 80, 30, tree, tree);
    pine(v, 184, 82, 36, tree, tree);
    pine(v, 170, 84, 24, tree, tree);
    vrect(v, 58, 64, 22, 14, lit(0x6a4228, d)); /* cabin */
    for (int r = 0; r < 7; r++) vrect(v, 56 + r, 57 + r, 26 - 2 * r, 1, cap);
    vrect(v, 64, 68, 6, 5, d < 140 ? lerp_col(0xffd870, 0x3a2a1a, d * 256 / 140) : 0x3a2a1a);
    vrect(v, 74, 50, 3, 8, lit(0x4a3a3a, d));
    for (int i = 0; i < 14; i++) { /* snowfall */
        uint32_t h = hash_u((uint32_t)i * 7331u + 17u);
        int fx = (int)(h % VW) + isin((int)(t / 90) + i * 5) * 3 / 64;
        int fy = (int)((t / 40 + (h >> 8)) % 96) - 4;
        vrect(v, fx, fy, 1 + (i & 1), 1 + (i & 1), 0xf4f8ff);
    }
}

typedef void (*WindowFn)(const View *, int, int64_t);
static const WindowFn WINDOWS[N_THEMES] = {win_reef, win_forest, win_nebula, win_city, win_candy, win_aurora};

static void window_into(View *v, int t, int daylight, int64_t now_ms) { WINDOWS[t](v, daylight, now_ms); }

void appearance_draw_window(Canvas *c, int id, int x, int y, int w, int h, int daylight, int64_t now_ms) {
    int t = theme_of(id, OFF_WINDOW);
    if (t < 0) return;
    View v = view_box(c, x, y, w, h);
    window_into(&v, t, daylight, now_ms);
}

/* ---- robot costumes ------------------------------------------------------------------------------
 * Cell art over the 12x12 robot: grid row 0 is robot row -4, grid column `ox` is robot column 0.
 * 'o' outline, '1'..'5' inks. Cells on the visor (robot rows 4..6, columns 3..8) are never drawn, so
 * the eyes always read; the family body stays visible around each costume. */

#define COSTUME_ROWS 16
#define COSTUME_OY 4

typedef struct {
    int ox;
    const char *art[COSTUME_ROWS];
    uint32_t ink[5];
} Costume;

static const Costume COSTUMES[N_THEMES * 2] = {
    /* Diver bot (Claude): brass dome with a porthole, air hose, weighted collar */
    {2,
     {"................", "......oooo......", ".....o2221o.....", "....o21oo11o....", "....o2o33o1o44..",
      "....o1o33o1o..4.", "...o21111112o.4.", "..............4.", "..............4.", "..............4.",
      "..............4.", "..............4.", ".............44.", "....o122221o44..", "................",
      "................"},
     {0xc8913a, 0xf0c870, 0x7ad6e8, 0x5a6470, 0}},
    /* Submarine bot (GPT): periscope, conning tower, riveted hull band, propeller */
    {2,
     {"........oooooo..", "........o3332o..", "........o3oooo..", "........o3o.....", ".....oooo3oo....",
      ".....o11211o....", "...o11111111o...", ".4..............", ".4..............", "o33.............",
      ".4..............", ".4.o13111131o...", "................", ".....o2112o.....", "................",
      "................"},
     {0xf2c230, 0xffe27a, 0x6a7080, 0xb8c0d0, 0}},
    /* Ranger bot (Gemini): wide-brimmed campaign hat with a badge, neckerchief */
    {2,
     {"................", ".......oo.......", "......o21o......", ".....o2112o.....", ".....o1111o.....",
      ".....o3553o.....", ".o222222222222o.", "..oooooooooooo..", "................", "................",
      "................", "................", "................", "....o444444o....", "......o44o......",
      ".......oo......."},
     {0xc8a060, 0xe8c88a, 0x6a4222, 0x3f8a4a, 0xffd166}},
    /* Owl bot (other): ear tufts, brow, folded wings, beak, chest feathers */
    {2,
     {"................", "................", "..o..........o..", "..o1o......o1o..", "...o21o..o12o...",
      "....o21oo12o....", "...o11111111o...", "...o4o....o4o...", "..o..........o..", ".o1..........1o.",
      "o21..........12o", "o12...o33o...21o", ".o2....oo....2o.", "..o....44....o..", "................",
      "................"},
     {0x8a5a32, 0xc8955a, 0xf0a030, 0xf0dcb0, 0}},
    /* Astronaut bot (Claude): glass bubble helmet, suit collar and chest */
    {2,
     {"................", ".....333333.....", "...33......43...", "..3..........3..", ".3............3.",
      ".3..........4.3.", ".3............3.", ".3............3.", ".3............3.", ".3............3.",
      ".3............3.", ".3............3.", ".3............3.", "..o2111111112o..", "...o15111151o...",
      "...o11111111o..."},
     {0xe8ecf4, 0x9aa0b0, 0x8fd8f0, 0xffffff, 0xe5484d}},
    /* Satellite bot (Gemini): dish antenna, solar panel wings, gold foil */
    {4,
     {".........55.........", "....o4o..33..o4o....", ".....o44.33.44o.....", "......o443344o......",
      ".......oooooo.......", ".........33.........", "....................", "oooo............oooo",
      "o12o............o21o", "o21o3..........3o12o", "o12o............o21o", "oooo............oooo",
      "....................", ".......o5555o.......", "......o555555o......", "......o555555o......"},
     {0x2a5ab8, 0x8fb0ff, 0xa0a8b8, 0xe8ecf4, 0xe0b040}},
    /* Mechanic bot (GPT): flat cap with a bill, goggles up on the forehead, overalls */
    {2,
     {"................", "................", "................", ".....oooooo.....", "....o112111o....",
      "...o11121111o...", "...o11111111222o", "...3o44oo44o3ooo", "................", "................",
      "................", "................", "................", ".....o5..5o.....", "....o553355o....",
      "....o555555o...."},
     {0x2e4a7a, 0x4a6aa8, 0x5a3a2a, 0xb8e0f0, 0xe8823a}},
    /* Clockwork bot (other): brass gear on top, wind-up key, chin rivets, copper plate */
    {4,
     {"........3..3........", ".......311113.......", "......31122113......", ".....3312oo2133.....",
      "......31122113......", ".......311113.......", "....................", ".................111",
      ".................1.1", "...............33111", ".................1.1", ".......2.22.2....111",
      "....................", ".......o4444o.......", "......o434434o......", "......o444444o......"},
     {0xc8962e, 0xf0cf72, 0x7a5418, 0xc0643a, 0}},
    /* Pastry chef (Claude): puffy toque, red neckerchief */
    {2,
     {"...oo..oo..oo...", "..o11oo11oo11o..", "..o1111111111o..", "..o1121111211o..", "...o11111111o...",
      "...o12121212o...", "...oooooooooo...", "................", "................", "................",
      "................", "................", "................", "....o333333o....", "......o33o......",
      "................"},
     {0xf6f4f0, 0xc8c4d0, 0xd8403a, 0, 0}},
    /* Soda bot (other): crimped bottle cap, bendy straw, fizz */
    {2,
     {"..........3434..", "..........4.....", "..........3.....", "..........4.....", "...1.1.11.1.1...",
      "...o11222211o...", "...o11111111o...", "................", ".5..............", "..............5.",
      "5...............", ".............5..", "................", ".....o1221o.....", "................",
      "................"},
     {0xe5484d, 0xff9a9a, 0xf4f4f8, 0xff6b8a, 0xa8e0ff}},
    /* Penguin bot (GPT): black hood, flippers, beak, white bib, orange feet */
    {2,
     {"................", "................", "................", ".....oooooo.....", "....o112211o....",
      "...o11111111o...", "...o11111111o...", "...o1o....o1o...", "................", "..o..........o..",
      ".o1..........1o.", ".o1....44....1o.", "..o1........1o..", ".......33.......", "................",
      "....o44oo44o...."},
     {0x22242e, 0x4a4e60, 0xf0f2f8, 0xf5a030, 0}},
    /* Snow bot (Gemini): snow drift with a twig, icicles, coal buttons */
    {2,
     {"................", "..........4.4...", "......111..4....", "....11111114....", "...1111211111...",
      "..112111111211..", "..122222222221..", "...3.3....3.3...", "...3........3...", "................",
      "................", "................", "................", ".......55.......", ".......55.......",
      "................"},
     {0xf4f8ff, 0xb8cce8, 0x9ad8f8, 0x6a4a2a, 0x26262e}},
};

#define CELL_NONE 0xffffffffu

static uint32_t costume_cell(const Costume *k, int r, int col) {
    char ch = k->art[r][col];
    int rr = r - COSTUME_OY, rc = col - k->ox;
    if (ch == '.' || (rr >= 4 && rr <= 6 && rc >= 3 && rc <= 8)) return CELL_NONE;
    if (ch == 'o') return COL_OUTLINE;
    if (ch >= '1' && ch <= '5') return k->ink[ch - '1'];
    return CELL_NONE;
}

static void costume_draw(const Pen *p, const Costume *k, int x, int y, int s) {
    int ox = x - k->ox * s, oy = y - COSTUME_OY * s;
    for (int r = 0; r < COSTUME_ROWS; r++) {
        int n = (int)strlen(k->art[r]);
        for (int col = 0; col < n;) {
            uint32_t cc = costume_cell(k, r, col);
            int run = col + 1;
            while (run < n && costume_cell(k, r, run) == cc) run++;
            if (cc != CELL_NONE) fill(p, ox + col * s, oy + r * s, (run - col) * s, s, cc);
            col = run;
        }
    }
}

void appearance_draw_robot(Canvas *c, int id, int x, int y, int scale) {
    int k = robot_of(id);
    if (k < 0 || scale <= 0) return;
    Pen p = pen_box(c, 0, 0, SCREEN_W, SCREEN_H);
    costume_draw(&p, &COSTUMES[k], x, y, scale);
}

static void robot_body(const Pen *p, const Palette *pal, int x, int y, int s) {
    for (int r = 0; r < 12; r++)
        for (int col = 0; col < 12;) {
            char ch = APPEARANCE_ROBOT[r][col];
            int run = col + 1;
            while (run < 12 && APPEARANCE_ROBOT[r][run] == ch) run++;
            uint32_t color = ch == 'o' ? COL_OUTLINE : ch == 'b' ? pal->body : ch == 'l' ? pal->light
                           : ch == 'd'   ? pal->dark
                           : ch == 'a'   ? pal->accent
                           : ch == 'v'   ? APPEARANCE_VISOR
                                         : CELL_NONE;
            if (color != CELL_NONE) fill(p, x + col * s, y + r * s, (run - col) * s, s, color);
            col = run;
        }
}

/* Family whose robots wear costume id (the WEAR_BOT_* slot it equips into). */
static const Palette *costume_family_pal(int id) {
    int slot = pet_unlock_slot(id);
    for (int f = 0; f < N_FAMILIES; f++)
        if (pet_robot_slot((Family)f) == slot) return &PALETTES[f];
    return &PALETTES[FAM_OTHER];
}

static void preview_robot(Pen *p, int id, int k, int x, int y, int w, int h, bool locked) {
    const Costume *cos = &COSTUMES[k];
    /* union of costume cells and the robot (grid rows 4..15, columns ox+1..ox+10) */
    int c0 = cos->ox + 1, c1 = cos->ox + 10, r0 = COSTUME_OY, r1 = COSTUME_ROWS - 1;
    for (int r = 0; r < COSTUME_ROWS; r++) {
        int n = (int)strlen(cos->art[r]);
        for (int col = 0; col < n; col++)
            if (costume_cell(cos, r, col) != CELL_NONE) {
                if (col < c0) c0 = col;
                if (col > c1) c1 = col;
                if (r < r0) r0 = r;
            }
    }
    int gw = c1 - c0 + 1, gh = r1 - r0 + 1;
    int s = w / gw < h / gh ? w / gw : h / gh;
    if (s < 1) s = 1;
    int rx = x + (w - gw * s) / 2 + (cos->ox - c0) * s, ry = y + (h - gh * s) / 2 + (COSTUME_OY - r0) * s;
    if (locked) {
        p->fade = 256;
        p->to = SILHOUETTE;
    }
    const Palette *pal = costume_family_pal(id);
    robot_body(p, pal, rx, ry, s);
    costume_draw(p, cos, rx, ry, s);
    if (!locked) /* open eyes, as in the den (scale 3: 3x6 at x+12 / x+21, y+13) */
        for (int e = 0; e < 2; e++) fill(p, rx + (12 + 9 * e) * s / 3, ry + 13 * s / 3, s, 2 * s, pal->accent);
}

bool appearance_draw_preview(Canvas *c, int id, int x, int y, int w, int h, bool locked, int64_t now_ms) {
    Pen p = pen_box(c, x, y, w, h);
    if (locked) {
        p.fade = OBSCURE_FADE;
        p.to = OBSCURED;
    }
    int t = theme_of(id, OFF_WALL);
    if (t >= 0) { /* a swatch of wall above a strip of trim */
        int trim = h / 8 > 2 ? h / 8 : 2;
        WALLS[t](&p, x, y, w, h - trim, w >= 120 && h >= 100 ? 2 : 1, 256);
        fill(&p, x, y + h - trim, w, trim, 0x1a1d26);
        fill(&p, x, y + h - trim, w, 1, 0x343b54);
        return true;
    }
    t = theme_of(id, OFF_WINDOW);
    if (t >= 0) { /* framed pane with mullions */
        int m = w < h ? w : h, f = m / 16 > 2 ? m / 16 : 2, bar = f > 2 ? f - 1 : 1;
        fill(&p, x, y, w, h, 0x5a4232);
        fill(&p, x + 1, y + 1, w - 2, 1, 0x7a5a44);
        View v = view_box(c, x + f, y + f, w - 2 * f, h - 2 * f);
        v.p.fade = p.fade;
        v.p.to = p.to;
        window_into(&v, t, 256, now_ms);
        fill(&p, x + w / 2 - bar / 2, y + f, bar, h - 2 * f, 0x5a4232);
        fill(&p, x + f, y + h / 2 - bar / 2, w - 2 * f, bar, 0x5a4232);
        return true;
    }
    int k = robot_of(id);
    if (k >= 0) {
        preview_robot(&p, id, k, x, y, w, h, locked);
        return true;
    }
    return false;
}
