/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel file system: guest paths, file descriptors and file I/O.
 *
 * Guest paths are resolved through a small mount table:
 *   /app0, /hostapp                        the game folder (read-only by convention)
 *   /download0, /temp0, /data, /savedata0  user/<title>/<name>/ under the working directory
 * Relative paths are relative to /app0. ".." never leaves a mount.
 *
 * POSIX entry points return -1 and set the thread's errno; sce* ones return
 * 0x80020000 | errno. Descriptors 0-2 are the console. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/game_patch.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "core/ue_scale.h"
#include "hle/hle.h"
#include "hle/net_loopback.h"
#include "pal/pal.h"
#include "pal/pal_exfat.h"

void hle_register_table(const hle_export *table, size_t count);

/* FreeBSD errno */
#define E_NOENT 2
#define E_BADF 9
#define E_NOMEM 12
#define E_ACCES 13
#define E_FAULT 14
#define E_EXIST 17
#define E_NOTDIR 20
#define E_ISDIR 21
#define E_INVAL 22
#define E_MFILE 24
#define E_IO 5
#define E_NAMETOOLONG 63

/* FreeBSD open flags */
#define O_ACCMODE_ 0x3
#define O_WRONLY_ 0x1
#define O_RDWR_ 0x2
#define O_APPEND_ 0x8
#define O_CREAT_ 0x200
#define O_TRUNC_ 0x400
#define O_EXCL_ 0x800
#define O_DIRECTORY_ 0x20000
#define O_NONBLOCK_ 0x4
#define E_AGAIN 35
#define E_SPIPE 29
#define E_PIPE 32

#define S_IFDIR_ 0040000
#define S_IFREG_ 0100000
#define DT_DIR_ 4
#define DT_REG_ 8

#define MAX_FDS 1024
#define FIRST_FD 3
#define PATH_MAX_ 1024

typedef struct dir_entry {
    char *name;
    bool is_dir;
} dir_entry;

typedef enum { DEV_NONE, DEV_RANDOM, DEV_LOG } dev_kind;

#define PIPE_CAPACITY 65536u
typedef struct pipe_channel {
    pal_cond *changed;
    uint32_t refs[2], active, head[2], count[2];
    bool nonblock[2];
    uint8_t bytes[2][PIPE_CAPACITY];
} pipe_channel;

typedef struct fd_entry {
    bool used;
    bool is_dir;
    dev_kind dev;
    bool append;
    pal_file *file;
    volatile uint64_t pos; /* file position: all I/O is positional, so threads sharing
                              the descriptor never race on the OS file pointer */
    char *host_path;
    char *guest_path;
    pipe_channel *pipe;
    unsigned pipe_end;
    struct kq_state *kq; /* a kqueue() descriptor */
    bool cloexec;
    /* directories: snapshot taken at open, read by getdents */
    dir_entry *entries;
    uint32_t entry_count, entry_pos;
} fd_entry;

static fd_entry g_fds[MAX_FDS];
static bool g_socket_reserved[MAX_FDS];
static pal_mutex *g_lock;
static char g_app_dir[PATH_MAX_];
static char g_user_dir[256];
/* Folders laid over /app0, the first one winning: the mods in user/mods/<title id>/<mod>/
 * (reverse alphabetical order, so a later name wins; a name starting with '_' or '.' is
 * switched off), then the update (core/game_patch.h). */
#define MAX_OVERLAYS 17
static char g_overlays[MAX_OVERLAYS][300];
static uint32_t g_overlay_count;
static void init_overlays(void);

/* ---- errno ---- */

static void set_errno(hle_call *c, int e) {
    if (c->thread && c->thread->errno_addr) gmem_write(c->mem, c->thread->errno_addr, &e, 4);
}

/* int *__error(void) */
static void error_(hle_call *c) { hle_ret_u64(c, c->thread ? c->thread->errno_addr : 0); }

static void ret_posix(hle_call *c, int64_t value_or_neg_errno) {
    if (value_or_neg_errno < 0) {
        set_errno(c, (int)-value_or_neg_errno);
        hle_ret_u64(c, (uint64_t)-1);
    } else {
        hle_ret_u64(c, (uint64_t)value_or_neg_errno);
    }
}

static void ret_sce(hle_call *c, int64_t value_or_neg_errno) {
    if (value_or_neg_errno < 0) hle_ret_s32(c, (int32_t)(0x80020000u | (uint32_t)-value_or_neg_errno));
    else hle_ret_u64(c, (uint64_t)value_or_neg_errno);
}

/* ---- Paths ---- */

static void init_dirs(ps5_runtime *rt) {
    if (g_app_dir[0]) return;
    const loaded_module *m = module_set_main(rt->ms);
    const char *p = m ? m->path : ".";
    snprintf(g_app_dir, sizeof(g_app_dir), "%s", p);
    char *slash = strrchr(g_app_dir, '/');
    char *bslash = strrchr(g_app_dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) *slash = '\0';
    else snprintf(g_app_dir, sizeof(g_app_dir), ".");
    /* with an update the eboot is the update's: /app0 and the user data stay the game's */
    if (game_patch_base()) snprintf(g_app_dir, sizeof(g_app_dir), "%s", game_patch_base());
    /* user/<game folder name> */
    const char *base = g_app_dir;
    for (const char *q = g_app_dir; *q; q++) {
        if (*q == '/' || *q == '\\') base = q + 1;
    }
    snprintf(g_user_dir, sizeof(g_user_dir), "user/%.200s", *base ? base : "game");
    if (exfat_path(g_app_dir)) {
        char id[64] = {0};
        bool safe = settings_param_string("titleId", id, sizeof(id)) && id[0];
        for (const char *q = id; safe && *q; q++)
            if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) safe = false;
        if (safe) snprintf(g_user_dir, sizeof(g_user_dir), "user/%s", id);
        else {
            char image_name[PATH_MAX_]; snprintf(image_name, sizeof(image_name), "%s", g_app_dir);
            char *mark = strstr(image_name, "/@game"); if (!mark) mark = strstr(image_name, "\\@game");
            if (mark) *mark = 0;
            const char *b = image_name;
            for (const char *q = image_name; *q; q++) if (*q == '/' || *q == '\\') b = q+1;
            snprintf(g_user_dir, sizeof(g_user_dir), "user/%.200s", *b ? b : "exfat-game");
        }
    }
    /* Keep the virtual root distinguishable from the ordinary image file,
     * including when the guest opens /app0 itself. Saves retain the image name. */
    if (exfat_image_path(g_app_dir)) {
        size_t n = strlen(g_app_dir);
        if (n + 2 < sizeof(g_app_dir)) memcpy(g_app_dir+n, "/.", 3);
    }
    pal_mkdir("user");
    pal_mkdir(g_user_dir);
    init_overlays();
    LOG_I(KERNEL, "file system: /app0 -> %s, user data -> %s", g_app_dir, g_user_dir);
}

typedef struct mod_scan {
    char names[MAX_OVERLAYS - 1][96];
    uint32_t count;
} mod_scan;

static bool mod_entry(void *user, const char *name, bool is_dir) {
    mod_scan *m = (mod_scan *)user;
    if (!is_dir || name[0] == '.' || name[0] == '_' || strlen(name) >= sizeof(m->names[0])) return true;
    if (m->count == PS5C_ARRAY_COUNT(m->names)) return false;
    snprintf(m->names[m->count++], sizeof(m->names[0]), "%s", name);
    return true;
}

static int mod_cmp(const void *a, const void *b) { return -strcmp((const char *)a, (const char *)b); }

static void init_overlays(void) {
    char id[64] = {0}, dir[160];
    bool safe = settings_param_string("titleId", id, sizeof(id)) && id[0];
    for (const char *q = id; safe && *q; q++)
        safe = (*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-';
    mod_scan m = {0};
    /* [geral] escala_unreal: a generated mod sets the scene size of Unreal games (core/ue_scale.h) */
    if (safe && !getenv("PS5C_NO_MODS") && !exfat_path(g_app_dir)) ue_scale_prepare(g_app_dir, id);
    if (safe && !getenv("PS5C_NO_MODS")) {
        snprintf(dir, sizeof(dir), "user/mods/%s", id);
        pal_dir_list(dir, mod_entry, &m);
    }
    qsort(m.names, m.count, sizeof(m.names[0]), mod_cmp);
    for (uint32_t i = 0; i < m.count; i++) {
        snprintf(g_overlays[g_overlay_count++], sizeof(g_overlays[0]), "%s/%s", dir, m.names[i]);
        LOG_I(KERNEL, "mod: %s laid over /app0", g_overlays[g_overlay_count - 1]);
    }
    if (game_patch_dir()) snprintf(g_overlays[g_overlay_count++], sizeof(g_overlays[0]), "%s", game_patch_dir());
}

/* Host directory for this game's user data (saves, temp files): user/<game folder>. */
const char *hle_user_dir(ps5_runtime *rt) {
    init_dirs(rt);
    return g_user_dir;
}

/* Mount points added at run time (save data: /savedata0...). */
#define MAX_DYN_MOUNTS 16
static struct {
    char point[32];
    char host[300];
} g_mounts[MAX_DYN_MOUNTS];

bool hle_fs_mount(const char *point, const char *host_dir) {
    for (int i = 0; i < MAX_DYN_MOUNTS; i++) {
        if (g_mounts[i].point[0] && strcmp(g_mounts[i].point, point) != 0) continue;
        snprintf(g_mounts[i].host, sizeof(g_mounts[i].host), "%s", host_dir);
        snprintf(g_mounts[i].point, sizeof(g_mounts[i].point), "%s", point);
        return true;
    }
    for (int i = 0; i < MAX_DYN_MOUNTS; i++) {
        if (g_mounts[i].point[0]) continue;
        snprintf(g_mounts[i].host, sizeof(g_mounts[i].host), "%s", host_dir);
        snprintf(g_mounts[i].point, sizeof(g_mounts[i].point), "%s", point);
        return true;
    }
    return false;
}

void hle_fs_unmount(const char *point) {
    for (int i = 0; i < MAX_DYN_MOUNTS; i++) {
        if (!strcmp(g_mounts[i].point, point)) g_mounts[i].point[0] = 0;
    }
}

/* The counterpart in overlay i (a mod or the update) of a host path inside the game's folder. */
static bool overlay_twin(uint32_t i, const char *host, char *out, size_t cap) {
    size_t bl = strlen(g_app_dir);
    if (i >= g_overlay_count || strncmp(host, g_app_dir, bl) != 0 || (host[bl] && host[bl] != '/' && host[bl] != '\\'))
        return false;
    int w = snprintf(out, cap, "%s%s", g_overlays[i], host + bl);
    return w > 0 && (size_t)w < cap;
}

/* The working directory (chdir/getcwd), a normalized guest path. Relative
 * paths start there; /app0 until the game changes it. */
static char g_cwd[PATH_MAX_] = "/app0";
static atomic_flag g_cwd_lock = ATOMIC_FLAG_INIT; /* chdir is rare: a short spin */
static void cwd_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_cwd_lock, memory_order_acquire)) {}
}
static void cwd_unlock(void) { atomic_flag_clear_explicit(&g_cwd_lock, memory_order_release); }

