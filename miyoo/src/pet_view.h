#pragma once
/* Everything that draws the pet or handles its screen: sprite + cosmetics, the Pet screen
 * (stats, diet, wardrobe), and den decor that comes from unlocks. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "platform.h"

/* Pet (with mood decorations and worn cosmetics) in a (16*s)-square box at x,y. */
void ui_draw_pet(Canvas *c, const App *app, int x, int y, int s, int64_t now_ms);
/* Pet p drawn as form 0..PET_FORMS-1 in a (16*scale)-square box at x,y: idle mood, its worn cosmetics
 * and colours. Form 0 is its classic look for its current stage; forms >= 1 are its generated adult
 * forms at any stage (locked ones included, for browsing). p is not modified or copied. */
void pet_draw_form_preview(Canvas *c, const Pet *p, int form, int x, int y, int scale, int64_t now_ms);
/* One-line mood text for speech areas. */
const char *pet_mood_line(const App *app, int64_t now);
/* Growth progress toward the next stage in [0,1]; writes "54.2k/100.0k" (or "2.70M tok") to buf. */
double pet_xp_progress(const Pet *p, char *buf, size_t len);
/* The Pet screen body, y in [HEADER_H, FOOTER_Y). */
void pet_screen_draw(Canvas *c, const App *app, int64_t now_ms);
/* Footer hint for the Pet screen (changes while the wardrobe is open). */
const char *pet_screen_hints(const App *app);
/* Input on the Pet screen. Returns true if consumed (e.g. by the wardrobe); main.c handles the rest. */
bool pet_screen_handle(App *app, Button b, int64_t now_ms);
/* Den decor unlocked by the pet (wall art, rug, toys). Called by scene.c after the room and before
 * desks/workers/pet are drawn; draws only inside the den area (y in [HEADER_H, FOOTER_Y)). */
void pet_draw_den_decor(Canvas *c, const App *app, int64_t now_ms);
