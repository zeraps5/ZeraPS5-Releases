/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pal/pal.h"
#include "pal/pal_exfat.h"

static uint64_t g_page_size = 4096;

void pal_utf8_args(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
}

ps5c_err pal_init(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) {
        g_page_size = (uint64_t)ps;
    }
    return exfat_init();
}

void pal_shutdown(void) { exfat_shutdown(); }

const char *pal_os_name(void) {
#if defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "POSIX";
#endif
}

/* ---------------------------------------------------------------------------
 * Virtual memory
 * ------------------------------------------------------------------------- */

static int posix_prot(uint32_t prot) {
    int p = PROT_NONE;
    if (prot & PAL_PROT_READ) p |= PROT_READ;
    if (prot & PAL_PROT_WRITE) p |= PROT_WRITE;
    if (prot & PAL_PROT_EXEC) p |= PROT_EXEC;
    return p;
}

uint64_t pal_page_size(void) { return g_page_size; }

uint64_t pal_alloc_granularity(void) { return g_page_size; }

ps5c_err pal_vm_reserve(void *want, uint64_t size, bool fixed, void **out) {
    if (size == 0 || (fixed && !want)) {
        return PS5C_ERR_INVALID;
    }
    size = PS5C_ALIGN_UP(size, g_page_size);

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
#ifdef MAP_FIXED_NOREPLACE
    if (fixed) {
        flags |= MAP_FIXED_NOREPLACE;
    }
#endif
    void *p = mmap(want, (size_t)size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) {
        return fixed ? PS5C_ERR_BUSY : PS5C_ERR_NOMEM;
    }
    if (fixed && p != want) {
        /* Kernel without MAP_FIXED_NOREPLACE treated the address as a hint. */
        munmap(p, (size_t)size);
        return PS5C_ERR_BUSY;
    }
    *out = p;
    return PS5C_OK;
}

ps5c_err pal_vm_commit(void *addr, uint64_t size, uint32_t prot) {
    return mprotect(addr, (size_t)size, posix_prot(prot)) == 0 ? PS5C_OK : PS5C_ERR_OS;
}

