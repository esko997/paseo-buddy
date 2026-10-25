#pragma once
/* Shared palette and screen layout. */
#include <stdint.h>

#include "feed.h"

#define BG 0x0f1117
#define PANEL 0x1a1d26
#define PANEL_HI 0x2a3040
#define CHROME 0x161922
#define TEXT 0xe8e8ec
#define DIM 0x8a90a0
#define FAINT 0x4a5060
#define ALERT_COL 0xff6b4a
#define SAY_COL 0xffd166
#define GROW_COL 0x8b7fc7

#define HEADER_H 40
#define FOOTER_Y 452

/* Indexed by Bucket. */
static const uint32_t BUCKET_COL[] = {0xff6b4a, 0xe5484d, 0x46c06f, 0xf5c542, 0x5b6170};

/* Context-window fill (Agent.ctx_pct): green, yellow from CTX_WARN, red from CTX_FULL. */
#define CTX_WARN 80
#define CTX_FULL 95
static inline uint32_t ctx_col(int pct) { return pct >= CTX_FULL ? 0xe5484d : pct >= CTX_WARN ? 0xf5c542 : 0x46c06f; }

/* Linear blend a -> b, t in 0..256. */
static inline uint32_t lerp_col(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255;
    int br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return (uint32_t)((ar + (br - ar) * t / 256) << 16 | (ag + (bg - ag) * t / 256) << 8 | (ab + (bb - ab) * t / 256));
}