/* Guest path -> normalized absolute guest path ("." and ".." folded, never
 * above the mount point). Returns its length, or a negative errno. */
static int normalize_path(const char *guest, char *norm, size_t cap) {
    size_t n = 0;
    const char *p = guest;
    norm[0] = 0;
    if (*p != '/') {
        cwd_lock();
        n = (size_t)snprintf(norm, cap, "%s", g_cwd);
        cwd_unlock();
        if (n >= cap) return -E_NAMETOOLONG;
        if (n == 1) n = norm[0] = 0; /* working directory "/" */
    }
    /* Normalize component by component. */
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 1 && p[0] == '.') {
            /* skip */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            char *last = strrchr(norm, '/');
            if (last && last != norm) { /* never pops the mount point itself */
                *last = '\0';
                n = strlen(norm);
            }
        } else {
            if (n + 1 + len >= cap) return -E_NAMETOOLONG;
            norm[n++] = '/';
            memcpy(norm + n, p, len);
            n += len;
            norm[n] = '\0';
        }
        p += len;
    }
    return (int)n;
}

/* Guest path -> host path. Returns 0 or a negative errno. */
static int resolve_path(hle_call *c, const char *guest, char *host, size_t host_size) {
    init_dirs(c->rt);
    char norm[PATH_MAX_];
    int normalized = normalize_path(guest, norm, sizeof(norm));
    if (normalized < 0) return normalized;
    size_t n = (size_t)normalized;
    if (n == 0) return -E_NOENT;

    const char *rest = strchr(norm + 1, '/');
    size_t mount_len = rest ? (size_t)(rest - norm) : n;
    if (!rest) rest = "";
    static const char *const user_mounts[] = {"/download0", "/download1", "/temp0", "/temp", "/data", "/savedata0"};
    int w;
    for (int i = 0; i < MAX_DYN_MOUNTS; i++) {
        if (g_mounts[i].point[0] && strlen(g_mounts[i].point) == mount_len && !strncmp(norm, g_mounts[i].point, mount_len)) {
            w = snprintf(host, host_size, "%s%s", g_mounts[i].host, rest);
            return w < 0 || (size_t)w >= host_size ? -E_NAMETOOLONG : 0;
        }
    }
    if ((mount_len == 5 && strncmp(norm, "/app0", 5) == 0) || (mount_len == 8 && strncmp(norm, "/hostapp", 8) == 0)) {
        w = snprintf(host, host_size, "%s%s", g_app_dir, rest);
        /* a mod's or the update's file wins; their folders are merged with the game's when
         * listed (do_open) */
        char twin[PATH_MAX_];
        pal_path_info pi, bi;
        for (uint32_t i = 0; w >= 0 && (size_t)w < host_size && *rest && i < g_overlay_count; i++)
            if (overlay_twin(i, host, twin, sizeof(twin)) && pal_path_stat(twin, &pi) == PS5C_OK &&
                (!pi.is_dir || pal_path_stat(host, &bi) != PS5C_OK)) {
                w = snprintf(host, host_size, "%s", twin);
                break;
            }
    } else {
        w = -1;
        for (size_t i = 0; i < PS5C_ARRAY_COUNT(user_mounts); i++) {
            if (strlen(user_mounts[i]) == mount_len && strncmp(norm, user_mounts[i], mount_len) == 0) {
                char dir[300];
                snprintf(dir, sizeof(dir), "%.255s%.*s", g_user_dir, (int)PS5C_MIN(mount_len, 16u), norm);
                pal_mkdir(dir);
                w = snprintf(host, host_size, "%s%s", dir, rest);
                break;
            }
        }
        if (w < 0) {
            LOG_W(KERNEL, "path '%s': mount point not supported", guest);
            return -E_NOENT;
        }
    }
    if (w < 0 || (size_t)w >= host_size) return -E_NAMETOOLONG;
    return 0;
}

/* The guest's root ("/", "/.", "//..", ...): no host folder, a directory of the mount points
 * (ghc::filesystem in PS4 games stats it while making paths canonical). */
typedef bool (*dir_collect_fn)(void *user, const char *name, bool is_dir);

static bool guest_is_root(const char *guest) {
    if (!*guest) return false;
    if (*guest != '/') {
        cwd_lock();
        bool at_root = !strcmp(g_cwd, "/");
        cwd_unlock();
        if (!at_root) return false;
    }
    for (const char *p = guest; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!(len == 1 && p[0] == '.') && !(len == 2 && p[0] == '.' && p[1] == '.')) return false;
        p += len;
    }
    return true;
}

static void root_entries(dir_collect_fn add, void *user) {
    static const char *const fixed[] = {"app0", "download0", "temp0", "data", "savedata0", "dev"};
    add(user, ".", true);
    add(user, "..", true);
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(fixed); i++) add(user, fixed[i], true);
    for (int i = 0; i < MAX_DYN_MOUNTS; i++) {
        const char *pt = g_mounts[i].point;
        if (!pt[0] || strchr(pt + 1, '/')) continue;
        bool dup = false;
        for (size_t k = 0; k < PS5C_ARRAY_COUNT(fixed) && !dup; k++) dup = !strcmp(pt + 1, fixed[k]);
        if (!dup) add(user, pt + 1, true);
    }
}

/* For other HLE modules (APR): 0 or a positive errno. */
int kernel_fs_resolve(hle_call *c, const char *guest, char *host, size_t host_size);
int kernel_fs_resolve(hle_call *c, const char *guest, char *host, size_t host_size) {
    return -resolve_path(c, guest, host, host_size);
}

/* For other HLE modules (mmap of a file): bytes read at off into host memory, or a negative errno. */
int64_t kernel_fs_pread_host(int32_t fd, void *buf, uint64_t size, uint64_t off);

static int read_guest_path(hle_call *c, uint32_t arg, char *out, size_t size) {
    guest_addr p = hle_arg_ptr(c, arg);
    if (!p || !hle_read_str(c, p, out, size)) return -E_FAULT;
    return 0;
}

/* ---- Descriptor table ---- */

static int fd_alloc(void) {
    for (int i = FIRST_FD; i < MAX_FDS; i++) {
        if (!g_fds[i].used && !g_socket_reserved[i]) {
            memset(&g_fds[i], 0, sizeof(g_fds[i]));
            g_fds[i].used = true;
            return i;
        }
    }
    return -E_MFILE;
}

static fd_entry *fd_get(int32_t fd) { return fd >= FIRST_FD && fd < MAX_FDS && g_fds[fd].used ? &g_fds[fd] : NULL; }

int64_t kernel_fs_pread_host(int32_t fd, void *buf, uint64_t size, uint64_t off) {
    fd_entry *e = fd_get(fd);
    if (!e || !e->file) return -E_BADF;
    if (e->is_dir) return -E_ISDIR;
    uint64_t got = 0;
    return pal_file_pread(e->file, buf, size, off, &got) == PS5C_OK ? (int64_t)got : -E_IO;
}

bool kernel_socket_claim_fd(int32_t fd) {
    if (!g_lock || fd < FIRST_FD || fd >= MAX_FDS)
        return false;
    pal_mutex_lock(g_lock);
    bool ok = !g_fds[fd].used && !g_socket_reserved[fd];
    if (ok)
        g_socket_reserved[fd] = true;
    pal_mutex_unlock(g_lock);
    return ok;
}
void kernel_socket_release_fd(int32_t fd) {
    if (!g_lock || fd < FIRST_FD || fd >= MAX_FDS)
        return;
    pal_mutex_lock(g_lock);
    g_socket_reserved[fd] = false;
    pal_mutex_unlock(g_lock);
}
int kernel_file_poll_fd(int32_t fd, bool write) {
    if (fd >= 0 && fd < FIRST_FD)
        return write ? (fd != 0) : 0;
    if (!g_lock)
        return -1;
    pal_mutex_lock(g_lock);
    fd_entry *e = fd_get(fd);
    int ready = e ? 1 : -1;
    if (e && e->pipe) {
        pipe_channel *p=e->pipe; unsigned end=e->pipe_end;
        ready = write ? (!p->refs[1-end] || p->count[1-end]<PIPE_CAPACITY)
                      : (p->count[end]!=0 || !p->refs[1-end]);
    }
    pal_mutex_unlock(g_lock);
    return ready;
}

static void pipe_release_if_unused(pipe_channel *p) {
    if (!p->refs[0] && !p->refs[1] && !p->active) {
        pal_cond_destroy(p->changed); free(p);
    }
}

static void fd_free(fd_entry *e) {
    if (e->pipe) {
        pipe_channel *p=e->pipe;
        p->refs[e->pipe_end]--; pal_cond_broadcast(p->changed);
        pipe_release_if_unused(p);
    }
    if (e->file)
        pal_file_close(e->file);
    free(e->kq);
    for (uint32_t i = 0; i < e->entry_count; i++)
        free(e->entries[i].name);
    free(e->entries);
    free(e->host_path);
    free(e->guest_path);
    memset(e, 0, sizeof(*e));
}

typedef struct dir_collect {
    dir_entry *items;
    uint32_t count, cap;
} dir_collect;