ps5c_err pal_vm_decommit(void *addr, uint64_t size) {
    /* Replace the pages with fresh inaccessible anonymous memory, dropping contents. */
    void *p = mmap(addr, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? PS5C_ERR_OS : PS5C_OK;
}

ps5c_err pal_vm_protect(void *addr, uint64_t size, uint32_t prot) {
    return mprotect(addr, (size_t)size, posix_prot(prot)) == 0 ? PS5C_OK : PS5C_ERR_OS;
}

ps5c_err pal_vm_release(void *addr, uint64_t size) {
    return munmap(addr, (size_t)PS5C_ALIGN_UP(size, g_page_size)) == 0 ? PS5C_OK : PS5C_ERR_OS;
}

ps5c_err pal_vm_release_range(void *addr, uint64_t size) { return pal_vm_release(addr, size); }

void pal_icache_flush(void *addr, uint64_t size) {
    /* x86-64 keeps instruction and data caches coherent; nothing to do. */
    PS5C_UNUSED(addr);
    PS5C_UNUSED(size);
}

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

uint64_t pal_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pal_sleep_ns(uint64_t ns) {
    if (ns == 0) {
        sched_yield();
        return;
    }
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ull);
    req.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------- */

uintptr_t pal_thread_open_current(void) { return 0; }
void pal_thread_close_handle(uintptr_t h) { (void)h; }
bool pal_thread_sample(uintptr_t h, uint64_t *pc, uint64_t *sp, uint64_t *fp) {
    (void)h, (void)pc, (void)sp, (void)fp;
    return false;
}

bool pal_thread_suspend(uintptr_t h) {
    (void)h;
    return false;
}
/* Portable fallback: short sleeps until the value changes. */
bool pal_wait_on_address(volatile void *addr, uint64_t expected, uint32_t size, uint64_t timeout_ns) {
    uint64_t deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : pal_time_ns() + timeout_ns;
    for (;;) {
        uint64_t cur = size == 8 ? *(volatile uint64_t *)addr : *(volatile uint32_t *)addr;
        if (cur != expected) return true;
        if (pal_time_ns() >= deadline) return false;
        pal_sleep_ns(100000);
    }
}

void pal_wake_on_address(volatile void *addr, int32_t count) {
    PS5C_UNUSED(addr);
    PS5C_UNUSED(count);
}

int pal_thread_suspend_count(uintptr_t h) {
    PS5C_UNUSED(h);
    return 0;
}

void pal_thread_resume(uintptr_t h) { (void)h; }
bool pal_thread_get_regs(uintptr_t h, pal_cpu_regs *out) {
    (void)h, (void)out;
    return false;
}

struct pal_thread {
    pthread_t handle;
    pal_thread_fn fn;
    void *arg;
    int32_t ret;
    char name[16];
};

static void *thread_entry(void *param) {
    pal_thread *t = (pal_thread *)param;
#if defined(__linux__)
    if (t->name[0]) {
        pthread_setname_np(pthread_self(), t->name);
    }
#elif defined(__APPLE__)
    if (t->name[0]) {
        pthread_setname_np(t->name);
    }
#endif
    t->ret = t->fn(t->arg);
    return NULL;
}

ps5c_err pal_thread_create(pal_thread **out, pal_thread_fn fn, void *arg, const char *name,
                           uint64_t stack_size) {
    pal_thread *t = (pal_thread *)calloc(1, sizeof(*t));
    if (!t) {
        return PS5C_ERR_NOMEM;
    }
    t->fn = fn;
    t->arg = arg;
    if (name) {
        /* Linux limits thread names to 15 characters. */
        snprintf(t->name, sizeof(t->name), "%s", name);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size) {
        pthread_attr_setstacksize(&attr, (size_t)PS5C_ALIGN_UP(stack_size, g_page_size));
    }
    int rc = pthread_create(&t->handle, &attr, thread_entry, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return PS5C_ERR_OS;
    }
    *out = t;
    return PS5C_OK;
}

ps5c_err pal_thread_join(pal_thread *t, int32_t *ret_out) {
    if (pthread_join(t->handle, NULL) != 0) {
        return PS5C_ERR_OS;
    }
    if (ret_out) {
        *ret_out = t->ret;
    }
    free(t);
    return PS5C_OK;
}

uint64_t pal_thread_current_id(void) { return (uint64_t)(uintptr_t)pthread_self(); }

void pal_thread_yield(void) { sched_yield(); }

struct pal_mutex {
    pthread_mutex_t m;
};

ps5c_err pal_mutex_create(pal_mutex **out) {
    pal_mutex *m = (pal_mutex *)malloc(sizeof(*m));
    if (!m) {
        return PS5C_ERR_NOMEM;
    }
    if (pthread_mutex_init(&m->m, NULL) != 0) {
        free(m);
        return PS5C_ERR_OS;
    }
    *out = m;
    return PS5C_OK;
}

void pal_mutex_destroy(pal_mutex *m) {
    if (m) {
        pthread_mutex_destroy(&m->m);
        free(m);
    }
}

void pal_mutex_lock(pal_mutex *m) { pthread_mutex_lock(&m->m); }
bool pal_mutex_trylock(pal_mutex *m) { return pthread_mutex_trylock(&m->m) == 0; }
void pal_mutex_unlock(pal_mutex *m) { pthread_mutex_unlock(&m->m); }

struct pal_cond {
    pthread_cond_t c;
};

ps5c_err pal_cond_create(pal_cond **out) {
    pal_cond *c = (pal_cond *)malloc(sizeof(*c));
    if (!c) {
        return PS5C_ERR_NOMEM;
    }
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    int rc = pthread_cond_init(&c->c, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        free(c);
        return PS5C_ERR_OS;
    }
    *out = c;
    return PS5C_OK;
}

void pal_cond_destroy(pal_cond *c) {
    if (c) {
        pthread_cond_destroy(&c->c);
        free(c);
    }
}

void pal_cond_wait(pal_cond *c, pal_mutex *m) { pthread_cond_wait(&c->c, &m->m); }

ps5c_err pal_cond_wait_ns(pal_cond *c, pal_mutex *m, uint64_t timeout_ns) {
    struct timespec ts;
#if defined(__APPLE__)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    uint64_t ns = (uint64_t)ts.tv_nsec + timeout_ns % 1000000000ull;
    ts.tv_sec += (time_t)(timeout_ns / 1000000000ull + ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    int rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
    if (rc == ETIMEDOUT) {
        return PS5C_ERR_TIMEOUT;
    }
    return rc == 0 ? PS5C_OK : PS5C_ERR_OS;
}

void pal_cond_signal(pal_cond *c) { pthread_cond_signal(&c->c); }
void pal_cond_broadcast(pal_cond *c) { pthread_cond_broadcast(&c->c); }

/* ---------------------------------------------------------------------------
 * Thread-local slots
 * ------------------------------------------------------------------------- */

ps5c_err pal_tls_alloc(uint32_t *slot_out) {
    pthread_key_t key;
    if (pthread_key_create(&key, NULL) != 0) {
        return PS5C_ERR_NOMEM;
    }
    *slot_out = (uint32_t)key;
    return PS5C_OK;
}

void pal_tls_free(uint32_t slot) { pthread_key_delete((pthread_key_t)slot); }
void pal_tls_set(uint32_t slot, void *value) { pthread_setspecific((pthread_key_t)slot, value); }
void *pal_tls_get(uint32_t slot) { return pthread_getspecific((pthread_key_t)slot); }

/* POSIX signals don't validate the stack pointer; nothing to track. */
void pal_thread_get_stack_bounds(pal_stack_bounds *out) { memset(out, 0, sizeof(*out)); }
void pal_thread_set_stack_bounds(const pal_stack_bounds *b) { PS5C_UNUSED(b); }
bool pal_thread_prefer_fast_cores(void) { return false; }
uint64_t pal_thread_cpu_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) ? 0 : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
bool pal_thread_raise_priority(void) { return false; }
bool pal_process_full_speed(void) { return false; }

uint32_t pal_tls_slot_gs_offset(uint32_t slot) {
    PS5C_UNUSED(slot);
    return 0; /* no directly addressable slot table; the native backend needs another scheme */
}

/* ---------------------------------------------------------------------------
 * Faults
 * ------------------------------------------------------------------------- */

#include <signal.h>
#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>
#endif

static pal_fault_fn g_fault_fn;
static void *g_fault_user;

static void signal_handler(int sig, siginfo_t *si, void *uctx) {
    pal_fault_info info;
    memset(&info, 0, sizeof(info));
    info.native_code = (uint32_t)sig;
    info.kind = sig == SIGSEGV || sig == SIGBUS ? PAL_FAULT_ACCESS
              : sig == SIGILL                   ? PAL_FAULT_ILLEGAL_INSTRUCTION
              : sig == SIGTRAP                  ? PAL_FAULT_BREAKPOINT
              : sig == SIGFPE                   ? PAL_FAULT_DIVIDE
                                                : PAL_FAULT_OTHER;
    info.addr = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t *uc = (ucontext_t *)uctx;
    greg_t *g = uc->uc_mcontext.gregs;
    info.pc = (uint64_t)g[REG_RIP];
    info.rsp = (uint64_t)g[REG_RSP];
    info.write = (g[REG_ERR] & 2) != 0;
    static const int order[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                  REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (int i = 0; i < 16; i++) info.gpr[i] = (uint64_t)g[order[i]];
    info.rflags = (uint64_t)g[REG_EFL];
    if (uc->uc_mcontext.fpregs) memcpy(info.xmm, uc->uc_mcontext.fpregs->_xmm, sizeof(info.xmm));
#else
    PS5C_UNUSED(uctx);
#endif
    if (g_fault_fn && g_fault_fn(g_fault_user, &info)) {
#if defined(__linux__) && defined(__x86_64__)
        g[REG_RIP] = (greg_t)info.pc;
        for (int i = 0; i < 16; i++)
            if (i != 4) g[order[i]] = (greg_t)info.gpr[i];
        g[REG_EFL] = (greg_t)info.rflags;
        if (uc->uc_mcontext.fpregs) memcpy(uc->uc_mcontext.fpregs->_xmm, info.xmm, sizeof(info.xmm));
#endif
        return;
    }
    signal(sig, SIG_DFL); /* crash with the default action */
}

ps5c_err pal_fault_handler_install(pal_fault_fn fn, void *user) {
    g_fault_fn = fn;
    g_fault_user = user;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(sigs); i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) return PS5C_ERR_OS;
    }
    return PS5C_OK;
}

