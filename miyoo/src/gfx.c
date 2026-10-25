#include "gfx.h"

#include <string.h>

#include "font_data.h"

void gfx_clear(Canvas *c, uint32_t color) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) c->px[i] = color;
}

void gfx_rect(Canvas *c, int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    int y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + yy * SCREEN_W;
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

void gfx_frame(Canvas *c, int x, int y, int w, int h, int t, uint32_t color) {
    gfx_rect(c, x, y, w, t, color);
    gfx_rect(c, x, y + h - t, w, t, color);
    gfx_rect(c, x, y, t, h, color);
    gfx_rect(c, x + w - t, y, t, h, color);
}

int gfx_font_w(FontId f) { return f == FONT_BIG ? FONT_BIG_W : FONT_BODY_W; }
int gfx_font_h(FontId f) { return f == FONT_BIG ? FONT_BIG_H : FONT_BODY_H; }

static void glyph(Canvas *c, FontId f, int x, int y, uint32_t color, unsigned char ch) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint16_t *rows = f == FONT_BIG ? font_big_bits[ch - 32] : font_body_bits[ch - 32];
    int w = gfx_font_w(f), h = gfx_font_h(f);
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= SCREEN_H) continue;
        uint16_t bits = rows[r];
        for (int col = 0; col < w; col++) {
            int xx = x + col;
            if (xx >= 0 && xx < SCREEN_W && (bits & (0x8000u >> col))) c->px[yy * SCREEN_W + xx] = color;
        }
    }
}

int gfx_text(Canvas *c, FontId f, int x, int y, uint32_t color, const char *s, int max_chars) {
    int w = gfx_font_w(f), n = 0;
    for (; s[n] && (max_chars <= 0 || n < max_chars); n++) glyph(c, f, x + n * w, y, color, (unsigned char)s[n]);
    return n * w;
}

int gfx_text_fit(Canvas *c, FontId f, int x, int y, uint32_t color, const char *s, int max_w) {
    int cw = gfx_font_w(f), fit = max_w / cw, len = (int)strlen(s);
    if (len <= fit) return gfx_text(c, f, x, y, color, s, 0);
    if (fit <= 3) return gfx_text(c, f, x, y, color, s, fit);
    int drawn = gfx_text(c, f, x, y, color, s, fit - 3);
    return drawn + gfx_text(c, f, x + drawn, y, color, "...", 0);
}

int gfx_text_wrap(Canvas *c, FontId f, int x, int y, int max_w, int max_lines, uint32_t color, const char *s) {
    int per_line = max_w / gfx_font_w(f), lines = 0;
    while (*s && lines < max_lines) {
        while (*s == ' ') s++;
        int len = (int)strlen(s);
        int take = len <= per_line ? len : per_line;
        if (len > per_line) {
            int brk = take;
            while (brk > 0 && s[brk] != ' ') brk--;
            if (brk > per_line / 2) take = brk;
        }
        if (lines == max_lines - 1 && len > take) {
            gfx_text_fit(c, f, x, y + lines * gfx_font_h(f), color, s, max_w);
            return lines + 1;
        }
        gfx_text(c, f, x, y + lines * gfx_font_h(f), color, s, take);
        s += take;
        lines++;
    }
    return lines;
}

void gfx_bar(Canvas *c, int x, int y, int w, int h, double frac, uint32_t bg, uint32_t fg) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx_rect(c, x, y, w, h, bg);
    gfx_rect(c, x, y, (int)(w * frac), h, fg);
}
