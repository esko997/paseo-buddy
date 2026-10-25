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
