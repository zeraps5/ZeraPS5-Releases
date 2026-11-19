/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Kraken (Oodle newLZ) block decoder for the compressed blocks of PS5 fake
 * packages' inner PFS image. C port of LibProsperoPkg's KrakenDecoder
 * (Copyright (C) 2026 SvenGDK, GPL-3.0-or-later), whose decode logic derives
 * from ooz by Powzix (kraken.cpp, GPL-3.0 per LibProsperoPkg NOTICE). */
#ifndef PS5C_BASE_KRAKEN_H
#define PS5C_BASE_KRAKEN_H

#include <stddef.h>
#include <stdint.h>

typedef enum kraken_status {
    KRAKEN_OK = 0,
    KRAKEN_MALFORMED = 1,
    KRAKEN_UNSUPPORTED = 2, /* entropy type 1 (tANS), 3 (RLE) or 5 */
} kraken_status;

typedef struct kraken_scratch kraken_scratch;
kraken_scratch *kraken_scratch_new(void);
void kraken_scratch_free(kraken_scratch *s);

/* Decodes one block of up to 256 KiB: two 128 KiB sub-chunks with no chunk
 * headers. flags: the per-sub-chunk bits (0x01/0x10 delta literals,
 * 0x02/0x20 LZ, 0x40 second sub-chunk restarts its window);
 * first_chunk_comp: compressed length of the first sub-chunk when dst_len
 * exceeds 128 KiB. dst may be left partly written on failure. */
kraken_status kraken_decode_block(kraken_scratch *s, const uint8_t *src, size_t src_len,
                                  int flags, int first_chunk_comp, uint8_t *dst, size_t dst_len);

#endif
