#pragma once
/* DNA Rewrite, an overlay of the forms browser (App.rebirth_page): a hub with this life's growth and
 * whether its final reward (Form 15) is reached, a preview choosing which trait of the old final form
 * the next life inherits, a confirmation that only Y commits, and a read-only album of archived past
 * lives. Nothing changes the pet except Y on the confirmation page (pet_rewrite_commit). File reads
 * and preview computations happen on input only, never per frame. */
#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"
#include "platform.h"

/* Opens the hub over the forms browser. */
void rebirth_open(App *app, int64_t now_ms);
/* The page body, y in [HEADER_H, FOOTER_Y). */
void rebirth_draw(Canvas *c, const App *app, int64_t now_ms);
/* Input while rebirth_page != 0. Hub: A trait preview (when ready), Y album, B back to the forms.
 * Preview: Left/Right (Up/Down) choose the inherited trait, A confirmation, B hub. Confirmation:
 * Y rewrites (then closes to the stats page), B preview. Album: Left/Right forms, L2/R2 lives,
 * B hub. Returns false for L1/R1 (closes the overlay and browser, screen switch) and X (pat);
 * consumes everything else. */
bool rebirth_handle(App *app, Button b, int64_t now_ms);
/* Footer hint for the open page. */
const char *rebirth_hints(const App *app);
