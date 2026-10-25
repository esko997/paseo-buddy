#pragma once
/* The forms browser on the Pet screen: the worn form next to a preview of any of the PET_FORMS
 * forms (locked ones too, clearly marked), what each form looks like and how far off the next one
 * is. Only A on a ready form changes the pet; every reached form stays switchable. */
#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"
#include "platform.h"

/* Opens the browser on the first ready form not browsed before, else the worn one, and marks the
 * ready forms seen. */
void evolution_open(App *app);
/* Input while the browser is open. A wears a ready preview and closes (a locked one only shows what
 * it needs); B keeps the current form and closes; D-pad cycles forms, L2/R2 jump five. Returns
 * false for L1/R1 (closes, screen switch) and X (pat); consumes everything else. */
bool evolution_handle(App *app, Button b, int64_t now_ms);
/* The browser body, y in [HEADER_H, FOOTER_Y). */
void evolution_draw(Canvas *c, const App *app, int64_t now_ms);
/* Footer hint while the browser is open. */
const char *evolution_hints(void);
