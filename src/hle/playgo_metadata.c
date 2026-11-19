/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/playgo_metadata.h"
#include <stdlib.h>
#include <string.h>

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16;
}
static uint64_t u64(const uint8_t *p) {
    return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}
typedef struct section {
    uint32_t offset, size;
} section;

bool playgo_metadata_parse(const uint8_t *data, size_t size, playgo_metadata *out) {
    if (!data || !out || size < 256 || size > PLAYGO_METADATA_MAX_SIZE || u32(data) != 0x78676c70 ||
        u32(data + 4) != 0x1000 || u32(data + 16) != size)
        return false;
    uint32_t count = u16(data + 10), mcount = u32(data + 32);
    if (!count)
        return false;
    section s[8];
    for (size_t i = 0; i < 8; i++) {
        s[i] = (section){u32(data + 192 + i * 8), u32(data + 196 + i * 8)};
        if (!s[i].size)
            continue;
        if (s[i].offset < 256 || s[i].offset > size || s[i].size > size - s[i].offset)
            return false;
        for (size_t j = 0; j < i; j++) {
            if (s[j].size && s[i].offset < (uint64_t)s[j].offset + s[j].size &&
                s[j].offset < (uint64_t)s[i].offset + s[i].size)
                return false;
        }
    }
    if (s[0].size != (uint64_t)count * 32 || s[1].size % 4 || s[3].size != (uint64_t)mcount * 16 ||
        !s[2].size)
        return false;
    playgo_chunk *chunks = calloc(count, sizeof(*chunks));
    if (!chunks)
        return false;
    playgo_range *ranges = NULL;
    uint64_t total_refs = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = data + s[0].offset + i * 32;
        uint32_t n = u32(a + 4), ref = u32(a + 24), label = u32(a + 28);
        if ((a[2] != 0 && a[2] != 2 && a[2] != 3) || ref % 4 || ref > s[1].size ||
            n > (s[1].size - ref) / 4 || label >= s[2].size ||
            !memchr(data + s[2].offset + label, 0, s[2].size - label))
            goto bad;
        total_refs += n;
        if (total_refs > PLAYGO_METADATA_MAX_SIZE / 4)
            goto bad;
    }
    if (total_refs && !(ranges = calloc((size_t)total_refs, sizeof(*ranges))))
        goto bad;
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = data + s[0].offset + i * 32;
        uint32_t n = u32(a + 4), ref = u32(a + 24);
        chunks[i].first_range = at;
        chunks[i].nranges = n;
        chunks[i].language_mask = u64(a + 16);
        chunks[i].required_locus = a[2];
        for (uint32_t j = 0; j < n; j++) {
            uint32_t id = u32(data + s[1].offset + ref + j * 4);
            if (id >= mcount)
                goto bad;
            const uint8_t *m = data + s[3].offset + id * 16;
            uint64_t offset = u64(m), length = u64(m + 8) & 0xffffffffffffull;
            if (chunks[i].size > UINT64_MAX - length || offset > UINT64_MAX - length)
                goto bad;
            chunks[i].size += length;
            ranges[at++] = (playgo_range){offset, length};
        }
    }
    *out = (playgo_metadata){count, chunks, at, ranges};
    return true;
bad:
    free(ranges);
    free(chunks);
    return false;
}
void playgo_metadata_free(playgo_metadata *metadata) {
    free(metadata->chunks);
    free(metadata->ranges);
    memset(metadata, 0, sizeof(*metadata));
}
