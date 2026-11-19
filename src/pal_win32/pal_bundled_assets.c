/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pal/bundled_assets.h"
const void *pal_bundled_asset(unsigned id, size_t *size) {
    if (size) *size = 0;
    if (id != PS5C_BUNDLED_FONT && id != PS5C_BUNDLED_INTRO) return NULL;
    HMODULE image = GetModuleHandleW(NULL);
    HRSRC resource = FindResourceW(image, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!resource) return NULL;
    DWORD bytes = SizeofResource(image, resource);
    HGLOBAL loaded = bytes ? LoadResource(image, resource) : NULL;
    const void *data = loaded ? LockResource(loaded) : NULL;
    if (data && size) *size = bytes;
    return data;
}
