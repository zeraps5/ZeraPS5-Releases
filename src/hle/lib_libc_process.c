/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Firmware libc state for homebrew importing libSceLibcInternal. Heap and
 * stream handles are guest mappings; Windows FILE objects and pointers never
 * escape to the guest. Files use the existing guest VFS and descriptor table.
 * Streams are unbuffered, so fflush is synchronous and setvbuf may accept a
 * buffering request without retaining the caller's buffer (ISO C permits it).
 */
#include "core/runtime.h"
#include "hle/guest_printf.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hle_register_table(const hle_export *, size_t);
typedef struct allocation {
    guest_addr addr;
    uint64_t size, mapped;
    struct allocation *next;
} allocation;
typedef struct stream {
    guest_addr addr;
    int fd;
    bool eof, error;
    struct stream *next;
} stream;
typedef struct destructor {
    guest_addr fn, arg, dso;
} destructor;
typedef struct environment {
    char *name;
    guest_addr value;
    struct environment *next;
} environment;
struct libc_state {
    pal_mutex *lock;
    allocation *allocs;
    stream *streams;
    destructor *dtors;
    size_t dtor_count, dtor_capacity;
    guest_addr params;
    uint32_t random_seed;
    environment *env;
    guest_addr error_text;
};
static int64_t sys(hle_call *c, const char *name, uint64_t a, uint64_t b, uint64_t d);
static struct libc_state *state(ps5_runtime *rt) {
    pal_mutex_lock(rt->threads_lock);
    if (!rt->libc) {
        struct libc_state *s = calloc(1, sizeof(*s));
        if (s && pal_mutex_create(&s->lock) != PS5C_OK) {
            free(s);
            s = NULL;
        }
        rt->libc = s;
    }
    struct libc_state *s = rt->libc;
    pal_mutex_unlock(rt->threads_lock);
    return s;
}
void hle_libc_reset(ps5_runtime *rt) {
    struct libc_state *s = rt->libc;
    if (!s)
        return;
    /* runtime_free releases all guest VMAs after these host records. */
    while (s->allocs) {
        allocation *a = s->allocs;
        s->allocs = a->next;
        free(a);
    }
    hle_call c = {.rt = rt, .mem = &rt->mem};
    while (s->streams) {
        stream *f = s->streams;
        s->streams = f->next;
        if (f->fd >= 3)
            sys(&c, "close", (uint64_t)f->fd, 0, 0);
        free(f);
    }
    while (s->env) {
        environment *e = s->env;
        s->env = e->next;
        free(e->name);
        free(e);
    }
    free(s->dtors);
    pal_mutex_destroy(s->lock);
    free(s);
    rt->libc = NULL;
}
static void error(hle_call *c, int e) {
    if (c->thread && c->thread->errno_addr)
        gmem_write(c->mem, c->thread->errno_addr, &e, 4);
}
static int64_t sys(hle_call *c, const char *name, uint64_t a, uint64_t b, uint64_t d) {
    const hle_export *e = hle_find("libkernel", nid_hash_name(name));
    if (!e || !e->fn) {
        error(c, 78);
        return -1;
    }
    hle_args args = {.gpr = {a, b, d}};
    hle_call call = *c;
    call.a = &args;
    e->fn(&call);
    return (int64_t)args.ret[0];
}
static guest_addr allocate(hle_call *c, uint64_t size, uint64_t align) {
    struct libc_state *s = state(c->rt);
    guest_addr addr = 0;
    if (!s || size > UINT64_MAX - GUEST_PAGE_SIZE) {
        error(c, 12);
        return 0;
    }
    uint64_t mapped = PS5C_ALIGN_UP(size ? size : 1, GUEST_PAGE_SIZE);
    allocation *a = malloc(sizeof(*a));
    if (!a || gmem_map_aligned(c->mem, 0, mapped, align, GPROT_RW, 0, "HLE libc allocation",
                               &addr) != PS5C_OK) {
        free(a);
        error(c, 12);
        return 0;
    }
    *a = (allocation){addr, size, mapped, NULL};
    pal_mutex_lock(s->lock);
    a->next = s->allocs;
    s->allocs = a;
    pal_mutex_unlock(s->lock);
    return addr;
}
static void deallocate(hle_call *c, guest_addr addr) {
    if (!addr)
        return;
    struct libc_state *s = state(c->rt);
    if (!s)
        return;
    pal_mutex_lock(s->lock);
    allocation **p = &s->allocs;
    while (*p && (*p)->addr != addr)
        p = &(*p)->next;
    allocation *a = *p;
    if (a)
        *p = a->next;
    pal_mutex_unlock(s->lock);
    if (a) {
        gmem_unmap(c->mem, a->addr, a->mapped);
        free(a);
    }
}
static void malloc_(hle_call *c) {
    hle_ret_u64(c, allocate(c, hle_arg(c, 0), GUEST_PAGE_SIZE));
}
static void free_(hle_call *c) {
    deallocate(c, hle_arg_ptr(c, 0));
    hle_ret_u64(c, 0);
}
static char *guest_string(hle_call *c, guest_addr addr, uint64_t *size) {
    const hle_export *e = hle_find("libSceLibcInternal", nid_hash_name("strlen"));
    hle_args args = {.gpr = {addr}};
    hle_call length = *c;
    length.a = &args;
    e->fn(&length);
    *size = args.ret[0];
    return hle_mem(c, addr, *size + 1);
}
static void strdup_(hle_call *c) {
    uint64_t n;
    const char *s = guest_string(c, hle_arg_ptr(c, 0), &n);
    guest_addr a = s ? allocate(c, n + 1, GUEST_PAGE_SIZE) : 0;
    if (a)
        gmem_write(c->mem, a, s, n + 1);
    hle_ret_u64(c, a);
}
static void asprint(hle_call *c, bool list) {
    guest_addr out = hle_arg_ptr(c, 0), fmt = hle_arg_ptr(c, 1),
               va_list = list ? hle_arg_ptr(c, 2) : 0;
    uint8_t saved[24];
    if (!hle_mem(c, out, 8) || (list && !gmem_read(c->mem, va_list, saved, sizeof(saved)))) {
        error(c, 14);
        hle_ret_s32(c, -1);
        return;
    }
    guest_va va;
    if (list)
        guest_va_from_list(&va, c, va_list);
    else
        guest_va_from_call(&va, c, 2);
    size_t n = guest_format(c, fmt, &va, NULL, 0);
    if (list)
        gmem_write(c->mem, va_list, saved, sizeof(saved));
    guest_addr a = n < INT32_MAX ? allocate(c, (uint64_t)n + 1, GUEST_PAGE_SIZE) : 0;
    if (a) {
        if (list)
            guest_va_from_list(&va, c, va_list);
        else
            guest_va_from_call(&va, c, 2);
        guest_format(c, fmt, &va, hle_mem(c, a, (uint64_t)n + 1), n + 1);
    }
    gmem_write(c->mem, out, &a, 8);
    hle_ret_s32(c, a ? (int32_t)n : -1);
}
static void asprintf_(hle_call *c) {
    asprint(c, false);
}
static void vasprintf_(hle_call *c) {
    asprint(c, true);
}
static void getenv_(hle_call *c) {
    uint64_t n;
    const char *key = guest_string(c, hle_arg_ptr(c, 0), &n);
    struct libc_state *s = state(c->rt);
    guest_addr value = 0;
    if (key && s) {
        pal_mutex_lock(s->lock);
        for (environment *e = s->env; e; e = e->next)
            if (!strcmp(e->name, key)) {
                value = e->value;
                break;
            }
        pal_mutex_unlock(s->lock);
    }
    hle_ret_u64(c, value);
}
static void setenv_(hle_call *c) {
    uint64_t n, len;
    const char *key = guest_string(c, hle_arg_ptr(c, 0), &n),
               *value = guest_string(c, hle_arg_ptr(c, 1), &len);
    struct libc_state *s = state(c->rt);
    if (!s || !key || !value || !n || strchr(key, '=')) {
        error(c, 22);
        hle_ret_s32(c, -1);
        return;
    }
    char *name = malloc((size_t)n + 1);
    if (!name) {
        error(c, 12);
        hle_ret_s32(c, -1);
        return;
    }
    memcpy(name, key, (size_t)n + 1);
    guest_addr copy = allocate(c, len + 1, GUEST_PAGE_SIZE);
    if (!copy) {
        free(name);
        hle_ret_s32(c, -1);
        return;
    }
    gmem_write(c->mem, copy, value, len + 1);
    guest_addr old = 0;
    pal_mutex_lock(s->lock);
    environment *e = s->env;
    while (e && strcmp(e->name, name))
        e = e->next;
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (e) {
            e->name = name;
            name = NULL;
            e->next = s->env;
            s->env = e;
        }
    }
    if (e && (!e->value || hle_arg_s32(c, 2))) {
        old = e->value;
        e->value = copy;
        copy = 0;
    }
    pal_mutex_unlock(s->lock);
    free(name);
    deallocate(c, copy);
    deallocate(c, old);
    if (!e)
        error(c, 12);
    hle_ret_s32(c, e ? 0 : -1);
}
static void unsetenv_(hle_call *c) {
    uint64_t n;
    const char *key = guest_string(c, hle_arg_ptr(c, 0), &n);
    struct libc_state *s = state(c->rt);
    if (!s || !key || !n || strchr(key, '=')) {
        error(c, 22);
        hle_ret_s32(c, -1);
        return;
    }
    pal_mutex_lock(s->lock);
    environment **p = &s->env;
    while (*p && strcmp((*p)->name, key))
        p = &(*p)->next;
    environment *e = *p;
    if (e)
        *p = e->next;
    pal_mutex_unlock(s->lock);
    if (e) {
        deallocate(c, e->value);
        free(e->name);
        free(e);
    }
    hle_ret_s32(c, 0);
}
static void calloc_(hle_call *c) {
    uint64_t n = hle_arg(c, 0), size = hle_arg(c, 1);
    if (size && n > UINT64_MAX / size) {
        error(c, 12);
        hle_ret_u64(c, 0);
        return;
    }
    hle_ret_u64(c, allocate(c, n * size, GUEST_PAGE_SIZE));
}
static void realloc_(hle_call *c) {
    guest_addr old = hle_arg_ptr(c, 0);
    uint64_t n = hle_arg(c, 1), previous = 0;
    if (!old) {
        hle_ret_u64(c, allocate(c, n, GUEST_PAGE_SIZE));
        return;
    }
    if (!n) {
        deallocate(c, old);
        hle_ret_u64(c, 0);
        return;
    }
    struct libc_state *s = state(c->rt);
    bool found = false;
    if (s) {
        pal_mutex_lock(s->lock);
        for (allocation *a = s->allocs; a; a = a->next)
            if (a->addr == old) {
                previous = a->size;
                found = true;
                break;
            }
        pal_mutex_unlock(s->lock);
    }
    if (!found) {
        error(c, 22);
        hle_ret_u64(c, 0);
        return;
    }
    guest_addr a = allocate(c, n, GUEST_PAGE_SIZE);
    if (a) {
        uint64_t bytes = n < previous ? n : previous;
        void *d = hle_mem(c, a, bytes), *src = hle_mem(c, old, bytes);
        if (bytes && d && src)
            memcpy(d, src, (size_t)bytes);
        deallocate(c, old);
    }
    hle_ret_u64(c, a);
}
static void memalign_(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    uint64_t align = hle_arg(c, 1);
    if (align < 8 || (align & (align - 1)) || !hle_mem(c, out, 8)) {
        hle_ret_s32(c, 22);
        return;
    }
    guest_addr a = allocate(c, hle_arg(c, 2), align < GUEST_PAGE_SIZE ? GUEST_PAGE_SIZE : align);
    if (a)
        gmem_write(c->mem, out, &a, 8);
    hle_ret_s32(c, a ? 0 : 12);
}
static void init_env(hle_call *c) {
    struct libc_state *s = state(c->rt);
    if (s)
        s->params = hle_arg_ptr(c, 0);
    hle_ret_s32(c, s ? 0 : -1);
}
static void getprogname_(hle_call *c) {
    struct libc_state *s = state(c->rt);
    guest_addr a = 0;
    uint64_t n;
    if (s && s->params && gmem_read_u64(c->mem, s->params + 8, &a)) {
        const char *name = guest_string(c, a, &n);
        if (name) {
            const char *slash = strrchr(name, '/');
            if (slash)
                a += (uint64_t)(slash - name) + 1;
        }
    }
    hle_ret_u64(c, a);
}
static void gethostname_(hle_call *c) {
    static const char name[] = "ZeraPS5";
    uint64_t size = hle_arg(c, 1);
    if (size < sizeof(name)) {
        error(c, 63);
        hle_ret_s32(c, -1);
        return;
    }
    if (!gmem_write(c->mem, hle_arg_ptr(c, 0), name, sizeof(name))) {
        error(c, 14);
        hle_ret_s32(c, -1);
        return;
    }
    hle_ret_s32(c, 0);
}
static const char *error_message(int code) {
    switch (code) {
    case 0: return "No error";
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 9: return "Bad file descriptor";
    case 12: return "Cannot allocate memory";
    case 13: return "Permission denied";
    case 14: return "Bad address";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 28: return "No space left on device";
    case 29: return "Illegal seek";
    case 32: return "Broken pipe";
    case 35: return "Resource temporarily unavailable";
    case 45: return "Operation not supported";
    case 47: return "Address family not supported by protocol family";
    case 61: return "Connection refused";
    case 63: return "File name too long";
    default: return "Unknown error";
    }
}
static void strerror_(hle_call *c) {
    struct libc_state *s = state(c->rt);
    if (!s) {
        hle_ret_u64(c, 0);
        return;
    }
    pal_mutex_lock(s->lock);
    guest_addr a = s->error_text;
    pal_mutex_unlock(s->lock);
    if (!a) {
        guest_addr fresh = allocate(c, 128, GUEST_PAGE_SIZE);
        pal_mutex_lock(s->lock);
        if (!s->error_text) {
            s->error_text = fresh;
            fresh = 0;
        }
        a = s->error_text;
        pal_mutex_unlock(s->lock);
        deallocate(c, fresh);
    }
    const char *text = error_message(hle_arg_s32(c, 0));
    if (a)
        gmem_write(c->mem, a, text, strlen(text) + 1);
    hle_ret_u64(c, a);
}
static void getifaddrs_(hle_call *c) {
    if (!hle_mem(c, hle_arg_ptr(c, 0), 8)) {
        error(c, 14);
        hle_ret_s32(c, -1);
        return;
    }
    guest_addr a = allocate(c, 96, GUEST_PAGE_SIZE);
    if (!a) {
        hle_ret_s32(c, -1);
        return;
    }
    uint64_t fields[7] = {0, a + 88, 0x49, a + 56, a + 72, 0, 0};
    uint8_t address[16] = {16, 2, 0, 0, 127, 0, 0, 1}, mask[16] = {16, 2, 0, 0, 255, 0, 0, 0};
    gmem_write(c->mem, a, fields, sizeof(fields));
    gmem_write(c->mem, a + 56, address, 16);
    gmem_write(c->mem, a + 72, mask, 16);
    gmem_write(c->mem, a + 88, "lo0", 4);
    gmem_write(c->mem, hle_arg_ptr(c, 0), &a, 8);
    hle_ret_s32(c, 0);
}
static void add_destructor(hle_call *c, bool cxa) {
    struct libc_state *s = state(c->rt);
    int result = -1;
    if (s) {
        pal_mutex_lock(s->lock);
        if (s->dtor_count == s->dtor_capacity) {
            size_t cap = s->dtor_capacity ? s->dtor_capacity * 2 : 32;
            destructor *p = realloc(s->dtors, cap * sizeof(*p));
            if (p) {
                s->dtors = p;
                s->dtor_capacity = cap;
            }
        }
        if (s->dtor_count < s->dtor_capacity) {
            s->dtors[s->dtor_count++] = (destructor){hle_arg_ptr(c, 0), cxa ? hle_arg_ptr(c, 1) : 0,
                                                     cxa ? hle_arg_ptr(c, 2) : 0};
            result = 0;
        }
        pal_mutex_unlock(s->lock);
    }
    hle_ret_s32(c, result);
}
static void atexit_(hle_call *c) {
    add_destructor(c, false);
}
static void cxa_atexit(hle_call *c) {
    add_destructor(c, true);
}
static void finalize(hle_call *c, guest_addr dso) {
    struct libc_state *s = state(c->rt);
    if (!s)
        return;
    for (;;) {
        destructor d = {0};
        pal_mutex_lock(s->lock);
        for (size_t i = s->dtor_count; i; i--)
            if (s->dtors[i - 1].fn && (!dso || s->dtors[i - 1].dso == dso)) {
                d = s->dtors[i - 1];
                s->dtors[i - 1].fn = 0;
                break;
            }
        pal_mutex_unlock(s->lock);
        if (!d.fn)
            break;
        uint64_t ignored;
        if (!runtime_call_nested(c->rt, d.fn, d.arg, 0, 0, &ignored))
            break;
    }
}
static void cxa_finalize(hle_call *c) {
    finalize(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, 0);
}
static void exit_(hle_call *c) {
    int status = hle_arg_s32(c, 0);
    finalize(c, 0);
    runtime_exit_thread(c->rt, status);
}
static void abort_(hle_call *c) {
    runtime_exit_thread(c->rt, 134);
}

