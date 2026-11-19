/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/compat_catalog.h"
#include <string.h>
#include "base/crypto.h"

static const uint8_t k_quake1_accept_original[] = {0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3, 0xcc};
/* Return the existing UDP listener instead of unconditional -1. The RIP
 * relative displacement remains valid after relocating the entire image. */
static const uint8_t k_quake1_accept_fixed[] = {0x8b, 0x05, 0xfe, 0x32, 0xdf, 0x00, 0xc3};
static const compat_patch k_patches[] = {
    {"quake1_direct_ip_host", "PPSA01880", "01.006.000",
     "Cooperativo por IP (experimental)",
     "Permite ao host receber conexões por IP. Requer o transporte LAN experimental; "
     "não libera o menu online nem os serviços PSN.",
     "083bf921771ba0bfbd3149367fbf393696cb0dfe5d7f92b30e2a8534050a1e5a",
     0x22fdc0, k_quake1_accept_original, k_quake1_accept_fixed,
     sizeof(k_quake1_accept_original), true},
};

size_t compat_patch_count(void) { return PS5C_ARRAY_COUNT(k_patches); }
const compat_patch *compat_patch_at(size_t index) {
    return index < compat_patch_count() ? &k_patches[index] : NULL;
}

compat_patch_status compat_patch_check(const compat_patch *p, const char *title,
                                      const char *version, const uint8_t *file, size_t size) {
    if (!p || !title || strcmp(title, p->title_id)) return COMPAT_PATCH_WRONG_TITLE;
    if (!version || strcmp(version, p->version)) return COMPAT_PATCH_WRONG_VERSION;
    if (!file || p->file_offset > size || p->size > size - p->file_offset)
        return COMPAT_PATCH_WRONG_BINARY;
    uint8_t hash[32];
    char hex[65];
    static const char digits[] = "0123456789abcdef";
    sha256(file, size, hash);
    for (size_t i = 0; i < sizeof(hash); i++) {
        hex[i * 2] = digits[hash[i] >> 4];
        hex[i * 2 + 1] = digits[hash[i] & 15];
    }
    hex[64] = 0;
    if (!p->sha256 || strcmp(hex, p->sha256)) return COMPAT_PATCH_WRONG_BINARY;
    if (memcmp(file + p->file_offset, p->expected, p->size)) return COMPAT_PATCH_WRONG_BYTES;
    return COMPAT_PATCH_READY;
}
