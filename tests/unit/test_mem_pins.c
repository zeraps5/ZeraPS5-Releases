/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdatomic.h>
#include <stdio.h>
#include "mem/gmem.h"
#include "pal/pal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct mutation {
    gmem *mem;
    guest_addr addr;
    unsigned op;
    ps5c_err result;
    atomic_bool done;
} mutation;

static int32_t mutate(void *arg) {
    mutation *w = arg;
    guest_addr out;
    if (w->op == 0) w->result = gmem_unmap(w->mem, w->addr, GUEST_PAGE_SIZE);
    else if (w->op == 1) w->result = gmem_protect(w->mem, w->addr, GUEST_PAGE_SIZE, GPROT_READ);
    else if (w->op == 2) w->result = gmem_map(w->mem, w->addr, GUEST_PAGE_SIZE, GPROT_RW,
                                           GMAP_FIXED | GMAP_REPLACE, "replacement", &out);
    else { gmem_free(w->mem); w->result = PS5C_OK; }
    atomic_store(&w->done, true);
    return 0;
}

int main(void) {
    CHECK(pal_init() == PS5C_OK);
    for (unsigned op = 0; op < 4; op++) {
        gmem m;
        gmem_config cfg = {.start = 0x200000000ull, .size = 16 * GUEST_PAGE_SIZE, .identity = false};
        CHECK(gmem_init(&m, &cfg) == PS5C_OK);
        guest_addr addr;
        CHECK(gmem_map(&m, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "pinned", &addr) == PS5C_OK);
        CHECK(gmem_pin_mappings(&m) && gmem_pin_mappings(&m));
        CHECK(gmem_write_u64(&m, addr, 0x0123456789abcdefull));
        mutation w = {.mem = &m, .addr = addr, .op = op};
        atomic_init(&w.done, false);
        pal_thread *thread;
        CHECK(pal_thread_create(&thread, mutate, &w, "mapping mutation", 0) == PS5C_OK);
        uint64_t deadline = pal_time_ns() + 2000000000ull;
        bool waiting = false;
        do {
            pal_mutex_lock(m.lock);
            waiting = op == 3 ? m.closing : m.mapping_writers != 0;
            pal_mutex_unlock(m.lock);
            if (!waiting) pal_thread_yield();
        } while (!waiting && pal_time_ns() < deadline);
        CHECK(waiting && !atomic_load(&w.done));
        uint64_t value;
        gmem_vma vma;
        CHECK(gmem_query_vma(&m, addr, false, &vma) && vma.prot == GPROT_RW);
        CHECK(gmem_read_u64(&m, addr, &value) && value == 0x0123456789abcdefull);
        if (op == 3) CHECK(!gmem_pin_mappings(&m));
        gmem_unpin_mappings(&m);
        CHECK(!atomic_load(&w.done)); /* one remaining pin keeps pages alive */
        gmem_unpin_mappings(&m);
        CHECK(pal_thread_join(thread, NULL) == PS5C_OK);
        CHECK(atomic_load(&w.done) && w.result == PS5C_OK);
        if (op == 0) CHECK(!gmem_is_mapped(&m, addr, 1));
        else if (op == 1) CHECK(gmem_query_vma(&m, addr, false, &vma) && vma.prot == GPROT_READ);
        else if (op == 2) CHECK(gmem_read_u64(&m, addr, &value) && value == 0);
        if (op != 3) gmem_free(&m);
    }
    pal_shutdown();
    puts("mapping pins: unmap/protect/replace/free wait; queries and contents stay accessible");
    return 0;
}
