#pragma once
/* Special appearance unlocks with their own renderers: den wallpapers (WEAR_WALL), window views
 * (WEAR_WINDOW), robot costumes per model family (WEAR_BOT_*) and pet coat palettes (WEAR_SHINY,
 * except the old U_SHINY). The den (scene.c) and the wardrobe previews (pet_view.c) draw them with
 * the same code. Everything is gfx_rect level drawing clipped to the given box: no allocation and
 * no pixel buffers. */
#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "pet.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sprites.h"
#pragma GCC diagnostic pop

/* The 12x12 desk robot shared by the den and costume previews: 'o' outline, 'b' body, 'l' light,
 * 'd' dark, 'a' accent (antenna tip) from the family Palette, 'v' visor (APPEARANCE_VISOR). Its
 * eyes are drawn on the visor (rows 4..6, columns 3..8), which costumes always leave clear. */
extern const char *const APPEARANCE_ROBOT[12];
#define APPEARANCE_VISOR 0x14161f

/* Palette of one of the six new coats; NULL for any other id (including U_SHINY). */
const Palette *appearance_coat_palette(int id);

/* Wardrobe icon / big preview of a wallpaper, window view or robot costume, centred in and clipped
 * to the w x h box at x,y (nothing outside it is touched). Robots show the costume on a robot in
 * the colours of the family that wears it. locked: an obscured swatch (walls, windows) or a
 * silhouette (robots). False, drawing nothing, for every other id. */
bool appearance_draw_preview(Canvas *c, int id, int x, int y, int w, int h, bool locked, int64_t now_ms);

/* Paints wallpaper id over the whole box (pattern anchored at x,y); daylight 0 night .. 256 day.
 * No-op for ids that are not wallpapers. */
void appearance_draw_wall(Canvas *c, int id, int x, int y, int w, int h, int daylight);

/* Paints window view id filling the box (the pane inside the frame; the den's is 196x88, other
 * sizes are scaled to cover it and cropped). now_ms drives the animation: pass a slowed clock when
 * calm. No-op for ids that are not window views. */
void appearance_draw_window(Canvas *c, int id, int x, int y, int w, int h, int daylight, int64_t now_ms);

/* Costume id over an APPEARANCE_ROBOT drawn at x,y with cell size scale (clipped to the canvas).
 * Draw it after the body and before the eyes and raised arms: it never covers the visor, so the
 * status eyes and arms stay on top. No-op for ids that are not robot costumes. */
void appearance_draw_robot(Canvas *c, int id, int x, int y, int scale);
