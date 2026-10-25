#pragma once
#include <stdint.h>

#include "platform.h"

typedef struct {
    uint32_t px[SCREEN_W * SCREEN_H];
} Canvas;

typedef enum { FONT_BODY, FONT_BIG } FontId;

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

void gfx_clear(Canvas *c, uint32_t color);
void gfx_rect(Canvas *c, int x, int y, int w, int h, uint32_t color);
void gfx_frame(Canvas *c, int x, int y, int w, int h, int t, uint32_t color);
int gfx_font_w(FontId f);
int gfx_font_h(FontId f);
/* Draws at most max_chars characters (<= 0: unlimited); returns pixel width drawn. */
int gfx_text(Canvas *c, FontId f, int x, int y, uint32_t color, const char *s, int max_chars);
/* Like gfx_text but truncates with "..." to fit max_w pixels. */
int gfx_text_fit(Canvas *c, FontId f, int x, int y, uint32_t color, const char *s, int max_w);
/* Word-wraps into a box; returns number of lines used (<= max_lines). */
int gfx_text_wrap(Canvas *c, FontId f, int x, int y, int max_w, int max_lines, uint32_t color, const char *s);
