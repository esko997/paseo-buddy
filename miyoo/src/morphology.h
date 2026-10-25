#pragma once
/* The pet's natural colours and its generated adult forms. Colours blend the lifetime diet of every
 * model family; forms 1..PET_FORMS-1 are composed from hand-drawn 16x16 parts chosen by the inherited
 * PetTraits (pet.h). Form 0 and every stage before adult keep the classic sprites. Everything works
 * on caller-provided storage: no allocation and no Pet copies. */
#include <stdbool.h>
#include <stdint.h>

#include "pet.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sprites.h"
#pragma GCC diagnostic pop

/* Natural (uncoated) palette: PALETTES mixed channel by channel, weighted by the pet's positive
 * lifetime diet per family; PALETTES[PAL_UNFED] before it has eaten. Deterministic, safe for any
 * int64 totals, and it follows the current diet at every stage (adults included). */
void morphology_palette(const Pet *p, Palette *out);
/* The same diet weighting over any table indexed like PALETTES (5 entries, PAL_UNFED last), e.g. the
 * old shiny coat's SHINY_PALETTES. */
void morphology_mix(const Pet *p, const Palette table[5], Palette *out);
/* Gentle vertical gradient for natural colours: rows[r] is the palette for sprite row r, its body
 * colour lighter toward the head and darker toward the feet (light, dark and accent unchanged). */
void morphology_gradient(const Palette *base, Palette rows[16]);

/* Motion personalities (PetTraits.motion). Classic shapes bounce. */
enum { MORPH_BOUNCE, MORPH_FLOAT, MORPH_WADDLE };

/* A 16x16 shape in sprites.h codes, rows NUL-terminated so they can be used as a Sprite. */
typedef char MorphGrid[16][17];

/* p's shape drawn as `form`: form 0 is the classic sprite of its current stage (teen antenna, adult
 * family crest and wings); forms 1..PET_FORMS-1 are its generated adult forms at any stage. hat hides
 * the antenna/crest/crown and the upper ears. Face, hat, neck and held anchors match SPR_KID for every
 * generated form. Returns the motion personality. */
int morphology_shape(const Pet *p, int form, bool hat, MorphGrid out);
/* A generated form from its traits (as morphology_shape for forms >= 1). Returns traits.motion. */
int morphology_compose(PetTraits t, bool hat, MorphGrid out);

/* Body offset in pixels for a motion personality at cell size s: asleep it rests, alert it hops like
 * every pet; otherwise bounce steps, float glides up and down, waddle sways side to side. */
void morphology_motion(int motion, Mood mood, int64_t now_ms, int s, int *dx, int *dy);