static bool collect_entry(void *user, const char *name, bool is_dir) {
    dir_collect *d = (dir_collect *)user;
    if (d->count == d->cap) {
        uint32_t cap = d->cap ? d->cap * 2 : 32;
        dir_entry *n = (dir_entry *)realloc(d->items, cap * sizeof(dir_entry));
        if (!n) return false;
        d->items = n;
        d->cap = cap;
    }
    size_t len = strlen(name);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, name, len + 1);
    d->items[d->count].name = copy;
    d->items[d->count].is_dir = is_dir;
    d->count++;
    return true;
}

/* ---- open/close ---- */

/* Devices: random sources and the debug log (/devlog/app/...). */
static dev_kind device_for(const char *guest) {
    if (strcmp(guest, "/dev/urandom") == 0 || strcmp(guest, "/dev/random") == 0) return DEV_RANDOM;
    if (strcmp(guest, "/devlog") == 0 || strncmp(guest, "/devlog/", 8) == 0) return DEV_LOG;
    return DEV_NONE;
}

static int64_t do_open(hle_call *c, const char *guest, uint32_t flags) {
    dev_kind dev = device_for(guest);
    if (dev != DEV_NONE) {
        pal_mutex_lock(g_lock);
        int fd = fd_alloc();
        if (fd >= 0) g_fds[fd].dev = dev;
        pal_mutex_unlock(g_lock);
        return fd;
    }
    char host[PATH_MAX_];
    pal_path_info info;
    bool exists, root = guest_is_root(guest);
    if (root) {
        init_dirs(c->rt);
        if (snprintf(host, sizeof(host), "%s", g_user_dir) >= (int)sizeof(host)) return -E_NAMETOOLONG;
        memset(&info, 0, sizeof(info));
        info.is_dir = exists = true;
    } else {
        int err = resolve_path(c, guest, host, sizeof(host));
        if (err) return err;
        exists = pal_path_stat(host, &info) == PS5C_OK;
    }
    if (exists && (flags & O_CREAT_) && (flags & O_EXCL_)) return -E_EXIST;
    if (!exists && !(flags & O_CREAT_)) {
        static int log_missing = -1; /* PS5C_LOG_MISSING: every file a game looks for and does not find */
        if (log_missing < 0) log_missing = getenv("PS5C_LOG_MISSING") != NULL;
        if (log_missing) LOG_I(KERNEL, "open('%s'): not found (%s)", guest, host);
        else LOG_D(KERNEL, "open('%s'): not found (%s)", guest, host);
        return -E_NOENT;
    }
    if (exists && info.is_dir && (flags & O_ACCMODE_) != 0) return -E_ISDIR;
    if ((flags & O_DIRECTORY_) && (!exists || !info.is_dir)) return -E_NOTDIR;

    pal_file *file = NULL;
    dir_collect dir = {0};
    if (root) {
        root_entries(collect_entry, &dir);
    } else if (exists && info.is_dir) {
        collect_entry(&dir, ".", true);
        collect_entry(&dir, "..", true);
        if (pal_dir_list(host, collect_entry, &dir) != PS5C_OK) {
            for (uint32_t i = 0; i < dir.count; i++) free(dir.items[i].name);
            free(dir.items);
            return -E_IO;
        }
        /* a folder of the game: the mods' and the update's entries too, each name once */
        for (uint32_t o = 0; o < g_overlay_count; o++) {
            char twin[PATH_MAX_];
            pal_path_info ti;
            dir_collect extra = {0};
            if (overlay_twin(o, host, twin, sizeof(twin)) && strcmp(twin, host) && pal_path_stat(twin, &ti) == PS5C_OK &&
                ti.is_dir && pal_dir_list(twin, collect_entry, &extra) == PS5C_OK) {
                for (uint32_t i = 0; i < extra.count; i++) {
                    bool dup = false;
                    for (uint32_t k = 0; k < dir.count && !dup; k++) dup = !strcmp(dir.items[k].name, extra.items[i].name);
                    if (!dup) collect_entry(&dir, extra.items[i].name, extra.items[i].is_dir);
                }
            }
            for (uint32_t i = 0; i < extra.count; i++) free(extra.items[i].name);
            free(extra.items);
        }
    } else {
        uint32_t pf = 0;
        uint32_t acc = flags & O_ACCMODE_;
        if (acc == 0 || acc == O_RDWR_) pf |= PAL_FILE_READ;
        if (acc == O_WRONLY_ || acc == O_RDWR_) pf |= PAL_FILE_WRITE;
        if (flags & O_CREAT_) pf |= PAL_FILE_CREATE | PAL_FILE_WRITE;
        if (flags & O_TRUNC_) pf |= PAL_FILE_TRUNCATE | PAL_FILE_WRITE;
        uint64_t t0 = pal_time_ns();
        ps5c_err pe = pal_file_open(host, pf, &file);
        /* The host can hold an open for seconds (a disk waking up, an antivirus scan):
         * a game that loads from its render thread then shows no frame meanwhile. */
        uint64_t took_ms = (pal_time_ns() - t0) / 1000000;
        if (took_ms >= 1000) LOG_W(KERNEL, "host open of %s took %llu ms", host, (unsigned long long)took_ms);
        if (pe != PS5C_OK) return pe == PS5C_ERR_NOT_FOUND ? -E_NOENT : pe == PS5C_ERR_BUSY ? -E_ACCES : -E_IO;
    }

    pal_mutex_lock(g_lock);
    int fd = fd_alloc();
    if (fd >= 0) {
        fd_entry *e = &g_fds[fd];
        e->is_dir = file == NULL;
        e->append = (flags & O_APPEND_) != 0;
        e->file = file;
        e->pos = 0;
        e->entries = dir.items;
        e->entry_count = dir.count;
        size_t hl = strlen(host) + 1, gl = strlen(guest) + 1;
        e->host_path = (char *)malloc(hl);
        e->guest_path = (char *)malloc(gl);
        if (e->host_path) memcpy(e->host_path, host, hl);
        if (e->guest_path) memcpy(e->guest_path, guest, gl);
        file = NULL;
        dir.items = NULL;
    }
    pal_mutex_unlock(g_lock);
    if (fd < 0) {
        if (file) pal_file_close(file);
        return fd;
    }
    LOG_D(KERNEL, "open('%s', 0x%x) = %d", guest, flags, fd);
    return fd;
}

