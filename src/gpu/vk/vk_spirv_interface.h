/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_VK_SPIRV_INTERFACE_H
#define PS5C_VK_SPIRV_INTERFACE_H
#include "gpu/shader/spv.h"
#include <stdlib.h>

/* Our translator emits one vec4 variable per user Location, with no component
 * packing or interface blocks. Check that contract before handing cached
 * modules to the driver. This is not a general SPIR-V validator. */
static inline bool vk_spirv_locations(const uint32_t *code, size_t words,
                                     uint32_t storage, uint32_t *mask) {
    if (!code || words < 5 || code[0] != 0x07230203 || !code[3] || code[3] > (1u << 20)) return false;
    uint32_t *locations = calloc(code[3], sizeof(*locations));
    if (!locations) return false;
    bool ok = true;
    uint32_t result = 0;
    for (unsigned pass = 0; pass < 2 && ok; pass++) {
        for (size_t i = 5; i < words;) {
            uint32_t n = code[i] >> 16, op = code[i] & 65535;
            if (!n || n > words - i) { ok = false; break; }
            if (!pass && op == SpvOpDecorate && n >= 4 && code[i+2] == SpvDecorationLocation) {
                if (code[i+1] >= code[3] || code[i+3] >= 32) { ok = false; break; }
                locations[code[i+1]] = 1u << code[i+3];
            }
            if (pass && op == SpvOpVariable && n >= 4 && code[i+3] == storage) {
                if (code[i+2] >= code[3]) { ok = false; break; }
                result |= locations[code[i+2]];
            }
            i += n;
        }
    }
    free(locations);
    if (ok) *mask = result;
    return ok;
}
#endif