void pal_exit_process(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

struct pal_file {
    exfat_file *image;
    int fd;
};

static ps5c_err map_errno(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR: return PS5C_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EBUSY: return PS5C_ERR_BUSY;
    case ENOMEM: return PS5C_ERR_NOMEM;
    case EINVAL: return PS5C_ERR_INVALID;
    default: return PS5C_ERR_IO;
    }
}

typedef struct native_file_view { int fd; } native_file_view;

void pal_file_view_close(pal_file_view *view) {
    if (!view) return;
    native_file_view *v = (native_file_view *)view->opaque;
    if (v) {
        if (view->data) munmap((void *)view->data, (size_t)view->size);
        close(v->fd);
        free(v);
    }
    memset(view, 0, sizeof(*view));
}

ps5c_err pal_file_view_open(const char *path, pal_file_view *out) {
    if (!path || !out) return PS5C_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (exfat_path(path)) return PS5C_ERR_UNSUPPORTED;
    int flags = O_RDONLY | O_NONBLOCK; /* reject non-regular files without waiting on a FIFO */
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) return map_errno(errno);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno; close(fd); return map_errno(e);
    }
    if (!S_ISREG(st.st_mode)) { close(fd); return PS5C_ERR_UNSUPPORTED; }
    if (st.st_size < 0 || (uint64_t)st.st_size > SIZE_MAX) {
        close(fd); return PS5C_ERR_OUT_OF_RANGE;
    }
    native_file_view *v = (native_file_view *)malloc(sizeof(*v));
    if (!v) { close(fd); return PS5C_ERR_NOMEM; }
    v->fd = fd;
    out->opaque = v;
    out->size = (uint64_t)st.st_size;
    if (!out->size) return PS5C_OK;
    void *data = mmap(NULL, (size_t)out->size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        int e = errno; pal_file_view_close(out); return map_errno(e);
    }
    out->data = data;
    return PS5C_OK;
}

