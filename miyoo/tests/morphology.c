#include "morphology.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void close_colour(uint32_t actual, uint32_t a, uint32_t b) {
    for (int shift = 0; shift <= 16; shift += 8) {
        int got = (actual >> shift) & 255;
        int expected = (((a >> shift) & 255) + ((b >> shift) & 255)) / 2;
        assert(got >= expected - 1 && got <= expected + 1);
    }
}

static void equal_palette(Palette a, Palette b) {
    assert(a.body == b.body && a.light == b.light && a.dark == b.dark && a.accent == b.accent);
}

static void face_survives_parts(void) {
    /* Every combination must preserve the face and the wardrobe's eye/mouth anchors. */
    for (int combination = 0; combination < 729; combination++) {
        int code = combination;
        uint8_t v[6];
        for (int i = 0; i < 6; i++, code /= 3) v[i] = code % 3;
        PetTraits traits = {v[0], v[1], v[2], v[3], v[4], v[5], 0};
        for (int hat = 0; hat <= 1; hat++) {
            MorphGrid grid;
            morphology_compose(traits, hat, grid);
            for (int row = 0; row < 16; row++) assert(strlen(grid[row]) == 16);
            assert(grid[7][5] == 'E' && grid[8][5] == 'E');
            assert(grid[7][10] == 'E' && grid[8][10] == 'E');
            assert(grid[9][7] == 'm' && grid[9][8] == 'm');
        }
    }
    puts("PASS all generated trait combinations keep eyes, mouth and wardrobe anchors clear");
}

int main(void) {
    face_survives_parts();
    Pet p;
    pet_init(&p, "Colour", 1792929600);
    p.stage = STAGE_ADULT;
    p.adult_family = FAM_CLAUDE;
    Palette endpoints[N_FAMILIES], mix, large;
    for (int family = 0; family < N_FAMILIES; family++) {
        memset(p.life_diet, 0, sizeof p.life_diet);
        p.life_diet[family] = 100;
        morphology_palette(&p, &endpoints[family]);
        equal_palette(endpoints[family], PALETTES[family]);
    }
    for (int a = 0; a < N_FAMILIES; a++) {
        for (int b = a + 1; b < N_FAMILIES; b++) {
            memset(p.life_diet, 0, sizeof p.life_diet);
            p.life_diet[a] = p.life_diet[b] = 100;
            morphology_palette(&p, &mix);
            close_colour(mix.body, endpoints[a].body, endpoints[b].body);
            close_colour(mix.light, endpoints[a].light, endpoints[b].light);
            close_colour(mix.dark, endpoints[a].dark, endpoints[b].dark);
            close_colour(mix.accent, endpoints[a].accent, endpoints[b].accent);
            /* Long-lived pets must retain the same mixture even when the sum exceeds INT64_MAX. */
            p.life_diet[a] = p.life_diet[b] = INT64_MAX;
            morphology_palette(&p, &large);
            equal_palette(large, mix);
        }
    }
    /* A change in the dominant provider changes colour gradually, not by choosing a new palette. */
    memset(p.life_diet, 0, sizeof p.life_diet);
    Palette before, after;
    p.life_diet[0] = 1001; p.life_diet[1] = 1000; morphology_palette(&p, &before);
    p.life_diet[0] = 1000; p.life_diet[1] = 1001; morphology_palette(&p, &after);
    for (int shift = 0; shift <= 16; shift += 8) {
        int delta = (int)((before.body >> shift) & 255) - (int)((after.body >> shift) & 255);
        assert(delta >= -1 && delta <= 1);
    }
    /* Previous lives must never tint a fresh baby's natural colours. */
    p.diet[FAM_CLAUDE] = INT64_MAX;
    memset(p.life_diet, 0, sizeof p.life_diet);
    morphology_palette(&p, &after);
    equal_palette(after, PALETTES[PAL_UNFED]);
    p.life_diet[FAM_GEMINI] = 100;
    morphology_palette(&p, &after);
    equal_palette(after, PALETTES[FAM_GEMINI]);
    puts("PASS diet mixture endpoints, blended provider crossover, and large lifetime totals");
    return 0;
}
