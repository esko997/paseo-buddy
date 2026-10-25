#include "appearance.h"
#include <assert.h>
#include <stdio.h>

static Canvas canvas;
static const uint32_t GUARD = 0x123456;

static void untouched_outside(int x, int y, int w, int h) {
    for (int py = 0; py < SCREEN_H; py++)
        for (int px = 0; px < SCREEN_W; px++)
            if (px < x || px >= x + w || py < y || py >= y + h)
                assert(canvas.px[py * SCREEN_W + px] == GUARD);
}

int main(void) {
    /* Ordinary cosmetics must still reach their sprite renderer, without being erased first. */
    const int ordinary[] = {-1, U_SPROUT, U_SHINY, U_SNOW_SCARF};
    for (unsigned i = 0; i < sizeof ordinary / sizeof ordinary[0]; i++) {
        gfx_clear(&canvas, GUARD);
        assert(!appearance_draw_preview(&canvas, ordinary[i], 17, 23, 47, 39, false, 12345));
        untouched_outside(0, 0, 0, 0);
    }
    /* Small and edge-adjacent wardrobe thumbnails must never paint over neighbouring controls. */
    const int boxes[][4] = {{17, 23, 47, 39}, {0, 0, 17, 19}, {623, 461, 17, 19}};
    for (int id = 0; id < N_UNLOCKS; id++) {
        int slot = pet_unlock_slot(id);
        if (slot != WEAR_WALL && slot != WEAR_WINDOW && slot < WEAR_BOT_CLAUDE) continue;
        for (unsigned b = 0; b < sizeof boxes / sizeof boxes[0]; b++) {
            const int *r = boxes[b];
            for (int locked = 0; locked <= 1; locked++) {
                gfx_clear(&canvas, GUARD);
                assert(appearance_draw_preview(&canvas, id, r[0], r[1], r[2], r[3], locked, 12345));
                untouched_outside(r[0], r[1], r[2], r[3]);
            }
        }
    }
    /* The den's wall and window regions have UI immediately beside them. */
    for (int theme = 0; theme < 6; theme++) {
        for (int light = 0; light <= 256; light += 256) {
            gfx_clear(&canvas, GUARD);
            appearance_draw_wall(&canvas, 64 + 10 * theme, 14, 47, 173, 83, light);
            untouched_outside(14, 47, 173, 83);
            gfx_clear(&canvas, GUARD);
            appearance_draw_window(&canvas, 65 + 10 * theme, 222, 54, 196, 88, light, 12345);
            untouched_outside(222, 54, 196, 88);
        }
    }
    puts("PASS appearance thumbnails, walls and windows respect neighbouring UI bounds");
    return 0;
}
