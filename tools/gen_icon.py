#!/usr/bin/env python3
"""Render the Onion OS app icon from the in-app pet art.

Parses miyoo/src/sprites.h (SPR_KID + OVL_ANTENNA, the claude palette and the
COL_* defines) so the icon always matches what the app draws. Output is a
74x74 RGBA PNG, the size of Onion's stock app icons (Icons/Default/app/*.png),
with the 16x16 sprite scaled 4x nearest-neighbor and centered.

Run from the repo root:  python3 tools/gen_icon.py
"""

import re
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SPRITES_H = ROOT / "miyoo" / "src" / "sprites.h"
OUT = ROOT / "miyoo" / "onion" / "icon.png"

ICON_SIZE = 74  # Onion's Icons/Default/app/*.png are 74x74
SCALE = 4
GRID = 16


def parse_sprite(src: str, name: str) -> list[str]:
    m = re.search(rf"static Sprite {name}\s*=\s*\{{(.*?)\}};", src, re.S)
    if not m:
        sys.exit(f"{SPRITES_H}: sprite {name} not found")
    rows = re.findall(r'"([^"]*)"', m.group(1))
    if len(rows) != GRID or any(len(r) != GRID for r in rows):
        sys.exit(f"{SPRITES_H}: sprite {name} is not {GRID}x{GRID}")
    return rows


def parse_define(src: str, name: str) -> int:
    m = re.search(rf"#define {name}\s+0x([0-9a-fA-F]{{6}})", src)
    if not m:
        sys.exit(f"{SPRITES_H}: #define {name} not found")
    return int(m.group(1), 16)


def parse_palette(src: str, family: str) -> dict[str, int]:
    hexes = r"\s*,\s*".join([r"0x([0-9a-fA-F]{6})"] * 4)
    m = re.search(rf"\{{\s*{hexes}\s*\}},?\s*/\*\s*{family}\s*\*/", src)
    if not m:
        sys.exit(f"{SPRITES_H}: palette '{family}' not found")
    body, light, dark, accent = (int(g, 16) for g in m.groups())
    return {"body": body, "light": light, "dark": dark, "accent": accent}


def rgba(color: int) -> tuple[int, int, int, int]:
    return (color >> 16 & 0xFF, color >> 8 & 0xFF, color & 0xFF, 255)


def blit(img: Image.Image, spr: list[str], colors: dict[str, int], outline: int) -> None:
    """Mirror of ui.c blit_layer with mood normal, eyes open."""
    for r, row in enumerate(spr):
        for c, ch in enumerate(row):
            if ch == "E":
                color = outline  # open eye: both pixels of the pair are outline
            elif ch in colors:
                color = colors[ch]
            else:
                continue
            img.putpixel((c, r), rgba(color))


def main() -> None:
    src = SPRITES_H.read_text()
    pal = parse_palette(src, "claude")
    outline = parse_define(src, "COL_OUTLINE")
    colors = {
        "o": outline,
        "b": pal["body"],
        "l": pal["light"],
        "d": pal["dark"],
        "a": pal["accent"],
        "w": parse_define(src, "COL_SHELL"),
        "c": parse_define(src, "COL_CHEEK"),
        "m": parse_define(src, "COL_MOUTH"),
    }

    sprite = Image.new("RGBA", (GRID, GRID), (0, 0, 0, 0))
    blit(sprite, parse_sprite(src, "SPR_KID"), colors, outline)
    blit(sprite, parse_sprite(src, "OVL_ANTENNA"), colors, outline)

    scaled = sprite.resize((GRID * SCALE, GRID * SCALE), Image.NEAREST)
    icon = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    offset = (ICON_SIZE - GRID * SCALE) // 2
    icon.paste(scaled, (offset, offset))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    icon.save(OUT)
    print(f"wrote {OUT.relative_to(ROOT)} ({ICON_SIZE}x{ICON_SIZE})")


if __name__ == "__main__":
    main()