static int64_t open_common(hle_call *c) {
    char path[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    return err ? err : do_open(c, path, hle_arg_u32(c, 1));
}

static void posix_open(hle_call *c) { ret_posix(c, open_common(c)); }
static void sce_open(hle_call *c) { ret_sce(c, open_common(c)); }

static int64_t do_close(int32_t fd) {
    if (fd >= 0 && fd < FIRST_FD) return 0;
    int64_t socket_result;
    if (net_loopback_close(fd, &socket_result)) return socket_result;
    pal_mutex_lock(g_lock);
    fd_entry *e = fd_get(fd);
    if (e) fd_free(e);
    pal_mutex_unlock(g_lock);
    return e ? 0 : -E_BADF;
}

static void posix_close(hle_call *c) { ret_posix(c, do_close(hle_arg_s32(c, 0))); }
static void sce_close(hle_call *c) { ret_sce(c, do_close(hle_arg_s32(c, 0))); }

/* FreeBSD pipes are bidirectional. Each endpoint has one bounded receive
 * buffer; a duplicate shares its endpoint's status and lifetime. */
static void posix_pipe(hle_call *c) {
    guest_addr output=hle_arg_ptr(c,0);
    if (!output || !hle_mem(c,output,8)) { ret_posix(c,-E_FAULT); return; }
    pipe_channel *p=calloc(1,sizeof(*p));
    if (!p || pal_cond_create(&p->changed)!=PS5C_OK) { free(p); ret_posix(c,-E_NOMEM); return; }
    pal_mutex_lock(g_lock);
    int32_t fds[2]; fds[0]=fd_alloc(); fds[1]=fds[0]>=0?fd_alloc():-E_MFILE;
    int err=0;
    if (fds[1]<0) err=-E_MFILE;
    else if (!gmem_write(c->mem,output,fds,sizeof(fds))) err=-E_FAULT;
    if (err) {
        if (fds[0]>=0) memset(&g_fds[fds[0]],0,sizeof(fd_entry));
        if (fds[1]>=0) memset(&g_fds[fds[1]],0,sizeof(fd_entry));
        pal_cond_destroy(p->changed); free(p);
    } else for (unsigned i=0;i<2;i++) {
        g_fds[fds[i]].pipe=p; g_fds[fds[i]].pipe_end=i; p->refs[i]=1;
    }
    pal_mutex_unlock(g_lock); ret_posix(c,err);
}

static void posix_dup2(hle_call *c) {
    int old=hle_arg_s32(c,0), next=hle_arg_s32(c,1);
    pal_mutex_lock(g_lock);
    fd_entry *e=fd_get(old); int64_t result;
    if ((!e && (old<0 || old>=FIRST_FD)) || next<0 || next>=MAX_FDS) result=-E_BADF;
    else if (old==next) result=next;
    /* The PS5 refuses redirecting its shell's console descriptors. Homebrew
     * uses fdopen on the pipe instead; do not silently report success. */
    else if (next<FIRST_FD) result=-1; /* EPERM */
    else if (!e || !e->pipe || g_socket_reserved[next]) result=-45; /* EOPNOTSUPP */
    else {
        pipe_channel *p=e->pipe; unsigned end=e->pipe_end;
        p->refs[end]++; /* keep alive if replacing another reference to it */
        if (g_fds[next].used) fd_free(&g_fds[next]);
        g_fds[next]=(fd_entry){.used=true,.pipe=p,.pipe_end=end}; result=next;
    }
    pal_mutex_unlock(g_lock); ret_posix(c,result);
}

static bool pipe_io(hle_call *c,int fd,guest_addr buf,uint64_t n,bool write,bool positioned,int64_t *result) {
    pal_mutex_lock(g_lock);
    fd_entry *e=fd_get(fd);
    if (!e || !e->pipe) { pal_mutex_unlock(g_lock); return false; }
    pipe_channel *p=e->pipe; unsigned end=e->pipe_end;
    if (positioned) *result=-E_SPIPE;
    else if (n>INT64_MAX) *result=-E_INVAL;
    else if (!n) *result=0;
    else if (!hle_mem(c,buf,n)) *result=-E_FAULT;
    else {
        p->active++;
        unsigned destination=write?1-end:end;
        uint32_t required=n<=4096?(uint32_t)n:1;
        while (write ? p->refs[1-end] && PIPE_CAPACITY-p->count[destination]<required
                     : p->refs[1-end] && !p->count[end]) {
            if (p->nonblock[end]) break;
            pal_cond_wait(p->changed,g_lock);
        }
        if (write && !p->refs[1-end]) *result=-E_PIPE;
        else if (!write && !p->count[end] && !p->refs[1-end]) *result=0;
        else if (write ? PIPE_CAPACITY-p->count[destination]<required : !p->count[end]) *result=-E_AGAIN;
        else {
            uint32_t amount=(uint32_t)PS5C_MIN(4096u,PS5C_MIN(n,write?PIPE_CAPACITY-p->count[destination]:p->count[end]));
            uint8_t temporary[4096];
            uint32_t at=write?(p->head[destination]+p->count[destination])%PIPE_CAPACITY:p->head[end];
            uint32_t first=PS5C_MIN(amount,PIPE_CAPACITY-at);
            if (write) {
                if (!gmem_read(c->mem,buf,temporary,amount)) *result=-E_FAULT;
                else {
                    memcpy(p->bytes[destination]+at,temporary,first);
                    memcpy(p->bytes[destination],temporary+first,amount-first);
                    p->count[destination]+=amount; *result=amount;
                }
            } else {
                memcpy(temporary,p->bytes[end]+at,first);
                memcpy(temporary+first,p->bytes[end],amount-first);
                if (!gmem_write(c->mem,buf,temporary,amount)) *result=-E_FAULT;
                else { p->head[end]=(at+amount)%PIPE_CAPACITY; p->count[end]-=amount; *result=amount; }
            }
            pal_cond_broadcast(p->changed);
        }
        p->active--; pipe_release_if_unused(p);
    }
    pal_mutex_unlock(g_lock); return true;
}

/* Pipe status flags and descriptor flags. Preserve the pre-existing fcntl
 * behavior for other descriptor kinds until their flag handling is extended. */
static void posix_fcntl(hle_call *c) {
    int fd=hle_arg_s32(c,0),cmd=hle_arg_s32(c,1); int64_t result=0;
    pal_mutex_lock(g_lock); fd_entry *e=fd_get(fd);
    if (e && e->pipe) {
        switch (cmd) {
        case 0: {
            int minimum=hle_arg_s32(c,2);
            if (minimum<0 || minimum>=MAX_FDS) result=-E_INVAL;
            else {
                result=-E_MFILE;
                for (int next=PS5C_MAX(minimum,FIRST_FD);next<MAX_FDS;next++)
                    if (!g_fds[next].used && !g_socket_reserved[next]) {
                        g_fds[next]=(fd_entry){.used=true,.pipe=e->pipe,.pipe_end=e->pipe_end};
                        e->pipe->refs[e->pipe_end]++; result=next; break;
                    }
            }
            break;
        }
        case 1: result=e->cloexec?1:0; break;
        case 2: e->cloexec=(hle_arg_u32(c,2)&1)!=0; break;
        case 3: result=O_RDWR_|(e->pipe->nonblock[e->pipe_end]?O_NONBLOCK_:0); break;
        case 4: e->pipe->nonblock[e->pipe_end]=(hle_arg_u32(c,2)&O_NONBLOCK_)!=0; break;
        default: result=-E_INVAL; break;
        }
    } else if(!e && !(fd>=0 && fd<FIRST_FD) &&
        !(fd>=0 && fd<MAX_FDS && g_socket_reserved[fd])) result=-E_BADF;
    else if (cmd<0 || cmd>4) result=-E_INVAL;
    pal_mutex_unlock(g_lock); ret_posix(c,result);
}

/* pollfd is int fd, short events, short revents on FreeBSD. Poll counts ready
 * descriptors, including invalid descriptors and hung-up pipe endpoints. A
 * bounded sleep also observes pipe and console changes; no host fd is used. */
/* poll() revents of one descriptor (POLLIN 1, POLLOUT 4, POLLHUP 0x10, POLLNVAL 0x20,
 * POLLRDNORM 0x40); takes g_lock */
static uint16_t fd_revents(int32_t fd, uint16_t events) {
    uint16_t rev = 0;
    if (net_loopback_poll_fd(fd, events, &rev)) return rev;
    if (fd < FIRST_FD) return fd > 0 ? events & 4 : 0;
    pal_mutex_lock(g_lock);
    fd_entry *e = fd_get(fd);
    if (!e) rev = 0x20;
    else if (e->pipe) {
        pipe_channel *p = e->pipe;
        unsigned end = e->pipe_end;
        if (!p->refs[1 - end]) rev |= 0x10;
        if (p->count[end] || !p->refs[1 - end]) rev |= events & 0x41;
        if (p->refs[1 - end] && p->count[1 - end] < PIPE_CAPACITY) rev |= events & 4;
    } else rev = events & 0x45;
    pal_mutex_unlock(g_lock);
    return rev;
}

static void posix_poll(hle_call *c) {
    typedef struct {int32_t fd;uint16_t events,revents;} pollfd;
    uint64_t n=hle_arg(c,1);guest_addr addr=hle_arg_ptr(c,0);int32_t timeout=hle_arg_s32(c,2);
    if(n>MAX_FDS){ret_posix(c,-E_INVAL);return;}
    pollfd *fds=n?malloc((size_t)n*sizeof(*fds)):NULL;
    if(n && (!fds || !gmem_read(c->mem,addr,fds,n*sizeof(*fds)))){free(fds);ret_posix(c,-E_FAULT);return;}
    uint64_t start=pal_time_ns(),limit=timeout<0?UINT64_MAX:(uint64_t)timeout*1000000;
    int count=0;
    do {
        count=0;
        for(uint64_t i=0;i<n;i++) {
            pollfd *f=&fds[i];f->revents=0;if(f->fd<0)continue;
            f->revents=fd_revents(f->fd,f->events);
            if(f->revents)count++;
        }
        if(count || pal_time_ns()-start>=limit)break;
        pal_sleep_ns(1000000);
    }while(!c->thread || !c->thread->exit_requested);
    bool ok=!n || gmem_write(c->mem,addr,fds,n*sizeof(*fds));free(fds);ret_posix(c,ok?count:-E_FAULT);
}

/* ---- kqueue / kevent ----
 * A kqueue is a descriptor holding registrations. Read/write filters report
 * what poll() would (pipes, loopback sockets, files always ready), user events
 * fire on NOTE_TRIGGER, timers every `data` ms. kevent waits by polling every
 * millisecond, like poll(). Other filters can be registered but never fire. */
#define KQ_MAX 64
#define KF_READ (-1)
#define KF_WRITE (-2)
#define KF_TIMER (-7)
#define KF_USER (-11)
#define EV_ADD_ 0x1u
#define EV_DELETE_ 0x2u
#define EV_ENABLE_ 0x4u
#define EV_DISABLE_ 0x8u
#define EV_ONESHOT_ 0x10u
#define EV_CLEAR_ 0x20u
#define EV_RECEIPT_ 0x40u
#define EV_ERROR_ 0x4000u
#define EV_EOF_ 0x8000u
#define NOTE_TRIGGER_ 0x01000000u
#define NOTE_FFLAGSMASK_ 0x00ffffffu
#define NOTE_FFCTRLMASK_ 0xc0000000u

typedef struct kq_kevent { /* struct kevent, 32 bytes */
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    uint64_t udata;
} kq_kevent;

typedef struct kq_reg {
    kq_kevent ev;
    bool used, disabled, fired;
    uint64_t next_ns; /* timers */
} kq_reg;

typedef struct kq_state {
    kq_reg regs[KQ_MAX];
} kq_state;

static void posix_kqueue(hle_call *c) {
    pal_mutex_lock(g_lock);
    int fd = fd_alloc();
    if (fd >= 0) {
        g_fds[fd].kq = (kq_state *)calloc(1, sizeof(kq_state));
        if (!g_fds[fd].kq) {
            fd_free(&g_fds[fd]);
            fd = -E_NOMEM;
        }
    }
    pal_mutex_unlock(g_lock);
    ret_posix(c, fd);
}

/* Applies one change under g_lock; 0 or a positive errno */
static int kq_apply(kq_state *q, const kq_kevent *ch) {
    kq_reg *r = NULL, *free_slot = NULL;
    for (int i = 0; i < KQ_MAX; i++) {
        kq_reg *x = &q->regs[i];
        if (x->used && x->ev.ident == ch->ident && x->ev.filter == ch->filter) r = x;
        else if (!x->used && !free_slot) free_slot = x;
    }
    if (!r && !(ch->flags & EV_ADD_)) return 2; /* ENOENT */
    if (ch->flags & EV_DELETE_) {
        if (!r) return 2;
        r->used = false;
        return 0;
    }
    if (!r) {
        if (!free_slot) return 12; /* ENOMEM */
        r = free_slot;
        memset(r, 0, sizeof(*r));
        r->used = true;
        r->ev.ident = ch->ident;
        r->ev.filter = ch->filter;
    }
    if (ch->flags & EV_ADD_) {
        r->ev.flags = ch->flags & (EV_ONESHOT_ | EV_CLEAR_);
        r->ev.udata = ch->udata;
        r->ev.data = ch->data;
        if (ch->filter == KF_TIMER) r->next_ns = pal_time_ns() + (uint64_t)(ch->data > 0 ? ch->data : 0) * 1000000u;
    }
    if (ch->flags & EV_DISABLE_) r->disabled = true;
    if (ch->flags & (EV_ENABLE_ | EV_ADD_)) r->disabled = (ch->flags & EV_DISABLE_) != 0;
    if (ch->filter == KF_USER) {
        uint32_t ff = ch->fflags & NOTE_FFLAGSMASK_;
        switch (ch->fflags & NOTE_FFCTRLMASK_) {
        case 0x40000000u: r->ev.fflags &= ff; break; /* NOTE_FFAND */
        case 0x80000000u: r->ev.fflags |= ff; break; /* NOTE_FFOR */
        case 0xc0000000u: r->ev.fflags = ff; break;  /* NOTE_FFCOPY */
        default: break;
        }
        if (ch->fflags & NOTE_TRIGGER_) r->fired = true;
    }
    return 0;
}

/* Ready events of the queue into out[]; -1 if kq is no longer a kqueue */
static int kq_collect(int32_t kq, kq_kevent *out, int max) {
    const size_t bytes = sizeof(kq_reg) * KQ_MAX;
    kq_reg *regs = (kq_reg *)malloc(bytes);
    if (!regs) return 0;
    pal_mutex_lock(g_lock);
    fd_entry *e = fd_get(kq);
    kq_state *q = e ? e->kq : NULL;
    if (q) memcpy(regs, q->regs, bytes);
    pal_mutex_unlock(g_lock);
    if (!q) {
        free(regs);
        return -1;
    }
    bool ready[KQ_MAX] = {false};
    uint16_t eof[KQ_MAX] = {0};
    uint64_t now = pal_time_ns();
    for (int i = 0; i < KQ_MAX; i++) {
        kq_reg *r = &regs[i];
        if (!r->used || r->disabled) continue;
        int32_t fd = (int32_t)r->ev.ident;
        if (r->ev.filter == KF_READ) {
            uint16_t rev = fd_revents(fd, 1);
            ready[i] = (rev & 0x51) != 0;
            eof[i] = rev & 0x10 ? EV_EOF_ : 0;
        } else if (r->ev.filter == KF_WRITE) {
            uint16_t rev = fd_revents(fd, 4);
            ready[i] = (rev & 0x14) != 0;
            eof[i] = rev & 0x10 ? EV_EOF_ : 0;
        } else if (r->ev.filter == KF_USER) ready[i] = r->fired;
        else if (r->ev.filter == KF_TIMER) ready[i] = now >= r->next_ns;
    }
    int n = 0;
    pal_mutex_lock(g_lock);
    e = fd_get(kq);
    if (e && e->kq == q) {
        for (int i = 0; i < KQ_MAX && n < max; i++) {
            kq_reg *r = &q->regs[i];
            if (!ready[i] || !r->used || r->ev.ident != regs[i].ev.ident || r->ev.filter != regs[i].ev.filter) continue;
            out[n] = r->ev;
            out[n].flags |= eof[i];
            if (r->ev.filter == KF_USER) out[n].fflags &= NOTE_FFLAGSMASK_;
            if (r->ev.filter == KF_TIMER) {
                uint64_t period = (uint64_t)(r->ev.data > 0 ? r->ev.data : 0) * 1000000u;
                out[n].data = 1;
                r->next_ns = now + (period ? period : 1000000u);
            } else if (r->ev.filter == KF_READ || r->ev.filter == KF_WRITE) out[n].data = 1;
            if (r->ev.flags & EV_ONESHOT_) r->used = false;
            else if (r->ev.filter == KF_USER && (r->ev.flags & EV_CLEAR_)) r->fired = false;
            n++;
        }
    }
    pal_mutex_unlock(g_lock);
    free(regs);
    return n;
}

/* int kevent(int kq, const struct kevent *changes, int nchanges, struct kevent *events, int nevents,
 *            const struct timespec *timeout) */
static void posix_kevent(hle_call *c) {
    int32_t kq = hle_arg_s32(c, 0), nchanges = hle_arg_s32(c, 2), nevents = hle_arg_s32(c, 4);
    guest_addr changes = hle_arg_ptr(c, 1), events = hle_arg_ptr(c, 3), tsp = hle_arg_ptr(c, 5);
    if (nchanges < 0 || nevents < 0 || nchanges > 4096 || nevents > 4096) {
        ret_posix(c, -E_INVAL);
        return;
    }
    uint64_t limit = UINT64_MAX;
    if (tsp) {
        int64_t ts[2];
        if (!gmem_read(c->mem, tsp, ts, sizeof(ts))) {
            ret_posix(c, -E_FAULT);
            return;
        }
        limit = ts[0] < 0 ? 0 : (uint64_t)ts[0] * 1000000000u + (uint64_t)(ts[1] < 0 ? 0 : ts[1]);
    }
    kq_kevent *out = nevents ? (kq_kevent *)calloc((size_t)nevents, sizeof(kq_kevent)) : NULL;
    if (nevents && !out) {
        ret_posix(c, -E_NOMEM);
        return;
    }
    int nout = 0;
    int64_t result = 0;
    for (int32_t i = 0; i < nchanges && !result; i++) {
        kq_kevent ch;
        if (!gmem_read(c->mem, changes + (uint64_t)i * sizeof(ch), &ch, sizeof(ch))) {
            result = -E_FAULT;
            break;
        }
        pal_mutex_lock(g_lock);
        fd_entry *e = fd_get(kq);
        int err = e && e->kq ? kq_apply(e->kq, &ch) : -1;
        pal_mutex_unlock(g_lock);
        if (err < 0) result = -E_BADF;
        else if (err || (ch.flags & EV_RECEIPT_)) {
            if (nout < nevents) {
                out[nout] = ch;
                out[nout].flags = EV_ERROR_;
                out[nout++].data = err;
            } else if (err) result = -err;
        }
    }
    if (!result && !nout && nevents) {
        uint64_t start = pal_time_ns();
        for (;;) {
            nout = kq_collect(kq, out, nevents);
            if (nout < 0) {
                result = -E_BADF;
                nout = 0;
                break;
            }
            if (nout || pal_time_ns() - start >= limit || (c->thread && c->thread->exit_requested)) break;
            pal_sleep_ns(1000000);
        }
    } else if (!result && !nevents) {
        pal_mutex_lock(g_lock);
        fd_entry *e = fd_get(kq);
        if (!e || !e->kq) result = -E_BADF;
        pal_mutex_unlock(g_lock);
    }
    if (!result && nout && !gmem_write(c->mem, events, out, (uint64_t)nout * sizeof(kq_kevent))) result = -E_FAULT;
    free(out);
    ret_posix(c, result ? result : nout);
}

/* ---- read/write/seek ---- */

/* offset < 0: current position */
static int64_t do_read(hle_call *c, int32_t fd, guest_addr buf, uint64_t n, int64_t offset) {
    int64_t pipe_result;
    if (pipe_io(c,fd,buf,n,false,offset>=0,&pipe_result)) return pipe_result;
    int64_t socket_result;
    if (net_loopback_io(c, fd, buf, n, false, offset >= 0, &socket_result))
        return socket_result;
    fd_entry *e = fd_get(fd);
    if (!e)
        return fd == 0 ? 0 : -E_BADF;
    if (e->is_dir) return -E_ISDIR;
    if (n == 0) return 0;
    void *dst = hle_mem(c, buf, n);
    if (!dst && e->file && runtime_longshot_commit(c->rt, buf, n))
        dst = hle_mem(c, buf, n);
    if (!dst) return -E_FAULT;
    if (e->dev == DEV_RANDOM) {
        static uint64_t state;
        uint8_t *p = (uint8_t *)dst;
        for (uint64_t i = 0; i < n; i++) {
            state = (state ^ pal_time_ns()) * 6364136223846793005ull + 1442695040888963407ull;
            p[i] = (uint8_t)(state >> 56);
        }
        return (int64_t)n;
    }
    if (e->dev == DEV_LOG) return 0;
    uint64_t got = 0;
    /* read(): take [pos, pos + n) first, give back what was not read */
    uint64_t at = offset >= 0 ? (uint64_t)offset : __atomic_fetch_add(&e->pos, n, __ATOMIC_SEQ_CST);
    ps5c_err err = pal_file_pread(e->file, dst, n, at, &got);
    if (offset < 0 && got < n) __atomic_fetch_sub(&e->pos, n - (err == PS5C_OK ? got : 0), __ATOMIC_SEQ_CST);
    return err == PS5C_OK ? (int64_t)got : -E_IO;
}

static void log_console(int32_t fd, const char *buf, uint64_t n) {
    size_t len = (size_t)n;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    if (!len) return;
    /* A line the game repeats (a warning printed every frame) is logged once,
     * then counted: writing it each time can cost more than the frame. */
    static pal_mutex *lock;
    static char last[256];
    static uint64_t repeats;
    if (!lock && pal_mutex_create(&lock) != PS5C_OK) return;
    pal_mutex_lock(lock);
    size_t cmp = len < sizeof(last) - 1 ? len : sizeof(last) - 1;
    if (strlen(last) == cmp && memcmp(last, buf, cmp) == 0) {
        repeats++;
        if ((repeats & (repeats - 1)) == 0 && repeats >= 16)
            LOG_I(KERNEL, "[guest %s] (last line repeated %llu times)", fd == 1 ? "stdout" : "stderr",
                  (unsigned long long)repeats);
    } else {
        if (repeats > 1) LOG_I(KERNEL, "[guest] (previous line repeated %llu times in total)", (unsigned long long)repeats);
        repeats = 0;
        memcpy(last, buf, cmp);
        last[cmp] = '\0';
        LOG_I(KERNEL, "[guest %s] %.*s", fd == 1 ? "stdout" : "stderr", (int)len, buf);
    }
    pal_mutex_unlock(lock);
}

static int64_t do_write(hle_call *c, int32_t fd, guest_addr buf, uint64_t n, int64_t offset) {
    int64_t pipe_result;
    if (pipe_io(c,fd,buf,n,true,offset>=0,&pipe_result)) return pipe_result;
    int64_t socket_result;
    if (net_loopback_io(c, fd, buf, n, true, offset >= 0, &socket_result))
        return socket_result;
    const char *src = n ? (const char *)hle_mem(c, buf, n) : "";
    if (!src)
        return -E_FAULT;
    if (fd == 1 || fd == 2) {
        log_console(fd, src, n);
        return (int64_t)n;
    }
    fd_entry *e = fd_get(fd);
    if (!e) return -E_BADF;
    if (e->is_dir) return -E_ISDIR;
    if (e->dev == DEV_LOG) {
        log_console(2, src, n);
        return (int64_t)n;
    }
    if (e->dev == DEV_RANDOM) return (int64_t)n;
    uint64_t put = 0, at;
    if (offset >= 0) {
        at = (uint64_t)offset;
    } else {
        if (e->append) {
            uint64_t size = 0;
            pal_file_size(e->file, &size);
            __atomic_store_n(&e->pos, size, __ATOMIC_SEQ_CST);
        }
        at = __atomic_fetch_add(&e->pos, n, __ATOMIC_SEQ_CST);
    }
    ps5c_err err = pal_file_pwrite(e->file, src, n, at, &put);
    if (offset < 0 && put < n) __atomic_fetch_sub(&e->pos, n - (err == PS5C_OK ? put : 0), __ATOMIC_SEQ_CST);
    return err == PS5C_OK ? (int64_t)put : -E_IO;
}

static void posix_read(hle_call *c) {
    ret_posix(c, do_read(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), -1));
}
static void sce_read(hle_call *c) {
    ret_sce(c, do_read(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), -1));
}
static void posix_pread(hle_call *c) {
    ret_posix(c, do_read(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), (int64_t)hle_arg(c, 3)));
}
static void sce_pread(hle_call *c) {
    ret_sce(c, do_read(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), (int64_t)hle_arg(c, 3)));
}
static void posix_write(hle_call *c) {
    ret_posix(c, do_write(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), -1));
}
static void sce_write(hle_call *c) {
    ret_sce(c, do_write(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), -1));
}
static void posix_pwrite(hle_call *c) {
    ret_posix(c, do_write(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), (int64_t)hle_arg(c, 3)));
}
static void sce_pwrite(hle_call *c) {
    ret_sce(c, do_write(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), (int64_t)hle_arg(c, 3)));
}

