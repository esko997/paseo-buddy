#pragma once
/* Pixel-art activity scene ("the den") and small animated agent widgets. */
#include <stdbool.h>
#include <stdint.h>

#include "app.h"

Scene *scene_create(void);
void scene_destroy(Scene *s);

/* Consumes app->events newer than the last call and advances animations (particles, the
 * wandering pet, celebrations). Call once per frame before drawing. */
void scene_update(Scene *s, const App *app, int64_t now_ms);

/* Full den screen; draws only inside y in [40, 452) (header and footer belong to ui.c). */
void scene_draw_den(const Scene *s, const App *app, Canvas *c, int64_t now_ms);

/* 16x16 animated status glyph for an agent row (running/needs-you/failed/finished/idle). */
void scene_draw_agent_icon(Canvas *c, const Agent *a, bool alert, int x, int y, int64_t now_ms);

/* Pixel bar chart of app->rate (tokens per minute, last RATE_MINUTES minutes). */
void scene_draw_sparkline(Canvas *c, const App *app, int x, int y, int w, int h, int64_t now_ms);
