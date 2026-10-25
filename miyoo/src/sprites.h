#pragma once
/* 16x16 pet pixel art. Included only by ui.c.
 * '.' transparent  o outline  b body  l highlight  d shade  E eye  m mouth  c cheek  a accent  w shell */

typedef const char *Sprite[16];

static Sprite SPR_EGG = {
    "................",
    "......oooo......",
    ".....owwwwo.....",
    "....owwwwwwo....",
    "....owaawwwo....",
    "...owwaawwwwo...",
    "...owwwwwaawo...",
    "..owwwwwwwaado..",
    "..owwaawwwwwdo..",
    "..owwaawwwwwdo..",
    "..owwwwwwaawdo..",
    "...owwwwwaado...",
    "...owwwwwwddo...",
    "....owwwwddo....",
    ".....oooooo.....",
    "................",
};

static Sprite SPR_BABY = {
    "................",
    "................",
    "................",
    "................",
    "......oooo......",
    "....oobbbboo....",
    "...obllbbbbbo...",
    "..oblbbbbbbbdo..",
    "..obbEbbbbEbdo..",
    "..obbEbbbbEbdo..",
    "..obcbbmmbbcdo..",
    "..obbbbbbbbbdo..",
    "...obbbbbbbdo...",
    "....oooooooo....",
    "................",
    "................",
};

/* Kid, teen and adult share this body; teen/adult add overlays and are drawn larger. */
static Sprite SPR_KID = {
    "................",
    "................",
    "..oo........oo..",
    "..oao......oao..",
    "..oaboooooobao..",
    "..obllbbbbbbdo..",
    ".obllbbbbbbbbdo.",
    ".obbbEbbbbEbbdo.",
    ".obbbEbbbbEbbdo.",
    ".obcbbbmmbbbcdo.",
    ".obbbbbbbbbbbdo.",
    "..obbbbbbbbbdo..",
    "..obbbbbbbbddo..",
    "...obo....obo...",
    "...ooo....ooo...",
    "................",
};

static Sprite OVL_ANTENNA = {
    ".......aa.......",
    ".......oo.......",
    ".......oo.......",
    ".......oo.......",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
};

static Sprite OVL_WINGS = {
    "................", "................", "................", "................",
    "................",
    "a..............a",
    "aa............aa",
    "aa............aa",
    "a..............a",
    "................", "................", "................", "................",
    "................", "................", "................",
};

/* Adult crests, indexed by Family (claude, gpt, gemini, other). */
static Sprite OVL_CREST[4] = {
    {".......aa.......", ".....aaaaaa.....", ".......aa.......", ".......oo.......",
     "................", "................", "................", "................",
     "................", "................", "................", "................",
     "................", "................", "................", "................"},
    {"......aaaa......", ".....a....a.....", "......aaaa......", ".......oo.......",
     "................", "................", "................", "................",
     "................", "................", "................", "................",
     "................", "................", "................", "................"},
    {"....a......a....", "...aaa....aaa...", "....a..oo..a....", ".......oo.......",
     "................", "................", "................", "................",
     "................", "................", "................", "................",
     "................", "................", "................", "................"},
    {"....aaa..aaa....", ".....aa..aa.....", ".......aa.......", ".......oo.......",
     "................", "................", "................", "................",
     "................", "................", "................", "................",
     "................", "................", "................", "................"},
};

typedef struct {
    uint32_t body, light, dark, accent;
} Palette;

/* Indexed by Family; the extra entry is the not-yet-fed egg/baby. */
#define PAL_UNFED 4
static const Palette PALETTES[5] = {
    {0xd97757, 0xf0a988, 0xa9533a, 0xffd166}, /* claude */
    {0x19a07e, 0x63d3b0, 0x0e6e57, 0xe8fff6}, /* gpt */
    {0x4f7cf0, 0x93b2ff, 0x3354b8, 0xc58af9}, /* gemini */
    {0x7cb342, 0xb5e07a, 0x557d2a, 0xf4e04d}, /* other */
    {0xd9d4e7, 0xf1eefa, 0xa9a3bd, 0x8b7fc7}, /* unfed */
};

#define COL_OUTLINE 0x1b1b24
#define COL_MOUTH 0x5a2030
#define COL_CHEEK 0xff8fa3
#define COL_SHELL 0xfbfaf5