/* ---- Kernel AIO ----
 * Requests run synchronously at submission; ids only report completion.
 *   SceKernelAioRWRequest { off_t offset; size_t nbyte; void *buf;
 *                           SceKernelAioResult *result; int fd; }       (40 bytes)
 *   SceKernelAioResult    { int64_t return_value; uint32_t state; }    (16 bytes) */

#define AIO_STATE_COMPLETED 3
#define AIO_STATE_ABORTED 4
#define AIO_MAX_IDS 512

static volatile int32_t g_aio_state[AIO_MAX_IDS];
static volatile int32_t g_aio_next;

static int32_t aio_new_id(void) {
    int32_t seq = __atomic_fetch_add(&g_aio_next, 1, __ATOMIC_RELAXED);
    return (int32_t)((uint32_t)seq % (AIO_MAX_IDS - 1)) + 1;
}

static bool aio_valid(int32_t id) { return id > 0 && id < AIO_MAX_IDS; }

/* Runs one request; returns its state. */
static int32_t aio_run(hle_call *c, guest_addr req, bool write) {
    uint64_t offset = 0, nbyte = 0, buf = 0, result = 0;
    int32_t fd = -1;
    gmem_read_u64(c->mem, req, &offset);
    gmem_read_u64(c->mem, req + 8, &nbyte);
    gmem_read_u64(c->mem, req + 16, &buf);
    gmem_read_u64(c->mem, req + 24, &result);
    gmem_read(c->mem, req + 32, &fd, 4);
    int64_t r = write ? do_write(c, fd, buf, nbyte, (int64_t)offset) : do_read(c, fd, buf, nbyte, (int64_t)offset);
    int32_t state = r < 0 ? AIO_STATE_ABORTED : AIO_STATE_COMPLETED;
    int64_t value = r < 0 ? (int64_t)(int32_t)(0x80020000u | (uint32_t)-r) : r;
    if (result) {
        gmem_write(c->mem, result, &value, 8);
        gmem_write(c->mem, result + 8, &state, 4);
    }
    return state;
}

