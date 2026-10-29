// Mediathek - Declarations of the vendored stb_image functions the app uses.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

// Decodes a JPEG or PNG held in memory into RGBA8; free the result with
// hui_image_free. Returns NULL when the data is not a supported image.
unsigned char *hui_image_decode(const unsigned char *data, int size, int *width, int *height);
// Box-filters RGBA8 pixels to a new size; free the result with hui_image_free.
unsigned char *hui_image_resize(const unsigned char *rgba, int width, int height, int new_width,
                                int new_height);
void hui_image_free(void *pixels);

#ifdef __cplusplus
}
#endif