static guest_addr new_stream(ps5_runtime *rt, int fd) {
    struct libc_state *s = state(rt);
    if (!s)
        return 0;
    stream *f = calloc(1, sizeof(*f));
    if (!f)
        return 0;
    if (gmem_map(&rt->mem, 0, GUEST_PAGE_SIZE, GPROT_RW, 0, "HLE libc FILE", &f->addr) != PS5C_OK) {
        free(f);
        return 0;
    }
    f->fd = fd;
    pal_mutex_lock(s->lock);
    f->next = s->streams;
    s->streams = f;
    pal_mutex_unlock(s->lock);
    return f->addr;
}
static stream *get_stream(hle_call *c, guest_addr a) {
    struct libc_state *s = state(c->rt);
    if (!s)
        return NULL;
    pal_mutex_lock(s->lock);
    stream *f = s->streams;
    while (f && f->addr != a)
        f = f->next;
    pal_mutex_unlock(s->lock);
    if (!f)
        error(c, 9);
    return f;
}
static int mode_flags(hle_call *c, guest_addr a) {
    char mode[8] = {0};
    if (!hle_read_str(c, a, mode, sizeof(mode)))
        return -1;
    int flags;
    switch (mode[0]) {
    case 'r': flags = 0; break;
    case 'w': flags = 1 | 512 | 1024; break;
    case 'a': flags = 1 | 512 | 8; break;
    default: return -1;
    }
    if (strchr(mode, '+'))
        flags = (flags & ~3) | 2;
    return flags;
}
static void fopen_(hle_call *c) {
    int flags = mode_flags(c, hle_arg_ptr(c, 1));
    if (flags < 0) {
        error(c, 22);
        hle_ret_u64(c, 0);
        return;
    }
    int64_t fd = sys(c, "open", hle_arg_ptr(c, 0), (uint64_t)flags, 0666);
    guest_addr a = fd < 0 ? 0 : new_stream(c->rt, (int)fd);
    if (fd >= 0 && !a) {
        sys(c, "close", (uint64_t)fd, 0, 0);
        error(c, 12);
    }
    hle_ret_u64(c, a);
}
static void fdopen_(hle_call *c) {
    int fd = hle_arg_s32(c, 0);
    if (fd < 0 || mode_flags(c, hle_arg_ptr(c, 1)) < 0 || sys(c, "fcntl", (uint64_t)fd, 1, 0) < 0) {
        hle_ret_u64(c, 0);
        return;
    }
    hle_ret_u64(c, new_stream(c->rt, fd));
}
static void fclose_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    if (!f) {
        hle_ret_s32(c, -1);
        return;
    }
    int result = (int)sys(c, "close", (uint64_t)f->fd, 0, 0);
    struct libc_state *s = state(c->rt);
    pal_mutex_lock(s->lock);
    stream **p = &s->streams;
    while (*p && *p != f)
        p = &(*p)->next;
    if (*p)
        *p = f->next;
    pal_mutex_unlock(s->lock);
    gmem_unmap(c->mem, f->addr, GUEST_PAGE_SIZE);
    free(f);
    hle_ret_s32(c, result);
}
static void fileno_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, f ? f->fd : -1);
}
static void transfer(hle_call *c, bool write) {
    uint64_t size = hle_arg(c, 1), count = hle_arg(c, 2);
    stream *f = get_stream(c, hle_arg_ptr(c, 3));
    if (!f || !size || !count) {
        hle_ret_u64(c, 0);
        return;
    }
    if (count > UINT64_MAX / size) {
        error(c, 22);
        f->error = true;
        hle_ret_u64(c, 0);
        return;
    }
    uint64_t total = size * count, done = 0;
    while (done < total) {
        int64_t n = sys(c, write ? "write" : "read", (uint64_t)f->fd, hle_arg_ptr(c, 0) + done,
                        total - done);
        if (n <= 0) {
            if (n < 0)
                f->error = true;
            else if (!write)
                f->eof = true;
            break;
        }
        done += (uint64_t)n;
    }
    hle_ret_u64(c, done / size);
}
static void fread_(hle_call *c) {
    transfer(c, false);
}
static void fwrite_(hle_call *c) {
    transfer(c, true);
}
static void fseek_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    int64_t pos = f ? sys(c, "lseek", (uint64_t)f->fd, hle_arg(c, 1), hle_arg(c, 2)) : -1;
    if (pos >= 0)
        f->eof = false;
    hle_ret_s32(c, pos < 0 ? -1 : 0);
}
static void ftell_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    hle_ret_u64(c, f ? (uint64_t)sys(c, "lseek", (uint64_t)f->fd, 0, 1) : UINT64_MAX);
}
static void fflush_(hle_call *c) {
    hle_ret_s32(c, !hle_arg_ptr(c, 0) || get_stream(c, hle_arg_ptr(c, 0)) ? 0 : -1);
}
static void setvbuf_(hle_call *c) {
    int mode = hle_arg_s32(c, 2);
    hle_ret_s32(c, get_stream(c, hle_arg_ptr(c, 0)) && mode >= 0 && mode <= 2 ? 0 : -1);
}
static void ferror_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, f && f->error);
}
static void feof_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    hle_ret_s32(c, f && f->eof);
}
static void clearerr_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    if (f)
        f->eof = f->error = false;
    hle_ret_s32(c, 0);
}
int64_t hle_libc_write_text(hle_call *c, guest_addr file, const char *text, size_t n) {
    stream *f = get_stream(c, file);
    if (!f)
        return -1;
    if (!n)
        return 0;
    guest_addr temp = allocate(c, n, GUEST_PAGE_SIZE);
    if (!temp)
        return -1;
    gmem_write(c->mem, temp, text, n);
    size_t done = 0;
    while (done < n) {
        int64_t bytes = sys(c, "write", (uint64_t)f->fd, temp + done, n - done);
        if (bytes <= 0) {
            f->error = true;
            deallocate(c, temp);
            return -1;
        }
        done += (size_t)bytes;
    }
    deallocate(c, temp);
    return (int64_t)done;
}
static void fputc_(hle_call *c) {
    char ch = (char)hle_arg_u32(c, 0);
    hle_ret_s32(c, hle_libc_write_text(c, hle_arg_ptr(c, 1), &ch, 1) == 1 ? (unsigned char)ch : -1);
}
static void fputs_(hle_call *c) {
    const hle_export *e = hle_find("libSceLibcInternal", nid_hash_name("strlen"));
    hle_args args = {.gpr = {hle_arg_ptr(c, 0)}};
    hle_call length = *c;
    length.a = &args;
    e->fn(&length);
    size_t n = (size_t)args.ret[0];
    const char *s = hle_mem(c, hle_arg_ptr(c, 0), n + 1);
    hle_ret_s32(c, s && hle_libc_write_text(c, hle_arg_ptr(c, 1), s, n) >= 0 ? 0 : -1);
}
static void fgetc_(hle_call *c) {
    stream *f = get_stream(c, hle_arg_ptr(c, 0));
    guest_addr temp = f ? allocate(c, 1, GUEST_PAGE_SIZE) : 0;
    int result = -1;
    if (temp) {
        int64_t n = sys(c, "read", (uint64_t)f->fd, temp, 1);
        if (n == 1) {
            unsigned char ch;
            gmem_read(c->mem, temp, &ch, 1);
            result = ch;
        } else if (n == 0)
            f->eof = true;
        else
            f->error = true;
        deallocate(c, temp);
    }
    hle_ret_s32(c, result);
}
static void fgets_(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    int count = hle_arg_s32(c, 1);
    stream *f = get_stream(c, hle_arg_ptr(c, 2));
    char *dst = count > 0 ? hle_mem(c, out, (uint64_t)count) : NULL;
    int done = 0;
    if (!f || !dst) {
        hle_ret_u64(c, 0);
        return;
    }
    while (done < count - 1) {
        int64_t n = sys(c, "read", (uint64_t)f->fd, out + (uint64_t)done, 1);
        if (n <= 0) {
            if (n < 0)
                f->error = true;
            else
                f->eof = true;
            break;
        }
        if (dst[done++] == '\n')
            break;
    }
    dst[done] = 0;
    hle_ret_u64(c, done || count == 1 ? out : 0);
}
static void time_(hle_call *c) {
    int64_t seconds, nanos;
    if (!kernel_clock_now(0, &seconds, &nanos)) {
        error(c, 22);
        hle_ret_u64(c, UINT64_MAX);
        return;
    }
    if (hle_arg_ptr(c, 0) && !gmem_write(c->mem, hle_arg_ptr(c, 0), &seconds, 8)) {
        error(c, 14);
        hle_ret_u64(c, UINT64_MAX);
        return;
    }
    hle_ret_u64(c, (uint64_t)seconds);
}
static void srand_(hle_call *c) {
    struct libc_state *s = state(c->rt);
    if (s) {
        pal_mutex_lock(s->lock);
        s->random_seed = hle_arg_u32(c, 0);
        pal_mutex_unlock(s->lock);
    }
    hle_ret_s32(c, 0);
}
static void rand_(hle_call *c) {
    struct libc_state *s = state(c->rt);
    uint32_t value = 0;
    if (s) {
        pal_mutex_lock(s->lock);
        s->random_seed = s->random_seed * 1103515245u + 12345u;
        value = (s->random_seed >> 1) & 0x7fffffffu;
        pal_mutex_unlock(s->lock);
    }
    hle_ret_u32(c, value);
}
static bool compare(hle_call *c, guest_addr fn, guest_addr a, guest_addr b, int *result) {
    uint64_t value;
    if (!runtime_call_nested(c->rt, fn, a, b, 0, &value))
        return false;
    *result = (int32_t)value;
    return true;
}
static void swap_items(uint8_t *a, uint8_t *b, size_t size) {
    uint8_t tmp[256];
    while (size) {
        size_t n = size < sizeof(tmp) ? size : sizeof(tmp);
        memcpy(tmp, a, n);
        memcpy(a, b, n);
        memcpy(b, tmp, n);
        a += n;
        b += n;
        size -= n;
    }
}
static bool sift(hle_call *c, guest_addr base, uint8_t *bytes, uint64_t size, uint64_t root,
                 uint64_t count, guest_addr fn) {
    while (root < count / 2) {
        uint64_t child = root * 2 + 1;
        int order;
        if (child + 1 < count) {
            if (!compare(c, fn, base + child * size, base + (child + 1) * size, &order))
                return false;
            if (order < 0)
                child++;
        }
        if (!compare(c, fn, base + root * size, base + child * size, &order))
            return false;
        if (order >= 0)
            break;
        swap_items(bytes + root * size, bytes + child * size, (size_t)size);
        root = child;
    }
    return true;
}
static void qsort_(hle_call *c) {
    guest_addr base = hle_arg_ptr(c, 0), fn = hle_arg_ptr(c, 3);
    uint64_t n = hle_arg(c, 1), size = hle_arg(c, 2);
    hle_ret_s32(c, 0);
    if (n < 2 || !size)
        return;
    uint8_t *p = n <= UINT64_MAX / size ? hle_mem(c, base, n * size) : NULL;
    if (!p || !fn)
        return;
    for (uint64_t i = n / 2; i; i--)
        if (!sift(c, base, p, size, i - 1, n, fn))
            return;
    for (uint64_t end = n - 1; end; end--) {
        swap_items(p, p + end * size, (size_t)size);
        if (!sift(c, base, p, size, 0, end, fn))
            return;
    }
}
static void bsearch_(hle_call *c) {
    guest_addr key = hle_arg_ptr(c, 0), base = hle_arg_ptr(c, 1), fn = hle_arg_ptr(c, 4);
    uint64_t n = hle_arg(c, 2), size = hle_arg(c, 3);
    hle_ret_u64(c, 0);
    if (!size || n > UINT64_MAX / size || !hle_mem(c, base, n * size))
        return;
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int order;
        if (!compare(c, fn, key, base + mid * size, &order))
            return;
        if (!order) {
            hle_ret_u64(c, base + mid * size);
            return;
        }
        if (order < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
}
static void stdstream(ps5_runtime *rt, void *slot, size_t n, int fd) {
    guest_addr a = new_stream(rt, fd);
    if (n >= 8)
        memcpy(slot, &a, 8);
}
static void stdin_init(ps5_runtime *rt, void *slot, size_t n) {
    stdstream(rt, slot, n, 0);
}
static void stdout_init(ps5_runtime *rt, void *slot, size_t n) {
    stdstream(rt, slot, n, 1);
}
static void stderr_init(ps5_runtime *rt, void *slot, size_t n) {
    stdstream(rt, slot, n, 2);
}
#define E(name, fn) HLE_FUNC_ENTRY("libSceLibcInternal", name, fn)
static const hle_export exports[] = {
    E("_init_env", init_env),
    E("atexit", atexit_),
    E("__cxa_atexit", cxa_atexit),
    E("__cxa_finalize", cxa_finalize),
    E("exit", exit_),
    E("abort", abort_),
    E("getprogname", getprogname_),
    E("gethostname", gethostname_),
    E("getifaddrs", getifaddrs_),
    E("freeifaddrs", free_),
    E("strerror", strerror_),
    E("malloc", malloc_),
    E("calloc", calloc_),
    E("realloc", realloc_),
    E("free", free_),
    E("posix_memalign", memalign_),
    E("strdup", strdup_),
    E("getenv", getenv_),
    E("setenv", setenv_),
    E("unsetenv", unsetenv_),
    E("asprintf", asprintf_),
    E("vasprintf", vasprintf_),
    E("fopen", fopen_),
    E("fdopen", fdopen_),
    E("fclose", fclose_),
    E("fileno", fileno_),
    E("fread", fread_),
    E("fwrite", fwrite_),
    E("fseek", fseek_),
    E("ftell", ftell_),
    E("fflush", fflush_),
    E("setvbuf", setvbuf_),
    E("ferror", ferror_),
    E("feof", feof_),
    E("clearerr", clearerr_),
    E("time", time_),
    E("srand", srand_),
    E("rand", rand_),
    E("qsort", qsort_),
    E("bsearch", bsearch_),
    E("fputc", fputc_),
    E("fputs", fputs_),
    E("fgetc", fgetc_),
    E("fgets", fgets_),
    HLE_DATA_ENTRY("libSceLibcInternal", "__stdinp", stdin_init),
    HLE_DATA_ENTRY("libSceLibcInternal", "__stdoutp", stdout_init),
    HLE_DATA_ENTRY("libSceLibcInternal", "__stderrp", stderr_init),
};
void hle_register_libc_process(void) {
    hle_register_table(exports, PS5C_ARRAY_COUNT(exports));
}