ps5c_err pal_file_open(const char *path, uint32_t flags, pal_file **out) {
    if (exfat_path(path)) {
        pal_file *f = (pal_file *)calloc(1, sizeof(*f));
        if (!f) return PS5C_ERR_NOMEM;
        ps5c_err err = exfat_open(path, flags, &f->image);
        if (err) { free(f); return err; }
        *out = f; return PS5C_OK;
    }
    int oflags;
    if ((flags & PAL_FILE_READ) && (flags & PAL_FILE_WRITE)) oflags = O_RDWR;
    else if (flags & PAL_FILE_WRITE) oflags = O_WRONLY;
    else if (flags & PAL_FILE_READ) oflags = O_RDONLY;
    else return PS5C_ERR_INVALID;
    if (flags & PAL_FILE_CREATE) oflags |= O_CREAT;
    if (flags & PAL_FILE_TRUNCATE) oflags |= O_TRUNC;
#ifdef O_CLOEXEC
    oflags |= O_CLOEXEC;
#endif

    int fd = open(path, oflags, 0644);
    if (fd < 0) {
        return map_errno(errno);
    }
    pal_file *f = (pal_file *)malloc(sizeof(*f));
    if (!f) {
        close(fd);
        return PS5C_ERR_NOMEM;
    }
    f->image = NULL;
    f->fd = fd;
    *out = f;
    return PS5C_OK;
}

void pal_file_close(pal_file *f) {
    if (f && f->image) { exfat_close(f->image); free(f); return; }
    if (f) {
        close(f->fd);
        free(f);
    }
}

