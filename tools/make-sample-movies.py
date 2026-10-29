#!/usr/bin/env python3
# Mediathek - Writes a folder of invented movies with generated artwork for the PC preview.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-sample-movies.py [output dir]
#
# The files are sparse and empty of video: the preview only needs names, sizes
# and pictures. Titles are invented; the artwork is drawn here, nothing is
# downloaded.

import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "third_party", "fonts", "Inter-SemiBold.ttf")
DISPLAY = os.path.join(ROOT, "third_party", "fonts", "Montserrat-Medium.ttf")

# (folder, file name, size in GB, palette hue, motif, poster?, fanart?)
MOVIES = [
    ("", "Blue Harbor (2011).mkv", 4.2, 0.58, "sea", True, True),
    ("", "The Last Lighthouse (2019) 2160p.mkv", 18.4, 0.55, "lighthouse", True, True),
    ("", "Paper Moons (2015).mp4", 2.1, 0.08, "moon", True, True),
    ("", "Night Train to Nowhere (1987).mkv", 3.3, 0.74, "train", True, False),
    ("", "Quiet Orchard (2022) 1080p.mkv", 6.8, 0.30, "orchard", True, True),
    ("", "The Glass Garden (2008).mkv", 5.1, 0.45, "garden", True, False),
    ("", "Copper Skies (2020) 2160p.mkv", 21.0, 0.06, "sun", True, True),
    ("", "Sundial (1994).avi", 1.4, 0.12, "sun", True, False),
    ("", "Winter Cartographer (2017).mkv", 7.5, 0.60, "mountain", True, True),
    ("", "Small Hours (2003).mp4", 1.9, 0.82, "moon", False, False),
    ("", "Velvet Static (2021).mkv", 8.9, 0.88, "waves", True, True),
    ("", "The Salt Road (2012).mkv", 6.0, 0.10, "road", True, False),
    ("", "Ember & Ash (2018).mkv", 7.2, 0.02, "sun", True, True),
    ("", "Lantern Season (2016).mkv", 5.6, 0.14, "lighthouse", True, False),
    ("", "Echo Valley (2009).mkv", 4.4, 0.36, "mountain", False, False),
    ("", "Parallel Tides (2023) 2160p.mkv", 24.5, 0.52, "sea", True, True),
    ("", "A Map of Small Things (2014).mkv", 3.8, 0.20, "garden", True, False),
    ("", "Harvest Moon Rising (2001).mkv", 4.9, 0.09, "moon", True, False),
    ("Space Sagas", "Orbital Dawn (2010).mkv", 7.7, 0.68, "stars", True, True),
    ("Space Sagas", "Orbital Dusk (2013).mkv", 8.1, 0.72, "stars", True, True),
    ("Space Sagas", "Orbital Eclipse (2016).mkv", 9.0, 0.78, "stars", True, False),
    ("Space Sagas", "Orbital Zenith (2020).mkv", 12.2, 0.64, "stars", True, True),
    ("", "Kite Runner of the Dunes (1999).mkv", 3.0, 0.11, "road", False, False),
    ("", "Moss & Marble (2005).mkv", 2.8, 0.38, "garden", True, False),
    ("", "The Cartwright Letters (2024).mkv", 9.4, 0.04, "train", True, True),
    ("", "Hollow Bell (2006).mkv", 3.6, 0.80, "lighthouse", True, False),
]

IN_PROGRESS = {"Blue Harbor (2011).mkv": 0.42, "Winter Cartographer (2017).mkv": 0.17,
               "Copper Skies (2020) 2160p.mkv": 0.71, "Velvet Static (2021).mkv": 0.05}


def hsv(h, s, v):
    h = h % 1.0
    i = int(h * 6)
    f = h * 6 - i
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i % 6]
    return int(r * 255), int(g * 255), int(b * 255)


def gradient(size, top, bottom):
    w, h = size
    image = Image.new("RGB", size)
    draw = ImageDraw.Draw(image)
    for y in range(h):
        t = y / max(1, h - 1)
        draw.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return image


