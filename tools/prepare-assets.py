#!/usr/bin/env python3
# Mediathek - Native Linux presentation asset preparation (no Windows, no texconv).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/prepare-assets.py [--icon PATH] [--background PATH]
#            [--selection-background PATH] [--launch-background PATH] [--audio PATH]
#            [--audio-start S] [--audio-duration S] [--ffmpeg CMD] [--at9-tool CMD]
#            [--output-directory DIR]
#
# Does what tools/prepare-assets.ps1 does with texconv, with Pillow and NumPy: the icon
# becomes a 512x512 PNG; a background becomes a 3840x2160 BC7_UNORM DX10 DDS without
# mipmaps, and its 3840x2160 PNG is kept as the editable source. tools/prepare-assets.sh
# calls this when there is no Windows PowerShell, then validates the result.
#
# The BC7 encoder writes mode 6 only (one subset, 7-bit RGBA endpoints, 4-bit indices, the
# picture opaque). That is a good fit for the smooth gradients of a launcher background; it
# is not texconv's encoder, so the bytes differ from a texconv run.

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

BACKGROUND_SIZE = (3840, 2160)
ICON_SIZE = (512, 512)
# BC7 mode 6 interpolation weights of the 4-bit indices, out of 64.
WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.float32)
CHUNK = 20000  # blocks encoded at a time, to bound the memory used


def fail(message):
    print("prepare-assets: " + message, file=sys.stderr)
    sys.exit(1)


def open_image(path, label):
    if not path or not os.path.isfile(path):
        fail("%s file not found: %s" % (label, path))
    try:
        return Image.open(path)
    except OSError as error:
        fail("%s is not an image Pillow can read: %s" % (label, error))


