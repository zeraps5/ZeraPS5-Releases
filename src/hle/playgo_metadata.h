/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PLAYGO_METADATA_H
#define PS5C_PLAYGO_METADATA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLAYGO_METADATA_MAX_SIZE (16u << 20)

/* Where a chunk's data lies: a byte range of the package file (PS5 fPKG: the
 * mchunks cover [0, end of the outer PFS image)). */
typedef struct playgo_range {
    uint64_t offset, length;
} playgo_range;
typedef struct playgo_chunk {
    uint64_t size;
    uint64_t language_mask;
    uint8_t required_locus;
    uint32_t first_range, nranges; /* in playgo_metadata.ranges */
} playgo_chunk;
typedef struct playgo_metadata {
    uint32_t count;
    playgo_chunk *chunks;
    uint32_t nranges;
    playgo_range *ranges; /* each chunk's mchunks, in order */
} playgo_metadata;
/* PS5 PLGX. Does not infer installation from required_locus or file presence.
 * Output is untouched on failure; caller owns successful output. */
bool playgo_metadata_parse(const uint8_t *data, size_t size, playgo_metadata *out);
void playgo_metadata_free(playgo_metadata *metadata);
#endif
