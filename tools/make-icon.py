#!/usr/bin/env python3
# Mediathek - Draws the launcher icon (sce_sys/icon0.png) from shapes; nothing is downloaded.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-icon.py [output png]

import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 512
SCALE = 4  # drawn at 4x and reduced, for smooth edges


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "sce_sys", "icon0.png")
    n = SIZE * SCALE
    image = Image.new("RGB", (n, n))
    draw = ImageDraw.Draw(image)
    top, bottom = (28, 14, 34), (7, 7, 11)
    for y in range(n):
        draw.line([(0, y), (n, y)], fill=lerp(top, bottom, y / (n - 1)))

    glow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    c = n // 2
    for k in range(40, 0, -1):
        r = n * 0.30 + k * n * 0.006
        g.ellipse([c - r, c - r, c + r, c + r], fill=(255, 77, 94, int(3 + 1.1 * (40 - k))))
    glow = glow.filter(ImageFilter.GaussianBlur(n * 0.02))
    image = Image.alpha_composite(image.convert("RGBA"), glow)
    draw = ImageDraw.Draw(image, "RGBA")

    # Film-strip holes down both sides.
    hole_w, hole_h = n * 0.05, n * 0.07
    for side in (n * 0.07, n * 0.88):
        for k in range(7):
            y = n * 0.09 + k * n * 0.125
            draw.rounded_rectangle([side, y, side + hole_w, y + hole_h], radius=n * 0.012,
                                   fill=(255, 255, 255, 38))

    # The ring and the play triangle.
    r = n * 0.29
    draw.ellipse([c - r, c - r, c + r, c + r], outline=(255, 77, 94, 255), width=int(n * 0.028))
    t = n * 0.15
    cx = c + n * 0.015
    draw.polygon([(cx - t * 0.8, c - t), (cx - t * 0.8, c + t), (cx + t * 1.1, c)], fill=(245, 245, 247, 255))

    image.convert("RGB").resize((SIZE, SIZE), Image.LANCZOS).save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