def scene(size, hue, motif, seed):
    """A stylised picture: sky gradient, a motif, layered ridges and grain."""
    rng = random.Random(seed)
    w, h = size
    image = gradient(size, hsv(hue, 0.55, 0.78), hsv(hue + 0.06, 0.7, 0.18))
    draw = ImageDraw.Draw(image, "RGBA")
    cx, cy = w * rng.uniform(0.35, 0.65), h * rng.uniform(0.28, 0.4)
    if motif in ("sun", "moon", "sea", "lighthouse"):
        r = w * (0.2 if motif == "sun" else 0.14)
        for k in range(6, 0, -1):
            draw.ellipse([cx - r * (1 + k * 0.5), cy - r * (1 + k * 0.5), cx + r * (1 + k * 0.5), cy + r * (1 + k * 0.5)],
                         fill=(255, 240, 220, 10))
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 244, 226, 235))
    if motif == "stars":
        for _ in range(160):
            x, y, s = rng.uniform(0, w), rng.uniform(0, h * 0.75), rng.uniform(1, 3.4)
            draw.ellipse([x, y, x + s, y + s], fill=(255, 255, 255, rng.randint(120, 255)))
        draw.ellipse([w * 0.55, h * 0.25, w * 0.55 + w * 0.3, h * 0.25 + w * 0.3], fill=hsv(hue + 0.1, 0.6, 0.9) + (230,))
    if motif == "waves":
        for k in range(14):
            y0 = h * 0.35 + k * h * 0.045
            pts = [(x, y0 + math.sin(x / w * 7 + k) * h * 0.02) for x in range(0, w + 8, 8)]
            draw.line(pts, fill=hsv(hue + k * 0.02, 0.5, 0.9) + (120,), width=max(2, h // 160))
    # Layered ridges front to back.
    for layer in range(4):
        base = h * (0.52 + layer * 0.11)
        amp = h * (0.07 - layer * 0.01)
        phase = rng.uniform(0, 6.28)
        pts = [(0, h)]
        for x in range(0, w + 6, 6):
            pts.append((x, base - amp * (0.6 * math.sin(x / w * 5 + phase) + 0.4 * math.sin(x / w * 13 + phase * 2))))
        pts.append((w, h))
        shade = 0.5 - layer * 0.1
        draw.polygon(pts, fill=hsv(hue + 0.04 * layer, 0.55, max(0.05, shade * 0.55)) + (255,))
    if motif == "lighthouse":
        x0 = w * 0.62
        draw.polygon([(x0, h * 0.78), (x0 + w * 0.05, h * 0.78), (x0 + w * 0.035, h * 0.5), (x0 + w * 0.015, h * 0.5)], fill=(14, 14, 20, 255))
        draw.polygon([(x0 + w * 0.025, h * 0.49), (w, h * 0.43), (w, h * 0.55)], fill=(255, 240, 200, 70))
    if motif == "train":
        for k in range(7):
            x = w * (0.08 + k * 0.12)
            draw.rectangle([x, h * 0.7, x + w * 0.1, h * 0.76], fill=(14, 14, 20, 255))
            draw.rectangle([x + 6, h * 0.715, x + w * 0.04, h * 0.735], fill=(255, 220, 140, 230))
    if motif == "mountain":
        draw.polygon([(w * 0.1, h * 0.7), (w * 0.4, h * 0.3), (w * 0.7, h * 0.7)], fill=(235, 240, 255, 230))
    image = image.filter(ImageFilter.GaussianBlur(0.8))
    noise = Image.effect_noise(size, 14).convert("RGB")
    return Image.blend(image, noise, 0.04)


def poster(path, title, year, hue, motif, seed):
    size = (600, 900)
    image = scene(size, hue, motif, seed)
    draw = ImageDraw.Draw(image, "RGBA")
    # Fade for the lettering.
    for y in range(500, 900):
        draw.line([(0, y), (600, y)], fill=(0, 0, 0, int(215 * (y - 500) / 400)))
    big = ImageFont.truetype(DISPLAY, 58)
    small = ImageFont.truetype(FONT, 26)
    words = title.upper().split()
    lines, line = [], ""
    for word in words:
        trial = (line + " " + word).strip()
        if draw.textlength(trial, font=big) > 500 and line:
            lines.append(line)
            line = word
        else:
            line = trial
    lines.append(line)
    y = 880 - len(lines) * 66 - 50
    for text in lines:
        draw.text((44, y), text, font=big, fill=(255, 255, 255, 245))
        y += 66
    draw.text((46, 836), str(year), font=small, fill=(255, 255, 255, 190))
    image.save(path, quality=88)


def fanart(path, hue, motif, seed):
    scene((1280, 720), hue, motif, seed).save(path, quality=86)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "sample-movies")
    root = os.path.join(out, "movies")
    os.makedirs(root, exist_ok=True)
    for index, (folder, name, size_gb, hue, motif, has_poster, has_fanart) in enumerate(MOVIES):
        directory = os.path.join(root, folder) if folder else root
        os.makedirs(directory, exist_ok=True)
        path = os.path.join(directory, name)
        with open(path, "wb") as handle:
            handle.truncate(int(size_gb * (1 << 30)))
        stem = os.path.splitext(name)[0]
        import re
        title = re.sub(r"\s*\(\d{4}\).*", "", stem)
        year = int(re.search(r"\((\d{4})\)", stem).group(1))
        if has_poster:
            poster(os.path.join(directory, stem + "-poster.jpg"), title, year, hue, motif, index)
        if has_fanart:
            fanart(os.path.join(directory, stem + "-fanart.jpg"), hue, motif, index + 100)
    with open(os.path.join(root, "Blue Harbor (2011).en.srt"), "w") as handle:
        handle.write("1\n00:00:01,000 --> 00:00:05,000\nThe tide always comes back.\n\n"
                     "2\n00:00:06,000 --> 00:00:10,000\n<i>It just never brings\nthe same water.</i>\n")
    print(f"wrote {len(MOVIES)} movies to {root}")


if __name__ == "__main__":
    main()