/* int sceKernelAioSubmit{Read,Write}Commands(req[], int count, int prio, SceKernelAioSubmitId *id) */
static void aio_submit(hle_call *c, bool write, bool multiple) {
    guest_addr req = hle_arg_ptr(c, 0);
    int32_t count = hle_arg_s32(c, 1);
    guest_addr ids = hle_arg_ptr(c, 3);
    if (!req || !ids) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EFAULT);
        return;
    }
    if (count <= 0 || count > 128) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    int32_t id = multiple ? 0 : aio_new_id(), state = AIO_STATE_COMPLETED;
    for (int32_t i = 0; i < count; i++) {
        int32_t st = aio_run(c, req + (uint64_t)i * 40, write);
        if (multiple) {
            int32_t own = aio_new_id();
            g_aio_state[own] = st;
            gmem_write(c->mem, ids + (uint64_t)i * 4, &own, 4);
        } else if (st != AIO_STATE_COMPLETED) {
            state = st;
        }
    }
    if (!multiple) {
        g_aio_state[id] = state;
        gmem_write(c->mem, ids, &id, 4);
    }
    hle_ret_s32(c, 0);
}

static void aio_submit_read(hle_call *c) { aio_submit(c, false, false); }
static void aio_submit_read_multiple(hle_call *c) { aio_submit(c, false, true); }
static void aio_submit_write(hle_call *c) { aio_submit(c, true, false); }
static void aio_submit_write_multiple(hle_call *c) { aio_submit(c, true, true); }

/* Poll/Wait/Cancel(id, int *state[, ...]): everything has completed already. */
static void aio_state_of(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0);
    if (!aio_valid(id)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    int32_t state = g_aio_state[id] ? g_aio_state[id] : AIO_STATE_COMPLETED;
    guest_addr out = hle_arg_ptr(c, 1);
    if (out) gmem_write(c->mem, out, &state, 4);
    hle_ret_s32(c, 0);
}

/* WaitRequests/CancelRequests(ids[], int count, int states[], ...) */
static void aio_states_of(hle_call *c) {
    guest_addr ids = hle_arg_ptr(c, 0), states = hle_arg_ptr(c, 2);
    int32_t count = hle_arg_s32(c, 1);
    if (!ids || !states || count <= 0) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        int32_t id = 0, state = AIO_STATE_COMPLETED;
        gmem_read(c->mem, ids + (uint64_t)i * 4, &id, 4);
        if (aio_valid(id) && g_aio_state[id]) state = g_aio_state[id];
        gmem_write(c->mem, states + (uint64_t)i * 4, &state, 4);
    }
    hle_ret_s32(c, 0);
}

/* DeleteRequest(id, int *ret) / DeleteRequests(ids[], int count, int rets[]) */
static void aio_delete(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0), zero = 0;
    if (!aio_valid(id)) {
        hle_ret_s32(c, SCE_KERNEL_ERROR_EINVAL);
        return;
    }
    g_aio_state[id] = 0;
    if (hle_arg_ptr(c, 1)) gmem_write(c->mem, hle_arg_ptr(c, 1), &zero, 4);
    hle_ret_s32(c, 0);
}

static void aio_delete_many(hle_call *c) {
    guest_addr ids = hle_arg_ptr(c, 0), rets = hle_arg_ptr(c, 2);
    int32_t count = hle_arg_s32(c, 1), zero = 0;
    for (int32_t i = 0; ids && i < count; i++) {
        int32_t id = 0;
        gmem_read(c->mem, ids + (uint64_t)i * 4, &id, 4);
        if (aio_valid(id)) g_aio_state[id] = 0;
        if (rets) gmem_write(c->mem, rets + (uint64_t)i * 4, &zero, 4);
    }
    hle_ret_s32(c, 0);
}

static void aio_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* struct iovec { void *base; size_t len; } */
static int64_t do_vector_io(hle_call *c, int32_t fd, guest_addr iov, int32_t count, int64_t offset, bool write) {
    if (count <= 0 || count > 1024) return -E_INVAL;
    typedef struct { guest_addr base; uint64_t len; } file_iov;
    file_iov small[8], *vectors = count <= 8 ? small : malloc((size_t)count * sizeof(*vectors));
    if (!vectors) return -E_NOMEM;
    int64_t total = 0, result = 0;
    uint64_t requested = 0;
    /* Snapshot the vector metadata before I/O; no partial transfer on a bad
     * vector array or a total length that cannot be represented by ssize_t. */
    if (!gmem_read(c->mem, iov, vectors, (uint64_t)count * sizeof(*vectors))) {
        result = -E_FAULT;
        goto done;
    }
    for (int32_t i = 0; i < count; i++) {
        if (vectors[i].len > (uint64_t)INT64_MAX - requested) {
            result = -E_INVAL;
            goto done;
        }
        requested += vectors[i].len;
    }
    for (int32_t i = 0; i < count; i++) {
        uint64_t base = vectors[i].base, len = vectors[i].len;
        if (offset >= 0 && (uint64_t)offset > (uint64_t)INT64_MAX - (uint64_t)total) {
            result = total ? total : -E_INVAL;
            goto done;
        }
        int64_t at = offset >= 0 ? offset + total : -1;
        int64_t r = write ? do_write(c, fd, base, len, at) : do_read(c, fd, base, len, at);
        if (r < 0) {
            result = total ? total : r;
            goto done;
        }
        total += r;
        if ((uint64_t)r < len) break;
    }
    result = total;
done:
    if (vectors != small) free(vectors);
    return result;
}

static void posix_readv(hle_call *c) {
    ret_posix(c, do_vector_io(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg_s32(c, 2), -1, false));
}
static void posix_preadv(hle_call *c) {
    int64_t offset = (int64_t)hle_arg(c, 3);
    ret_posix(c, offset < 0 ? -E_INVAL : do_vector_io(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg_s32(c, 2), offset, false));
}
static void sce_preadv(hle_call *c) {
    int64_t offset = (int64_t)hle_arg(c, 3);
    ret_sce(c, offset < 0 ? -E_INVAL : do_vector_io(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg_s32(c, 2), offset, false));
}
static void posix_pwritev(hle_call *c) {
    int64_t offset = (int64_t)hle_arg(c, 3);
    ret_posix(c, offset < 0 ? -E_INVAL : do_vector_io(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg_s32(c, 2), offset, true));
}
static void sce_pwritev(hle_call *c) {
    int64_t offset = (int64_t)hle_arg(c, 3);
    ret_sce(c, offset < 0 ? -E_INVAL : do_vector_io(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg_s32(c, 2), offset, true));
}