def blocks_of(image):
    """(blocks, 16, 3) float32: the pixels of every 4x4 block, row by row."""
    width, height = image.size
    pixels = np.asarray(image.convert("RGB"), dtype=np.float32)
    pixels = pixels.reshape(height // 4, 4, width // 4, 4, 3).transpose(0, 2, 1, 3, 4)
    return pixels.reshape(-1, 16, 3)


def fit_endpoints(blocks):
    """Two colours per block along its principal axis, refined by least squares."""
    mean = blocks.mean(axis=1, keepdims=True)
    centred = blocks - mean
    covariance = np.einsum("nij,nik->njk", centred, centred)
    _, vectors = np.linalg.eigh(covariance + np.eye(3, dtype=np.float32) * 1e-3)
    axis = vectors[:, :, -1][:, None, :]
    along = (centred * axis).sum(axis=2)
    low = np.clip(mean[:, 0] + axis[:, 0] * along.min(axis=1)[:, None], 0, 255)
    high = np.clip(mean[:, 0] + axis[:, 0] * along.max(axis=1)[:, None], 0, 255)
    for _ in range(3):
        direction = high - low
        length = (direction * direction).sum(axis=1)
        length[length < 1e-6] = 1.0
        t = ((blocks - low[:, None]) * direction[:, None]).sum(axis=2) / length[:, None]
        t = np.round(np.clip(t, 0, 1) * 15) / 15
        a = ((1 - t) ** 2).sum(axis=1)
        b = ((1 - t) * t).sum(axis=1)
        c = (t * t).sum(axis=1)
        det = a * c - b * b
        usable = det > 1e-4
        det[~usable] = 1.0
        p = ((1 - t)[:, :, None] * blocks).sum(axis=1)
        q = (t[:, :, None] * blocks).sum(axis=1)
        new_low = (c[:, None] * p - b[:, None] * q) / det[:, None]
        new_high = (a[:, None] * q - b[:, None] * p) / det[:, None]
        low = np.where(usable[:, None], np.clip(new_low, 0, 255), low)
        high = np.where(usable[:, None], np.clip(new_high, 0, 255), high)
    return low, high


def encode_blocks(blocks):
    """BC7 mode 6 for each block: (n, 2) uint64, the low and the high half of its 128 bits."""
    count = blocks.shape[0]
    low, high = fit_endpoints(blocks)
    # 7 bits and a p-bit of 1 make the endpoint values 2q + 1.
    q_low = np.clip(np.round((low - 1) / 2), 0, 127).astype(np.int64)
    q_high = np.clip(np.round((high - 1) / 2), 0, 127).astype(np.int64)
    v_low = (q_low * 2 + 1).astype(np.float32)
    v_high = (q_high * 2 + 1).astype(np.float32)
    palette = np.floor(((64 - WEIGHTS)[None, :, None] * v_low[:, None] +
                        WEIGHTS[None, :, None] * v_high[:, None] + 32) / 64)
    distance = ((blocks[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    index = distance.argmin(axis=2).astype(np.int64)
    # The first pixel's index has an implicit top bit of 0: swap the ends when it would be 1.
    swap = index[:, 0] >= 8
    index = np.where(swap[:, None], 15 - index, index)
    q_low, q_high = (np.where(swap[:, None], q_high, q_low),
                     np.where(swap[:, None], q_low, q_high))

    out = np.zeros((count, 2), dtype=np.uint64)
    position = 0

    def put(value, width):
        nonlocal position
        v = value.astype(np.uint64) if isinstance(value, np.ndarray) else np.uint64(value)
        if position < 64:
            out[:, 0] |= v << np.uint64(position)
            if position + width > 64:  # the field straddles the two halves
                out[:, 1] |= v >> np.uint64(64 - position)
        else:
            out[:, 1] |= v << np.uint64(position - 64)
        position += width

    put(64, 7)  # the mode: six zeros, then a one
    for channel in range(3):
        put(q_low[:, channel], 7)
        put(q_high[:, channel], 7)
    put(127, 7)  # alpha 255 at both ends
    put(127, 7)
    put(1, 1)  # the two p-bits
    put(1, 1)
    put(index[:, 0], 3)
    for pixel in range(1, 16):
        put(index[:, pixel], 4)
    assert position == 128
    return out


def dds_header(width, height):
    header = bytearray(148)
    linear_size = (width // 4) * (height // 4) * 16
    # DDS_HEADER: caps, height, width, pixel format and linear size; one surface, no mipmaps.
    struct.pack_into("<4sIIIIIII", header, 0, b"DDS ", 124, 0x81007, height, width, linear_size, 0, 1)
    struct.pack_into("<II4s", header, 76, 32, 4, b"DX10")  # the pixel format: a DX10 header follows
    struct.pack_into("<I", header, 108, 0x1000)            # caps: texture
    struct.pack_into("<IIIII", header, 128, 98, 3, 0, 1, 0)  # BC7_UNORM, 2D, 1 element
    return bytes(header)


def convert_background(source, label, stem, destination):
    """Writes <stem>.dds and returns the resized picture, which is the editable source."""
    image = open_image(source, label).convert("RGB").resize(BACKGROUND_SIZE, Image.LANCZOS)
    blocks = blocks_of(image)
    parts = [encode_blocks(blocks[start:start + CHUNK]).astype("<u8").tobytes()
             for start in range(0, blocks.shape[0], CHUNK)]
    with open(os.path.join(destination, stem + ".dds"), "wb") as out:
        out.write(dds_header(*BACKGROUND_SIZE))
        out.write(b"".join(parts))
    return image


def save_source(picture, path, source):
    """Keeps the resized picture as the editable source, unless it is the file it came from."""
    if os.path.abspath(path) != os.path.abspath(source):
        picture.save(path)


def convert_icon(source, destination):
    image = open_image(source, "Icon").convert("RGBA").resize(ICON_SIZE, Image.LANCZOS)
    image.save(os.path.join(destination, "icon0.png"))


def run(command, label):
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        fail("%s: %s was not found" % (label, command[0]))
    except subprocess.CalledProcessError as error:
        fail("%s failed with exit code %d" % (label, error.returncode))


def convert_audio(arguments, destination, work):
    source = arguments.audio
    if not os.path.isfile(source):
        fail("Audio file not found: " + source)
    output = os.path.join(destination, "snd0.at9")
    if source.lower().endswith(".at9"):
        if os.path.abspath(source) != os.path.abspath(output):
            shutil.copyfile(source, output)
    else:
        encoder = arguments.at9_tool
        if not encoder:
            fail("An ATRAC9 encoder is needed for this audio. Supply your legally obtained "
                 "ps4_at9tool.exe with --at9-tool (it runs through wine), or pass a ready .at9.")
        ffmpeg = arguments.ffmpeg or shutil.which("ffmpeg")
        if not ffmpeg:
            fail("FFmpeg was not found. Install it (sudo apt install ffmpeg) or pass --ffmpeg.")
        wav = os.path.join(work, "selection.wav")
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-ss", repr(arguments.audio_start),
             "-i", source, "-t", repr(arguments.audio_duration), "-map_metadata", "-1",
             "-af", "loudnorm=I=-28:LRA=11:TP=-2", "-ar", "48000", "-ac", "2",
             "-c:a", "pcm_s16le", wav], "Audio preparation")
        encoded = os.path.join(work, "snd0.at9")
        command = [encoder, "-e", "-br", "192", "-wholeloop", wav, encoded]
        if encoder.lower().endswith(".exe") and not os.access(encoder, os.X_OK):
            command.insert(0, shutil.which("wine") or fail("a .exe encoder needs wine"))
        run(command, "ATRAC9 encoding")
        shutil.copyfile(encoded, output)

    param_path = os.path.join(destination, "param.json")
    if os.path.isfile(param_path):
        with open(param_path, encoding="utf-8") as handle:
            param = json.load(handle)
        param.setdefault("pubtools", {})["loudnessSnd0"] = "-28.00"
        with open(param_path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(param, handle, indent=2, ensure_ascii=False)
            handle.write("\n")


def main():
    parser = argparse.ArgumentParser(description="Prepare sce_sys presentation assets on Linux.")
    parser.add_argument("--icon", default="")
    parser.add_argument("--background", default="")
    parser.add_argument("--selection-background", default="")
    parser.add_argument("--launch-background", default="")
    parser.add_argument("--audio", default="")
    parser.add_argument("--audio-start", type=float, default=0.0)
    parser.add_argument("--audio-duration", type=float, default=15.0)
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--at9-tool", default="")
    parser.add_argument("--output-directory", default="")
    arguments = parser.parse_args()

    if arguments.background and (arguments.selection_background or arguments.launch_background):
        fail("Use --background by itself, or --selection-background and --launch-background "
             "for distinct artwork.")
    if not (arguments.icon or arguments.background or arguments.selection_background or
            arguments.launch_background or arguments.audio):
        fail("Supply an icon, background, or audio input.")
    if not 0.1 <= arguments.audio_duration <= 87.3:
        fail("--audio-duration must be between 0.1 and 87.3 seconds.")

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    destination = os.path.abspath(arguments.output_directory or os.path.join(repo, "sce_sys"))
    if destination == os.path.dirname(destination):
        fail("Refusing to use a filesystem root as the output directory.")
    os.makedirs(destination, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mediathek-assets-") as work:
        if arguments.icon:
            convert_icon(arguments.icon, destination)
        if arguments.background:
            picture = convert_background(arguments.background, "Background", "pic0", destination)
            shutil.copyfile(os.path.join(destination, "pic0.dds"),
                            os.path.join(destination, "pic1.dds"))
            # One picture for both: one source is kept, not two identical copies.
            save_source(picture, os.path.join(destination, "background-source.png"),
                        arguments.background)
        else:
            if arguments.selection_background:
                picture = convert_background(arguments.selection_background,
                                             "Selection background", "pic0", destination)
                save_source(picture, os.path.join(destination, "background-source.png"),
                            arguments.selection_background)
            if arguments.launch_background:
                picture = convert_background(arguments.launch_background,
                                             "Launch background", "pic1", destination)
                save_source(picture, os.path.join(destination, "launch-background-source.png"),
                            arguments.launch_background)
        if arguments.audio:
            convert_audio(arguments, destination, work)


main()
