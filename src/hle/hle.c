/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/hle.h"

#include <string.h>

#include "base/dynarray.h"
#include "base/hashmap.h"
#include "base/log.h"
#include "loader/nid.h"

typedef struct hle_entry {
    uint64_t nid;
    const hle_export *exp;
} hle_entry;

static DYNARRAY(hle_entry) g_entries;
static hmap64 g_index; /* key(library, nid) -> g_entries index */
static bool g_ready;

/* Positive module handles, disjoint from loaded PRX indices. */
#define HLE_MODULE_BASE 0x40000000u
uint32_t hle_library_handle(const char *library) {
    for(size_t i=0;i<g_entries.len;i++)if(!strcmp(g_entries.data[i].exp->library,library))
        return HLE_MODULE_BASE+(uint32_t)i;
    return 0;
}
const char *hle_library_from_handle(uint32_t handle) {
    uint32_t i=handle-HLE_MODULE_BASE;
    return handle>=HLE_MODULE_BASE && i<g_entries.len?g_entries.data[i].exp->library:NULL;
}

static uint64_t lib_key(const char *library, uint64_t nid) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const char *s = library; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
    return nid ^ hash_u64(h);
}

/* Called by the per-library registration functions. */
void hle_register_table(const hle_export *table, size_t count);
void hle_register_table(const hle_export *table, size_t count) {
    for (size_t i = 0; i < count; i++) {
        /* "#<11-char NID>" registers a function whose name is unknown. */
        uint64_t nid = 0;
        if (table[i].name[0] == '#') {
            if (!nid_from_str(table[i].name + 1, strlen(table[i].name + 1), &nid)) {
                LOG_E(HLE, "bad NID '%s'", table[i].name);
                continue;
            }
        } else {
            nid = nid_hash_name(table[i].name);
        }
        hle_entry e = {nid, &table[i]};
        if (!dyn_push(&g_entries, e) || !hmap64_put(&g_index, lib_key(table[i].library, e.nid), g_entries.len - 1)) {
            LOG_E(HLE, "out of memory registering %s", table[i].name);
            return;
        }
    }
}

ps5c_err hle_registry_init(void) {
    if (g_ready) {
        return PS5C_OK;
    }
    hmap64_init(&g_index);
    hle_register_libkernel();
    hle_register_libkernel_sync();
    hle_register_libkernel_thread();
    hle_register_libkernel_memory();
    hle_register_libc_internal();
    hle_register_libkernel_fs();
    hle_register_system();
    hle_register_agc_driver();
    hle_register_equeue();
    hle_register_video_out();
    hle_register_libkernel_apr();
    hle_register_services();
    hle_register_audio_out();
    hle_register_audio_out2();
    hle_register_fiber();
    hle_register_ult();
    hle_register_platform();
    hle_register_psml();
    hle_register_json2();
    hle_register_rtc();
    hle_register_net_offline();
    hle_register_save_data();
    hle_register_save_data_dir();
    hle_register_dialogs();
    hle_register_services_offline();
    hle_register_av_player();
    hle_register_videodec2();
    hle_register_vdecsw();
    hle_register_ampr();
    hle_register_png_enc();
    hle_register_png_dec();
    hle_register_jpeg_dec();
    hle_register_audiodec();
    hle_register_compat_stubs();
    hle_register_zlib();
    hle_register_audio3d();
    hle_register_jpeg_enc();
    hle_register_ajm();
    hle_register_acm();
    hle_register_ngs2();
    hle_register_font();
    hle_register_voice();
    hle_register_camera();
    hle_register_np_trophy();
    hle_register_playgo();
    g_ready = true;
    LOG_D(HLE, "%u HLE exports registered", (unsigned)g_entries.len);
    return PS5C_OK;
}

void hle_registry_free(void) {
    dyn_free(&g_entries);
    hmap64_free(&g_index);
    g_ready = false;
}

uint32_t hle_registry_count(void) { return (uint32_t)g_entries.len; }

const hle_export *hle_find(const char *library, uint64_t nid) {
    uint64_t idx;
    if (!library || !hmap64_get(&g_index, lib_key(library, nid), &idx)) {
        return NULL;
    }
    const hle_entry *e = &g_entries.data[idx];
    return e->nid == nid && strcmp(e->exp->library, library) == 0 ? e->exp : NULL;
}

const hle_export *hle_find_any(uint64_t nid) {
    for (size_t i = 0; i < g_entries.len; i++) {
        if (g_entries.data[i].nid == nid) return g_entries.data[i].exp;
    }
    return NULL;
}

bool hle_query(void *user, const char *library, uint64_t nid) {
    PS5C_UNUSED(user);
    return hle_find(library, nid) != NULL;
}

uint64_t hle_arg(hle_call *c, uint32_t index) {
    if (index < 6) {
        return c->a->gpr[index];
    }
    uint64_t v = 0;
    gmem_read_u64(c->mem, c->a->stack + (uint64_t)(index - 6) * 8, &v);
    return v;
}

void *hle_mem(hle_call *c, guest_addr addr, uint64_t size) {
    return addr ? gmem_host(c->mem, addr, size ? size : 1) : NULL;
}

bool hle_read_str(hle_call *c, guest_addr addr, char *buf, size_t buf_size) {
    if (!addr || buf_size == 0) {
        return false;
    }
    size_t i = 0;
    for (; i + 1 < buf_size; i++) {
        const char *p = (const char *)gmem_host(c->mem, addr + i, 1);
        if (!p) {
            buf[i] = '\0';
            return false;
        }
        buf[i] = *p;
        if (*p == '\0') {
            return true;
        }
    }
    buf[i] = '\0';
    return true;
}