static int64_t do_lseek(int32_t fd, int64_t offset, int32_t whence) {
    fd_entry *e = fd_get(fd);
    if (!e) return -E_BADF;
    if (whence < 0 || whence > 2) return -E_INVAL;
    if (e->dev != DEV_NONE) return 0;
    if (e->pipe) return -E_SPIPE;
    if (e->is_dir) {
        /* directories: position in the entry list */
        int64_t base = whence == 0 ? 0 : whence == 1 ? e->entry_pos : e->entry_count;
        if (base + offset < 0) return -E_INVAL;
        e->entry_pos = (uint32_t)PS5C_MIN((uint64_t)(base + offset), e->entry_count);
        return e->entry_pos;
    }
    int64_t base = 0;
    if (whence == 1) base = (int64_t)__atomic_load_n(&e->pos, __ATOMIC_SEQ_CST);
    if (whence == 2) {
        uint64_t size = 0;
        if (pal_file_size(e->file, &size) != PS5C_OK) return -E_INVAL;
        base = (int64_t)size;
    }
    if (base + offset < 0) return -E_INVAL;
    __atomic_store_n(&e->pos, (uint64_t)(base + offset), __ATOMIC_SEQ_CST);
    return base + offset;
}

static int64_t do_ftruncate(int32_t fd, int64_t length) {
    fd_entry *e = fd_get(fd);
    if (!e || !e->file) return -E_BADF;
    if (length < 0) return -E_INVAL;
    ps5c_err err = pal_file_truncate(e->file, (uint64_t)length);
    return err == PS5C_OK ? 0 : err == PS5C_ERR_BUSY ? -E_ACCES : -E_IO;
}
static void posix_ftruncate(hle_call *c) { ret_posix(c, do_ftruncate(hle_arg_s32(c, 0), (int64_t)hle_arg(c, 1))); }
static void sce_ftruncate(hle_call *c) { ret_sce(c, do_ftruncate(hle_arg_s32(c, 0), (int64_t)hle_arg(c, 1))); }

/* truncate(path, length) */
static int64_t do_truncate(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    if ((int64_t)hle_arg(c, 1) < 0) return -E_INVAL;
    pal_file *f = NULL;
    if (pal_file_open(host, PAL_FILE_READ | PAL_FILE_WRITE, &f) != PS5C_OK) return -E_NOENT;
    ps5c_err pe = pal_file_truncate(f, hle_arg(c, 1));
    pal_file_close(f);
    return pe == PS5C_OK ? 0 : pe == PS5C_ERR_BUSY ? -E_ACCES : -E_IO;
}
static void sce_truncate(hle_call *c) { ret_sce(c, do_truncate(c)); }

/* chmod(path, mode), utimes(path, times): accepted for any existing path */
static int64_t do_path_exists(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    pal_path_info info = {0};
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err && guest_is_root(path)) return 0;
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    return pal_path_stat(host, &info) == PS5C_OK ? 0 : -E_NOENT;
}
static void posix_path_exists(hle_call *c) { ret_posix(c, do_path_exists(c)); }
static void sce_path_exists(hle_call *c) { ret_sce(c, do_path_exists(c)); }
static void sce_fchmod(hle_call *c) { ret_sce(c, fd_get(hle_arg_s32(c, 0)) ? 0 : -E_BADF); }
static void posix_fchmod(hle_call *c) {
    int32_t fd=hle_arg_s32(c,0);
    /* The virtual console exists without a host terminal. File permission
     * changes follow the same existence-only policy as chmod above. */
    ret_posix(c,(fd>=0 && fd<FIRST_FD) || fd_get(fd) ? 0 : -E_BADF);
}

static void posix_lseek(hle_call *c) {
    ret_posix(c, do_lseek(hle_arg_s32(c, 0), (int64_t)hle_arg(c, 1), hle_arg_s32(c, 2)));
}
static void sce_lseek(hle_call *c) {
    ret_sce(c, do_lseek(hle_arg_s32(c, 0), (int64_t)hle_arg(c, 1), hle_arg_s32(c, 2)));
}

/* ---- stat ---- */

/* FreeBSD struct stat (120 bytes) */
typedef struct guest_stat {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    int64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    int64_t birth_sec, birth_nsec;
} guest_stat;
PS5C_STATIC_ASSERT(sizeof(guest_stat) == 120, "FreeBSD struct stat");

static int64_t fill_stat(hle_call *c, guest_addr out_addr, const pal_path_info *info) {
    guest_stat *st = (guest_stat *)hle_mem(c, out_addr, sizeof(guest_stat));
    if (!st) return -E_FAULT;
    memset(st, 0, sizeof(*st));
    st->mode = (uint16_t)((info->is_dir ? S_IFDIR_ : S_IFREG_) | 0777);
    st->nlink = 1;
    st->size = (int64_t)info->size;
    st->blksize = 0x10000;
    st->blocks = (int64_t)((info->size + 511) / 512);
    st->atime_sec = st->mtime_sec = st->ctime_sec = st->birth_sec = info->mtime_sec;
    st->atime_nsec = st->mtime_nsec = st->ctime_nsec = st->birth_nsec = info->mtime_nsec;
    return 0;
}

/* struct stat of a host file (for other libkernel parts: APR) */
int64_t kernel_fs_stat_host(hle_call *c, const char *host, guest_addr out_addr);
int64_t kernel_fs_stat_host(hle_call *c, const char *host, guest_addr out_addr) {
    pal_path_info info;
    if (pal_path_stat(host, &info) != PS5C_OK) return -E_NOENT;
    return fill_stat(c, out_addr, &info);
}

static int64_t do_stat(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    pal_path_info info = {0};
    if (!err && guest_is_root(path)) {
        info.is_dir = true;
        return fill_stat(c, hle_arg_ptr(c, 1), &info);
    }
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    if (pal_path_stat(host, &info) != PS5C_OK) {
        LOG_D(KERNEL, "stat('%s'): not found", path);
        return -E_NOENT;
    }
    return fill_stat(c, hle_arg_ptr(c, 1), &info);
}

static int64_t do_fstat(hle_call *c) {
    int32_t fd = hle_arg_s32(c, 0);
    pal_path_info info = {0};
    if (fd >= 0 && fd < FIRST_FD) {
        return fill_stat(c, hle_arg_ptr(c, 1), &info);
    }
    fd_entry *e = fd_get(fd);
    if (!e) return -E_BADF;
    if (e->pipe) {
        int64_t result=fill_stat(c,hle_arg_ptr(c,1),&info);
        if (!result) { uint16_t mode=0010000|0600; if (!gmem_write(c->mem,hle_arg_ptr(c,1)+8,&mode,2)) result=-E_FAULT; }
        return result;
    }
    if (e->dev != DEV_NONE) return fill_stat(c, hle_arg_ptr(c, 1), &info);
    if (pal_path_stat(e->host_path, &info) != PS5C_OK) return -E_IO;
    if (e->file) pal_file_size(e->file, &info.size); /* includes unflushed writes */
    return fill_stat(c, hle_arg_ptr(c, 1), &info);
}

static void posix_stat(hle_call *c) { ret_posix(c, do_stat(c)); }
static void sce_stat(hle_call *c) { ret_sce(c, do_stat(c)); }
static void posix_fstat(hle_call *c) { ret_posix(c, do_fstat(c)); }
static void sce_fstat(hle_call *c) { ret_sce(c, do_fstat(c)); }

static int64_t do_check_reachability(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err && guest_is_root(path)) return 0;
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    pal_path_info info;
    return pal_path_stat(host, &info) == PS5C_OK ? 0 : -E_NOENT;
}

static void sce_check_reachability(hle_call *c) { ret_sce(c, do_check_reachability(c)); }

/* ---- Directories ---- */

static int64_t do_mkdir(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    ps5c_err pe = pal_mkdir(host);
    return pe == PS5C_OK ? 0 : pe == PS5C_ERR_BUSY ? -E_EXIST : pe == PS5C_ERR_NOT_FOUND ? -E_NOENT : -E_IO;
}

static void posix_mkdir(hle_call *c) { ret_posix(c, do_mkdir(c)); }
static void sce_mkdir(hle_call *c) { ret_sce(c, do_mkdir(c)); }

static int64_t do_unlink(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    return pal_file_remove(host) == PS5C_OK ? 0 : -E_NOENT;
}

/* int chdir(const char *path): an existing directory becomes the start of relative paths */
static int64_t do_chdir(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_], norm[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err && !path[0]) err = -E_NOENT;
    if (!err && guest_is_root(path)) { /* "/" holds the mount points, as stat and open see it */
        cwd_lock();
        memcpy(g_cwd, "/", 2);
        cwd_unlock();
        LOG_I(KERNEL, "chdir: working directory /");
        return 0;
    }
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    pal_path_info info = {0};
    if (pal_path_stat(host, &info) != PS5C_OK) return -E_NOENT;
    if (!info.is_dir) return -E_NOTDIR;
    int n = normalize_path(path, norm, sizeof(norm));
    if (n <= 0) return n < 0 ? n : -E_NOENT;
    cwd_lock();
    memcpy(g_cwd, norm, (size_t)n + 1);
    cwd_unlock();
    LOG_I(KERNEL, "chdir: working directory %s", norm);
    return 0;
}
static void posix_chdir(hle_call *c) { ret_posix(c, do_chdir(c)); }

/* char *getcwd(char *buf, size_t size) */
static void posix_getcwd(hle_call *c) {
    guest_addr buf = hle_arg_ptr(c, 0);
    uint64_t size = hle_arg(c, 1);
    char cwd[PATH_MAX_];
    cwd_lock();
    memcpy(cwd, g_cwd, sizeof(cwd));
    cwd_unlock();
    size_t len = strlen(cwd);
    if (!buf || !size) {
        set_errno(c, E_INVAL);
        hle_ret_u64(c, 0);
    } else if (len + 1 > size) {
        set_errno(c, 34); /* ERANGE */
        hle_ret_u64(c, 0);
    } else if (!gmem_write(c->mem, buf, cwd, len + 1)) {
        set_errno(c, E_FAULT);
        hle_ret_u64(c, 0);
    } else {
        hle_ret_u64(c, buf);
    }
}

static void posix_unlink(hle_call *c) { ret_posix(c, do_unlink(c)); }
static void sce_unlink(hle_call *c) { ret_sce(c, do_unlink(c)); }

