// Mediathek - The single compilation unit for the vendored stb_image and stb_image_resize2.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Posters are read into memory and decoded from there, so stdio is left out.
// Only JPEG and PNG are compiled in: those are what movie artwork comes as.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO 1
#define STBI_ONLY_JPEG 1
#define STBI_ONLY_PNG 1
#define STBI_NO_LINEAR 1
#define STBI_NO_HDR 1
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "image.h"

#include <stdlib.h>

unsigned char *hui_image_decode(const unsigned char *data, int size, int *width, int *height)
{
    int channels = 0;
    return stbi_load_from_memory(data, size, width, height, &channels, 4);
}

unsigned char *hui_image_resize(const unsigned char *rgba, int width, int height, int new_width,
                                int new_height)
{
    unsigned char *out = (unsigned char *)malloc((size_t)new_width * (size_t)new_height * 4u);
    if (out == NULL)
        return NULL;
    if (stbir_resize_uint8_linear(rgba, width, height, 0, out, new_width, new_height, 0,
                                  STBIR_RGBA) == NULL)
    {
        free(out);
        return NULL;
    }
    return out;
}

void hui_image_free(void *pixels)
{
    stbi_image_free(pixels);
}
