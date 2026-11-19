/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_GPU_WATCH_H
#define PS5C_GPU_WATCH_H
/* Write tracking of guest pages for the GPU caches (the page manager /
 * memory tracker pattern of KytyPS5 and shadPS4).
 *
 * A cache that has just read guest memory into a host copy arms the range:
 * its pages become read-only. The first CPU write to such a page faults,
 * gpu_watch_fault marks the page written for every client and makes it
 * writable again, and the instruction is retried. A client then knows its
 * copy is current while gpu_watch_written stays false, without reading or
 * hashing the memory again.
 *
 * Pages are 4 KiB (host pages). Every client has its own written bit per
 * page; arming clears the client's bit and protects the page if it is not
 * protected already. Order for a client: arm, then read the memory (a write
 * racing with the read either lands before the protection, and the read sees
 * it, or faults and marks the page).
 *
 * Writes the CPU does not make itself must be announced:
 *   - mapping changes (map, unmap, protect, demand commits) reset the pages'
 *     protection: gpu_watch_mapping_changed (no protection call, the change
 *     already replaced it);
 *   - the OS writing into guest memory (file reads into a guest buffer)
 *     fails on a read-only page instead of faulting: gpu_watch_prepare_write
 *     makes the pages writable and marks them first.
 *
 * Guest addresses are host addresses (identity mapping). The caller arms
 * only ranges of ordinary read-write memory (no code, no read-only data).
 * Physical memory that other addresses show too is watched through all of
 * its views: arming one protects them all, a write through any marks them all. */
#include <stdbool.h>
#include <stdint.h>

#include "mem/gmem.h"

#define GPU_WATCH_PAGE 4096ull

enum {
    GPU_WATCH_TEXTURES = 0,
    GPU_WATCH_BUFFERS = 1,
    GPU_WATCH_VERTICES = 2,
    GPU_WATCH_CLIENTS = 5,
};

/* Tracking is on by default with copy-on-write pages where the system has them (Windows);
 * PS5C_GPU_WATCH=1 or =fault uses write faults instead, =0 turns it off (every range then reads
 * as written). */
bool gpu_watch_enabled(void);

/* The runtime installed the fault and mapping hooks for the identity-mapped
 * guest memory `m`: arming works from now on. Ranges are armed only when every
 * page is ordinary read-write memory (no reserved, read-only or executable
 * pages, no physical page another view shows too). */
void gpu_watch_start(gmem *m);

/* Every page of the range could be armed (see gpu_watch_start); no side effects. */
bool gpu_watch_eligible(guest_addr addr, uint64_t size);

/* The client relies on [addr, addr + size) from now on. False when the pages
 * could not all be protected: treat the range as always written. */
bool gpu_watch_arm(uint32_t client, guest_addr addr, uint64_t size);

/* A page of the range was written (or never armed by the client) since the
 * client's last arm of it. */
bool gpu_watch_written(uint32_t client, guest_addr addr, uint64_t size);

/* Changes whenever some page becomes written for some client (a write fault,
 * gpu_watch_prepare_write, a mapping change): equal values mean no page did. */
uint64_t gpu_watch_generation(void);

/* The first run of such pages in [addr, addr + size): false when there is none.
 * The run is clipped to the range. */
bool gpu_watch_find_written(uint32_t client, guest_addr addr, uint64_t size, guest_addr *run, uint64_t *len);

/* fn(page) just before a page the client armed (and nobody has written since)
 * becomes writable: it still holds the contents the client read, and `fn` may
 * copy them (copy-on-write originals). Called with the page's region locked,
 * on whatever thread writes: it must not arm, wait or take renderer locks. */
void gpu_watch_set_saver(uint32_t client, void (*fn)(guest_addr page));

/* Fault hook: a write to a page this module protected. True = handled, retry. */
bool gpu_watch_fault(guest_addr addr, bool write);

/* The mapping or protection of the range changed: every client sees it written. */
void gpu_watch_mapping_changed(guest_addr addr, uint64_t size);

/* Something other than a CPU instruction is about to write the range. */
void gpu_watch_prepare_write(guest_addr addr, uint64_t size);

/* Copy-on-write mode: one pass over every armed page, marking those the system shows written
 * (what the sweeper thread does over and over). */
void gpu_watch_sweep(void);

/* Statistics since the last call (faults handled, pages protected). */
/* Logs the tracker state and the mapping of the page at addr (diagnostics). */
void gpu_watch_log_page(guest_addr addr);
void gpu_watch_counts(uint64_t *faults, uint64_t *protected_pages);

#endif