/* rename(from, to) */
static int64_t do_rename(hle_call *c) {
    char from[PATH_MAX_], to[PATH_MAX_], hfrom[PATH_MAX_], hto[PATH_MAX_];
    int err = read_guest_path(c, 0, from, sizeof(from));
    if (!err) err = read_guest_path(c, 1, to, sizeof(to));
    if (!err) err = resolve_path(c, from, hfrom, sizeof(hfrom));
    if (!err) err = resolve_path(c, to, hto, sizeof(hto));
    if (err) return err;
    return pal_file_rename(hfrom, hto) == PS5C_OK ? 0 : -E_NOENT;
}

static void posix_rename(hle_call *c) { ret_posix(c, do_rename(c)); }
static void sce_rename(hle_call *c) { ret_sce(c, do_rename(c)); }

static int64_t do_rmdir(hle_call *c) {
    char path[PATH_MAX_], host[PATH_MAX_];
    int err = read_guest_path(c, 0, path, sizeof(path));
    if (!err) err = resolve_path(c, path, host, sizeof(host));
    if (err) return err;
    return pal_rmdir(host) == PS5C_OK ? 0 : -E_NOENT;
}

static void posix_rmdir(hle_call *c) { ret_posix(c, do_rmdir(c)); }
static void sce_rmdir(hle_call *c) { ret_sce(c, do_rmdir(c)); }
static void sce_sync(hle_call *c) { hle_ret_s32(c, 0); }

/* FreeBSD struct dirent { u32 fileno; u16 reclen; u8 type; u8 namlen; char name[]; } */
static int64_t do_getdents(hle_call *c, int32_t fd, guest_addr buf, uint64_t nbytes, guest_addr basep) {
    fd_entry *e = fd_get(fd);
    if (!e) return -E_BADF;
    if (!e->is_dir) return -E_NOTDIR;
    uint8_t *out = (uint8_t *)hle_mem(c, buf, nbytes);
    if (!out) return -E_FAULT;
    if (basep) gmem_write_u64(c->mem, basep, e->entry_pos);
    uint64_t used = 0;
    while (e->entry_pos < e->entry_count) {
        const dir_entry *d = &e->entries[e->entry_pos];
        size_t namlen = strlen(d->name);
        if (namlen > 255) namlen = 255;
        uint16_t reclen = (uint16_t)PS5C_ALIGN_UP(8 + namlen + 1, 8);
        if (used + reclen > nbytes) break;
        uint8_t *r = out + used;
        memset(r, 0, reclen);
        uint32_t fileno = e->entry_pos + 1;
        memcpy(r, &fileno, 4);
        memcpy(r + 4, &reclen, 2);
        r[6] = d->is_dir ? DT_DIR_ : DT_REG_;
        r[7] = (uint8_t)namlen;
        memcpy(r + 8, d->name, namlen);
        used += reclen;
        e->entry_pos++;
    }
    if (used == 0 && e->entry_pos < e->entry_count) return -E_INVAL; /* buffer too small */
    return (int64_t)used;
}

static void posix_getdents(hle_call *c) {
    ret_posix(c, do_getdents(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), 0));
}
static void sce_getdents(hle_call *c) {
    ret_sce(c, do_getdents(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), 0));
}
static void posix_getdirentries(hle_call *c) {
    ret_posix(c, do_getdents(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), hle_arg_ptr(c, 3)));
}
static void sce_getdirentries(hle_call *c) {
    ret_sce(c, do_getdents(c, hle_arg_s32(c, 0), hle_arg_ptr(c, 1), hle_arg(c, 2), hle_arg_ptr(c, 3)));
}

static void fsync_(hle_call *c) {
    int32_t fd = hle_arg_s32(c, 0);
    hle_ret_s32(c, fd_get(fd) || (fd >= 0 && fd < FIRST_FD) ? 0 : -1);
}

#define K "libkernel"
#define P "libScePosix"
#define POSIX_ENTRY(name, fn) HLE_FUNC_ENTRY(K, name, fn), HLE_FUNC_ENTRY(P, name, fn)

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(K, "__error", error_),
    POSIX_ENTRY("pipe", posix_pipe),
    POSIX_ENTRY("dup2", posix_dup2),
    POSIX_ENTRY("fcntl", posix_fcntl),
    POSIX_ENTRY("poll", posix_poll),
    POSIX_ENTRY("kqueue", posix_kqueue),
    POSIX_ENTRY("kevent", posix_kevent),
    POSIX_ENTRY("open", posix_open),
    POSIX_ENTRY("_open", posix_open),
    HLE_FUNC_ENTRY(K, "sceKernelOpen", sce_open),
    POSIX_ENTRY("close", posix_close),
    POSIX_ENTRY("_close", posix_close),
    HLE_FUNC_ENTRY(K, "sceKernelClose", sce_close),
    POSIX_ENTRY("read", posix_read),
    POSIX_ENTRY("_read", posix_read),
    HLE_FUNC_ENTRY(K, "sceKernelRead", sce_read),
    POSIX_ENTRY("pread", posix_pread),
    HLE_FUNC_ENTRY(K, "sceKernelPread", sce_pread),
    HLE_FUNC_ENTRY(K, "sceKernelAioInitializeImpl", aio_ok),
    HLE_FUNC_ENTRY(K, "sceKernelAioInitializeParam", aio_ok),
    HLE_FUNC_ENTRY(K, "sceKernelAioSetParam", aio_ok),
    HLE_FUNC_ENTRY(K, "sceKernelAioSubmitReadCommands", aio_submit_read),
    HLE_FUNC_ENTRY(K, "sceKernelAioSubmitReadCommandsMultiple", aio_submit_read_multiple),
    HLE_FUNC_ENTRY(K, "sceKernelAioSubmitWriteCommands", aio_submit_write),
    HLE_FUNC_ENTRY(K, "sceKernelAioSubmitWriteCommandsMultiple", aio_submit_write_multiple),
    HLE_FUNC_ENTRY(K, "sceKernelAioPollRequest", aio_state_of),
    HLE_FUNC_ENTRY(K, "sceKernelAioWaitRequest", aio_state_of),
    HLE_FUNC_ENTRY(K, "sceKernelAioCancelRequest", aio_state_of),
    HLE_FUNC_ENTRY(K, "sceKernelAioPollRequests", aio_states_of),
    HLE_FUNC_ENTRY(K, "sceKernelAioWaitRequests", aio_states_of),
    HLE_FUNC_ENTRY(K, "sceKernelAioCancelRequests", aio_states_of),
    HLE_FUNC_ENTRY(K, "sceKernelAioDeleteRequest", aio_delete),
    HLE_FUNC_ENTRY(K, "sceKernelAioDeleteRequests", aio_delete_many),
    POSIX_ENTRY("readv", posix_readv),
    POSIX_ENTRY("_readv", posix_readv),
    POSIX_ENTRY("preadv", posix_preadv),
    HLE_FUNC_ENTRY(K, "sceKernelPreadv", sce_preadv),
    POSIX_ENTRY("pwritev", posix_pwritev),
    HLE_FUNC_ENTRY(K, "sceKernelPwritev", sce_pwritev),
    POSIX_ENTRY("write", posix_write),
    POSIX_ENTRY("_write", posix_write),
    HLE_FUNC_ENTRY(K, "sceKernelWrite", sce_write),
    POSIX_ENTRY("pwrite", posix_pwrite),
    HLE_FUNC_ENTRY(K, "sceKernelPwrite", sce_pwrite),
    POSIX_ENTRY("lseek", posix_lseek),
    HLE_FUNC_ENTRY(K, "sceKernelLseek", sce_lseek),
    POSIX_ENTRY("ftruncate", posix_ftruncate),
    HLE_FUNC_ENTRY(K, "sceKernelFtruncate", sce_ftruncate),
    HLE_FUNC_ENTRY(K, "sceKernelTruncate", sce_truncate),
    HLE_FUNC_ENTRY(K, "sceKernelChmod", sce_path_exists),
    HLE_FUNC_ENTRY(K, "sceKernelFchmod", sce_fchmod),
    POSIX_ENTRY("fchmod", posix_fchmod),
    HLE_FUNC_ENTRY(K, "sceKernelUtimes", sce_path_exists),
    HLE_FUNC_ENTRY(P, "chmod", posix_path_exists),
    HLE_FUNC_ENTRY(K, "chmod", posix_path_exists), /* Prince of Persia imports it from libkernel */
    HLE_FUNC_ENTRY("libSceLibcInternal", "remove", posix_unlink), /* files; Prince of Persia */
    POSIX_ENTRY("stat", posix_stat),
    HLE_FUNC_ENTRY(K, "sceKernelStat", sce_stat),
    POSIX_ENTRY("fstat", posix_fstat),
    POSIX_ENTRY("_fstat", posix_fstat),
    HLE_FUNC_ENTRY(K, "sceKernelFstat", sce_fstat),
    HLE_FUNC_ENTRY(K, "sceKernelCheckReachability", sce_check_reachability),
    POSIX_ENTRY("mkdir", posix_mkdir),
    POSIX_ENTRY("chdir", posix_chdir),
    POSIX_ENTRY("getcwd", posix_getcwd),
    HLE_FUNC_ENTRY(K, "sceKernelMkdir", sce_mkdir),
    POSIX_ENTRY("unlink", posix_unlink),
    HLE_FUNC_ENTRY(K, "sceKernelUnlink", sce_unlink),
    POSIX_ENTRY("rename", posix_rename),
    HLE_FUNC_ENTRY(K, "sceKernelRename", sce_rename),
    POSIX_ENTRY("rmdir", posix_rmdir),
    HLE_FUNC_ENTRY(K, "sceKernelRmdir", sce_rmdir),
    HLE_FUNC_ENTRY(K, "sceKernelSync", sce_sync),
    POSIX_ENTRY("getdents", posix_getdents),
    HLE_FUNC_ENTRY(K, "sceKernelGetdents", sce_getdents),
    POSIX_ENTRY("getdirentries", posix_getdirentries),
    HLE_FUNC_ENTRY(K, "sceKernelGetdirentries", sce_getdirentries),
    POSIX_ENTRY("fsync", fsync_),
    HLE_FUNC_ENTRY(K, "sceKernelFsync", fsync_),
    HLE_FUNC_ENTRY(K, "sceKernelFdatasync", fsync_),
};

void hle_register_libkernel_fs(void) {
    if (!g_lock && pal_mutex_create(&g_lock) != PS5C_OK) {
        LOG_E(KERNEL, "cannot create the file table lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