ps5c_err pal_file_pread(pal_file *f, void *buf, uint64_t size, uint64_t offset, uint64_t *read_out) {
    if (f->image) return exfat_read(f->image, buf, size, offset, read_out);
    uint64_t total = 0;
    while (total < size) {
        size_t want = (size_t)PS5C_MIN(size - total, (uint64_t)(1u << 30));
        ssize_t got = pread(f->fd, (uint8_t *)buf + total, want, (off_t)(offset + total));
        if (got < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        if (got == 0) break;
        total += (uint64_t)got;
    }
    if (read_out) *read_out = total;
    return PS5C_OK;
}

ps5c_err pal_file_pwrite(pal_file *f, const void *buf, uint64_t size, uint64_t offset, uint64_t *written_out) {
    if (f->image) { if (written_out) *written_out = 0; return PS5C_ERR_BUSY; }
    uint64_t total = 0;
    while (total < size) {
        size_t want = (size_t)PS5C_MIN(size - total, (uint64_t)(1u << 30));
        ssize_t put = pwrite(f->fd, (const uint8_t *)buf + total, want, (off_t)(offset + total));
        if (put < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        if (put == 0) break;
        total += (uint64_t)put;
    }
    if (written_out) *written_out = total;
    return PS5C_OK;
}

/* The kernel's writes into guest buffers (read) return EFAULT on a read-only page. */
static void (*g_write_prepare)(void *buf, uint64_t size);
void pal_set_write_prepare(void (*fn)(void *buf, uint64_t size)) { g_write_prepare = fn; }

ps5c_err pal_file_read(pal_file *f, void *buf, uint64_t size, uint64_t *read_out) {
    if (f->image) return exfat_read_current(f->image, buf, size, read_out);
    uint64_t total = 0;
    while (total < size) {
        size_t want = (size_t)PS5C_MIN(size - total, (uint64_t)(1u << 30));
        if (g_write_prepare) g_write_prepare((uint8_t *)buf + total, want);
        ssize_t got = read(f->fd, (uint8_t *)buf + total, want);
        if (got < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        if (got == 0) {
            break;
        }
        total += (uint64_t)got;
    }
    if (read_out) {
        *read_out = total;
    }
    return PS5C_OK;
}

ps5c_err pal_file_write(pal_file *f, const void *buf, uint64_t size, uint64_t *written_out) {
    if (f->image) { if (written_out) *written_out = 0; return PS5C_ERR_BUSY; }
    uint64_t total = 0;
    while (total < size) {
        size_t want = (size_t)PS5C_MIN(size - total, (uint64_t)(1u << 30));
        ssize_t put = write(f->fd, (const uint8_t *)buf + total, want);
        if (put < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        total += (uint64_t)put;
    }
    if (written_out) {
        *written_out = total;
    }
    return PS5C_OK;
}

ps5c_err pal_file_seek(pal_file *f, int64_t offset, int whence, uint64_t *pos_out) {
    if (f->image) return exfat_seek(f->image, offset, whence, pos_out);
    int w = whence == PAL_SEEK_CUR ? SEEK_CUR : whence == PAL_SEEK_END ? SEEK_END : SEEK_SET;
    off_t pos = lseek(f->fd, (off_t)offset, w);
    if (pos < 0) {
        return map_errno(errno);
    }
    if (pos_out) {
        *pos_out = (uint64_t)pos;
    }
    return PS5C_OK;
}

ps5c_err pal_file_size(pal_file *f, uint64_t *size_out) {
    if (f->image) { *size_out = exfat_size(f->image); return PS5C_OK; }
    struct stat st;
    if (fstat(f->fd, &st) != 0) {
        return map_errno(errno);
    }
    *size_out = (uint64_t)st.st_size;
    return PS5C_OK;
}

ps5c_err pal_file_truncate(pal_file *f, uint64_t size) {
    if (f->image) return PS5C_ERR_BUSY;
    return ftruncate(f->fd, (off_t)size) == 0 ? PS5C_OK : map_errno(errno);
}

ps5c_err pal_rmdir(const char *path) {
    if (exfat_path(path)) return PS5C_ERR_BUSY;
    return rmdir(path) == 0 ? PS5C_OK : map_errno(errno);
}

ps5c_err pal_file_remove(const char *path) {
    if (exfat_path(path)) return PS5C_ERR_BUSY;
    return unlink(path) == 0 ? PS5C_OK : map_errno(errno);
}
ps5c_err pal_file_rename(const char *from, const char *to) {
    if (exfat_path(from) || exfat_path(to)) return PS5C_ERR_BUSY;
    return rename(from, to) == 0 ? PS5C_OK : map_errno(errno);
}

/* No windowing on POSIX hosts yet. */
ps5c_err pal_window_open(const char *title, uint32_t width, uint32_t height, pal_window **out) {
    PS5C_UNUSED(title);
    PS5C_UNUSED(width);
    PS5C_UNUSED(height);
    *out = NULL;
    return PS5C_ERR_UNSUPPORTED;
}
void pal_window_present(pal_window *w, const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes) {
    PS5C_UNUSED(w);
    PS5C_UNUSED(pixels);
    PS5C_UNUSED(width);
    PS5C_UNUSED(height);
    PS5C_UNUSED(pitch_bytes);
}
void pal_window_native(pal_window *w, void **display, void **handle) {
    PS5C_UNUSED(w);
    *display = NULL;
    *handle = NULL;
}
void pal_window_client_size(pal_window *w, uint32_t *width, uint32_t *height) {
    PS5C_UNUSED(w);
    *width = *height = 0;
}
size_t pal_display_list(char *out, size_t cap) {
    if (cap) out[0] = 0;
    return 0;
}
int pal_window_move_to_gpu(pal_window *w, const uint8_t luid[8]) {
    PS5C_UNUSED(w);
    PS5C_UNUSED(luid);
    return -1;
}
void pal_window_set_external_present(pal_window *w, bool on) {
    PS5C_UNUSED(w);
    PS5C_UNUSED(on);
}
void pal_window_set_mode(pal_window *w, int mode) {
    PS5C_UNUSED(w);
    PS5C_UNUSED(mode);
}
pal_library *pal_library_open(const char *name) { return (pal_library *)dlopen(name, RTLD_NOW); }
pal_proc pal_library_symbol(pal_library *lib, const char *name) {
    void *p = lib ? dlsym(lib, name) : NULL;
    pal_proc f;
    memcpy(&f, &p, sizeof(f)); /* POSIX: dlsym results are callable */
    return f;
}

void pal_window_set_title(pal_window *w, const char *title) {
    PS5C_UNUSED(w);
    PS5C_UNUSED(title);
}
bool pal_window_closed(pal_window *w) {
    PS5C_UNUSED(w);
    return false;
}
void pal_window_destroy(pal_window *w) { PS5C_UNUSED(w); }

/* No host input on POSIX hosts yet: a controller at rest. */
bool pal_input_panel_toggled(void) { return false; }
bool pal_input_has_controller(void) { return false; }

bool pal_input_read_player(uint32_t player, pal_pad_state *out) {
    PS5C_UNUSED(player);
    PS5C_UNUSED(out);
    return false;
}

void pal_input_read(pal_pad_state *out) {
    memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;
}
void pal_input_set_rumble(uint8_t large, uint8_t small) {
    PS5C_UNUSED(large);
    PS5C_UNUSED(small);
}
void pal_input_set_light(uint8_t r, uint8_t g, uint8_t b) {
    PS5C_UNUSED(r);
    PS5C_UNUSED(g);
    PS5C_UNUSED(b);
}
const char *pal_input_device(void) { return ""; }

ps5c_err pal_path_stat(const char *path, pal_path_info *out) {
    if (exfat_path(path)) return exfat_stat(path, out);
    struct stat st;
    if (stat(path, &st) != 0) return map_errno(errno);
    out->is_dir = S_ISDIR(st.st_mode);
    out->size = out->is_dir ? 0 : (uint64_t)st.st_size;
    out->mtime_sec = (int64_t)st.st_mtim.tv_sec;
    out->mtime_nsec = (uint32_t)st.st_mtim.tv_nsec;
    return PS5C_OK;
}

ps5c_err pal_mkdir(const char *path) {
    if (exfat_path(path)) return PS5C_ERR_BUSY;
    if (mkdir(path, 0777) == 0) return PS5C_OK;
    return errno == EEXIST ? PS5C_ERR_BUSY : map_errno(errno);
}

ps5c_err pal_dir_list(const char *path, pal_dir_fn fn, void *user) {
    if (exfat_path(path)) return exfat_list(path, fn, user);
    DIR *d = opendir(path);
    if (!d) return map_errno(errno);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        bool is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (!fn(user, e->d_name, is_dir)) break;
    }
    closedir(d);
    return PS5C_OK;
}

/* ---------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

void pal_log_sink(void *user, log_level level, const char *line, size_t len) {
    PS5C_UNUSED(user);
    PS5C_UNUSED(level);
    fwrite(line, 1, len, stderr);
    fflush(stderr);
}

/* No host sound on POSIX hosts yet. */
ps5c_err pal_audio_start(pal_audio_fill_fn fill, void *user) {
    PS5C_UNUSED(fill);
    PS5C_UNUSED(user);
    return PS5C_ERR_UNSUPPORTED;
}
void pal_audio_stop(void) {}


/* Diagnostics: the host state of a page (not available here). */
void pal_log_host_page(uint64_t addr) { (void)addr; }
bool pal_host_module(uint64_t addr, char *name, size_t cap, uint64_t *base) {
    (void)addr, (void)name, (void)cap, (void)base;
    return false;
}

/* No copy-on-write tracking here: gpu_watch keeps to write faults (sigaltstack spares the red zone). */
bool pal_vm_cow_supported(void) { return false; }
bool pal_vm_cow_arm(void *addr, uint64_t size) {
    (void)addr, (void)size;
    return false;
}
bool pal_vm_page_private(void *addr) {
    (void)addr;
    return false;
}
void pal_vm_private_pages(void *addr, uint32_t pages, uint8_t *out) {
    (void)addr;
    memset(out, 0, pages);
}
bool pal_vm_cow_clean(void *addr, uint64_t size, uint64_t *off, uint64_t *len, bool *is_private) {
    (void)addr;
    *off = 0, *len = size, *is_private = false;
    return false;
}
