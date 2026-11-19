/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Platform Abstraction Layer: host operating-system services with no PS5
 * concepts in them. Everything that knows about NIDs, guest addresses or PM4
 * belongs to the core; everything that calls the host OS belongs here.
 *
 * Implementations: pal_win32/ (Windows), pal_posix/ (Linux, other Unix).
 * Window, audio and input are not part of the PAL: they are frontend backends. */
#ifndef PS5C_PAL_H
#define PS5C_PAL_H

#include "base/base.h"
#include "base/log.h"

ps5c_err pal_init(void);
/* The command line as UTF-8 (Windows passes main() the ANSI code page, which
 * mangles paths like "God of War – PPSA28997"); unchanged elsewhere. */
void pal_utf8_args(int *argc, char ***argv);
void pal_shutdown(void);
const char *pal_os_name(void);

/* Host diagnostics. Names are stable; counters are sampled by the caller (not
 * per draw). Memory is in bytes, cumulative CPU times in nanoseconds. Missing
 * counters have their valid bit clear rather than pretending to report zero. */
typedef struct pal_host_info {
    char cpu[96], os[96];
    uint32_t logical_processors, physical_cores;
} pal_host_info;
enum {
    PAL_HOST_RAM = 1u, PAL_HOST_COMMIT = 2u, PAL_HOST_PROCESS_RAM = 4u,
    PAL_HOST_PROCESS_CPU = 8u, PAL_HOST_SYSTEM_CPU = 16u, PAL_HOST_PROCESS_COMMIT = 32u,
};
typedef struct pal_host_stats {
    uint32_t valid;
    uint64_t ram_total, ram_available, commit_total, commit_limit;
    uint64_t process_ram, process_commit;
    uint64_t process_cpu_ns, system_cpu_ns, system_idle_ns;
} pal_host_stats;
void pal_host_get_info(pal_host_info *out);
void pal_host_get_stats(pal_host_stats *out);

/* ---- Virtual memory ----
 * Reserve address space first, then commit parts of it. A reservation must be
 * released as a whole, with the same base and size it was reserved with. */
enum {
    PAL_PROT_NONE = 0,
    PAL_PROT_READ = 1,
    PAL_PROT_WRITE = 2,
    PAL_PROT_EXEC = 4,
    PAL_PROT_RW = PAL_PROT_READ | PAL_PROT_WRITE,
    PAL_PROT_RX = PAL_PROT_READ | PAL_PROT_EXEC,
    PAL_PROT_RWX = PAL_PROT_READ | PAL_PROT_WRITE | PAL_PROT_EXEC,
};

uint64_t pal_page_size(void);
uint64_t pal_alloc_granularity(void); /* alignment of reservations (64 KiB on Windows) */

/* `fixed`: fail with PS5C_ERR_BUSY unless the range is reserved exactly at `want`. */
ps5c_err pal_vm_reserve(void *want, uint64_t size, bool fixed, void **out);
ps5c_err pal_vm_commit(void *addr, uint64_t size, uint32_t prot);
ps5c_err pal_vm_decommit(void *addr, uint64_t size);
ps5c_err pal_vm_protect(void *addr, uint64_t size, uint32_t prot);
ps5c_err pal_vm_release(void *addr, uint64_t size);
/* Releases everything this process has in [addr, addr + size): reservations,
 * memory placed in placeholders, placeholders and shared-memory views. */
ps5c_err pal_vm_release_range(void *addr, uint64_t size);
void pal_icache_flush(void *addr, uint64_t size);

/* Placeholder address space (Windows 10 1803+): a reserved range where memory
 * is placed piece by piece, including several views of one shared-memory
 * object (one physical page at many addresses). pal_ph_supported() is false
 * where the host cannot do this. Ranges are page aligned; a range mapped or
 * split must lie inside one placeholder. */
bool pal_ph_supported(void);
ps5c_err pal_ph_reserve(void *addr, uint64_t size);
ps5c_err pal_ph_split(void *addr, uint64_t size);    /* carves [addr, addr + size) out of its placeholder */
ps5c_err pal_ph_coalesce(void *addr, uint64_t size); /* merges the adjacent placeholders covering the range */
ps5c_err pal_ph_alloc(void *addr, uint64_t size, uint32_t prot);    /* private memory into a placeholder */
ps5c_err pal_ph_free(void *addr, uint64_t size);                     /* private memory back to a placeholder */
/* demand: leave unused section pages reserved; protect already committed runs. */
ps5c_err pal_ph_map_view(void *shm, uint64_t offset, void *addr, uint64_t size, uint32_t prot, bool demand);
ps5c_err pal_ph_unmap_view(void *addr);                              /* a whole view back to a placeholder */
/* Shared memory object of `size` bytes, committed page by page as views use it. */
ps5c_err pal_shm_create(uint64_t size, void **out);
void pal_shm_destroy(void *shm); /* after its views are gone */
/* One more view of the whole object, for the host (its pages are committed
 * with pal_vm_commit, by this view or by any other). */
ps5c_err pal_shm_map(void *shm, uint64_t size, void **out);
/* Whether the page at addr is committed; *run: bytes from addr in the same state. */
bool pal_vm_committed(void *addr, uint64_t *run);
/* Copy-on-write arming of pages of a pal_ph_map_view view, for write tracking without faults
 * (gpu_watch.h). An armed page the process writes (CPU instruction or system call) becomes a
 * private read-write copy with no exception raised: other views of the object stop seeing
 * its writes, and it cannot be armed again. */
bool pal_vm_cow_supported(void);
/* Arms [addr, addr + size); pages that are private already stay as they are. */
bool pal_vm_cow_arm(void *addr, uint64_t size);
/* True when every page of the range is armed and unwritten. Otherwise [*off, *off + *len)
 * from addr is the first run of pages that are not (clipped to the range), all private copies
 * (*is_private) or none of them. */
bool pal_vm_cow_clean(void *addr, uint64_t size, uint64_t *off, uint64_t *len, bool *is_private);
/* The readable page at addr is a private copy (no longer the object's page). */
bool pal_vm_page_private(void *addr);
/* pal_vm_page_private for `pages` pages from addr (one system query per 256 pages): out[i] = 1 when
 * page i is a private copy or unknown. The pages must be readable. */
void pal_vm_private_pages(void *addr, uint32_t pages, uint8_t *out);
void pal_log_host_page(uint64_t addr); /* logs the host state of a page (diagnostics) */
/* The host module (DLL) holding addr: its file name and base; false if none. */
bool pal_host_module(uint64_t addr, char *name, size_t cap, uint64_t *base);

/* ---- Time ---- */
uint64_t pal_time_ns(void); /* monotonic, arbitrary origin */
void pal_sleep_ns(uint64_t ns);

/* ---- Threads and synchronization ---- */
typedef struct pal_thread pal_thread;
typedef struct pal_mutex pal_mutex;
typedef struct pal_cond pal_cond;
typedef int32_t (*pal_thread_fn)(void *arg);

/* `stack_size` 0 = host default. `name` may be NULL. */
ps5c_err pal_thread_create(pal_thread **out, pal_thread_fn fn, void *arg, const char *name,
                           uint64_t stack_size);
ps5c_err pal_thread_join(pal_thread *t, int32_t *ret_out); /* also frees `t` */
uint64_t pal_thread_current_id(void);
void pal_thread_yield(void);
/* Keeps the calling thread on the fastest cores of a hybrid CPU (P-cores);
 * false when the CPU has one kind of core or the host cannot tell. */
bool pal_thread_prefer_fast_cores(void);

/* Waits while the 4- or 8-byte value at `addr` equals `expected`, up to
 * `timeout_ns` (UINT64_MAX: no limit). True when woken or the value differs,
 * false on timeout. Wakes may be spurious: callers recheck. */
bool pal_wait_on_address(volatile void *addr, uint64_t expected, uint32_t size, uint64_t timeout_ns);
/* Wakes up to `count` threads waiting on `addr` (count < 0: all). */
void pal_wake_on_address(volatile void *addr, int32_t count);

/* Profiling: a handle to the calling thread that other threads can sample.
 * pal_thread_sample briefly suspends the thread and reads pc/sp/fp. */
uintptr_t pal_thread_open_current(void);
void pal_thread_close_handle(uintptr_t h);
bool pal_thread_sample(uintptr_t h, uint64_t *pc, uint64_t *sp, uint64_t *fp);

/* Asynchronous exceptions: freeze a thread (handle from pal_thread_open_current)
 * and read its registers. Suspends nest; each needs a resume. */
typedef struct pal_cpu_regs {
    uint64_t gpr[16]; /* rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15 */
    uint64_t rip, rflags;
    uint8_t fxsave[512];
} pal_cpu_regs;
bool pal_thread_suspend(uintptr_t h);
void pal_thread_resume(uintptr_t h);
/* Debugging aid: the thread's current suspension count (-1 on error). */
int pal_thread_suspend_count(uintptr_t h);
/* Registers of a suspended thread. */
bool pal_thread_get_regs(uintptr_t h, pal_cpu_regs *out);

ps5c_err pal_mutex_create(pal_mutex **out);
void pal_mutex_destroy(pal_mutex *m);
void pal_mutex_lock(pal_mutex *m);
bool pal_mutex_trylock(pal_mutex *m);
void pal_mutex_unlock(pal_mutex *m);

ps5c_err pal_cond_create(pal_cond **out);
void pal_cond_destroy(pal_cond *c);
void pal_cond_wait(pal_cond *c, pal_mutex *m);
/* Returns PS5C_ERR_TIMEOUT when the timeout expires. */
ps5c_err pal_cond_wait_ns(pal_cond *c, pal_mutex *m, uint64_t timeout_ns);
void pal_cond_signal(pal_cond *c);
void pal_cond_broadcast(pal_cond *c);

/* ---- Thread-local slots ----
 * Host TLS slots. On Windows the slot value is also readable by generated
 * code at gs:[pal_tls_slot_gs_offset(slot)] (TEB TlsSlots, slots < 64). */
ps5c_err pal_tls_alloc(uint32_t *slot_out);
void pal_tls_free(uint32_t slot);
void pal_tls_set(uint32_t slot, void *value);
void *pal_tls_get(uint32_t slot);
/* 0 if the slot can't be read directly from generated code on this host. */
uint32_t pal_tls_slot_gs_offset(uint32_t slot);

/* Stack range the host OS believes the current thread runs on. Must cover the
 * guest stack while guest code runs (Windows validates rsp against it when
 * dispatching exceptions and reports STATUS_STACK_OVERFLOW otherwise). */
typedef struct pal_stack_bounds {
    uint64_t low, high, dealloc;
} pal_stack_bounds;
void pal_thread_get_stack_bounds(pal_stack_bounds *out);
void pal_thread_set_stack_bounds(const pal_stack_bounds *b);

/* ---- Shared libraries ---- */
typedef struct pal_library pal_library;
pal_library *pal_library_open(const char *name); /* NULL if not found */
typedef void (*pal_proc)(void);
pal_proc pal_library_symbol(pal_library *lib, const char *name);

/* ---- Faults (x86-64 hosts) ---- */
typedef enum {
    PAL_FAULT_ACCESS,
    PAL_FAULT_ILLEGAL_INSTRUCTION,
    PAL_FAULT_BREAKPOINT,
    PAL_FAULT_DIVIDE,
    PAL_FAULT_OTHER,
    PAL_FAULT_SINGLE_STEP /* after an instruction run with single_step (Windows only) */
} pal_fault_kind;

typedef struct pal_fault_info {
    pal_fault_kind kind;
    uint32_t native_code; /* exception code / signal number */
    uint64_t pc;
    uint64_t rsp;
    uint64_t addr; /* faulting data address for PAL_FAULT_ACCESS */
    bool write;
    bool execute; /* access violation while fetching an instruction */
    uint64_t gpr[16]; /* rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15 */
    uint64_t xmm[16][2]; /* low, high quadwords; written back when the handler resumes */
    uint64_t rflags;     /* written back with gpr (rsp excepted) when the handler resumes */
    uint64_t ymm_hi[16][2]; /* upper halves of ymm0-15, when has_ymm_hi; written back */
    bool has_ymm_hi;
    bool single_step; /* set by the handler: run one instruction, then a PAL_FAULT_SINGLE_STEP */
} pal_fault_info;

/* Called on the faulting thread. Return true to resume execution at info->pc
 * (the handler may change it), false to let the fault continue (crash). */
typedef bool (*pal_fault_fn)(void *user, pal_fault_info *info);
ps5c_err pal_fault_handler_install(pal_fault_fn fn, void *user);
/* Terminates the process immediately (usable from a fault handler). */
void pal_exit_process(int code);

/* ---- Files (paths are UTF-8) ---- */
typedef struct pal_file pal_file;

enum {
    PAL_FILE_READ = 1,
    PAL_FILE_WRITE = 2,
    PAL_FILE_CREATE = 4,   /* create if missing */
    PAL_FILE_TRUNCATE = 8, /* truncate existing contents */
    /* Optional immutable-asset recovery on platforms supporting it. Only
     * READ without WRITE/CREATE/TRUNCATE is eligible; other platforms ignore. */
    PAL_FILE_RECOVER_READ = 16,
};

enum { PAL_SEEK_SET = 0, PAL_SEEK_CUR = 1, PAL_SEEK_END = 2 };

ps5c_err pal_file_open(const char *path, uint32_t flags, pal_file **out);
void pal_file_close(pal_file *f);
ps5c_err pal_file_read(pal_file *f, void *buf, uint64_t size, uint64_t *read_out);
ps5c_err pal_file_write(pal_file *f, const void *buf, uint64_t size, uint64_t *written_out);
ps5c_err pal_file_seek(pal_file *f, int64_t offset, int whence, uint64_t *pos_out);
/* Read/write at `offset`, safe to call from several threads on one file at
 * once. The OS file position is unspecified afterwards. */
ps5c_err pal_file_pread(pal_file *f, void *buf, uint64_t size, uint64_t offset, uint64_t *read_out);
/* fn(buf, size) before the OS writes into buf (file reads): the GPU write tracker makes pages it
 * protected writable first, since the OS fails the read on a read-only page instead of faulting. */
void pal_set_write_prepare(void (*fn)(void *buf, uint64_t size));
ps5c_err pal_file_pwrite(pal_file *f, const void *buf, uint64_t size, uint64_t offset, uint64_t *written_out);
ps5c_err pal_file_size(pal_file *f, uint64_t *size_out);
ps5c_err pal_file_truncate(pal_file *f, uint64_t size); /* grows with zeros or cuts */
ps5c_err pal_file_remove(const char *path);
/* Renames a file or directory, replacing an existing file at `to` (POSIX rename). */
ps5c_err pal_file_rename(const char *from, const char *to);

typedef struct pal_path_info {
    bool is_dir;
    uint64_t size;
    int64_t mtime_sec; /* Unix time */
    uint32_t mtime_nsec;
} pal_path_info;

/* PS5C_ERR_NOT_FOUND if the path does not exist. */
ps5c_err pal_path_stat(const char *path, pal_path_info *out);
/* PS5C_ERR_BUSY if it already exists. */
ps5c_err pal_mkdir(const char *path);
/* Removes an empty directory. */
ps5c_err pal_rmdir(const char *path);
/* Calls fn for each entry of a directory (not "." or ".."); stops when fn returns false. */
typedef bool (*pal_dir_fn)(void *user, const char *name, bool is_dir);
ps5c_err pal_dir_list(const char *path, pal_dir_fn fn, void *user);

/* Convenience (pal_common.c): reads a whole file into a malloc'd buffer. */
ps5c_err pal_file_read_all(const char *path, uint8_t **data_out, uint64_t *size_out);

/* Read-only native-file view. The caller must keep the file unchanged until
 * close; Windows also denies writers/deletion while the view is open.
 * Virtual image paths return UNSUPPORTED. Empty files succeed with size 0.
 * Initialize to zero, do not copy ownership, and close before reopening.
 * close accepts NULL/zero and clears the view. No private COW pages are used. */
typedef struct pal_file_view {
    const uint8_t *data;
    uint64_t size;
    void *opaque;
} pal_file_view;
ps5c_err pal_file_view_open(const char *path, pal_file_view *out);
void pal_file_view_close(pal_file_view *view);

/* ---- Window (presentation) ----
 * A top-level window showing the last presented frame, scaled to fit. The
 * window runs its own UI thread; all functions are thread-safe. */
typedef struct pal_window pal_window;

ps5c_err pal_window_open(const char *title, uint32_t width, uint32_t height, pal_window **out);
/* Copies a frame of 32-bit B,G,R,X pixels and schedules a repaint. */
void pal_window_present(pal_window *w, const void *pixels, uint32_t width, uint32_t height, uint32_t pitch_bytes);
void pal_window_set_title(pal_window *w, const char *title);
/* Native handles for graphics APIs (Windows: HINSTANCE and HWND). */
void pal_window_native(pal_window *w, void **display, void **handle);
/* Client area size in pixels. */
void pal_window_client_size(pal_window *w, uint32_t *width, uint32_t *height);
/* The displays and the GPU driving each, one line per display, into out. */
size_t pal_display_list(char *out, size_t cap);
/* Moves the window (centered, fitted) to a display the GPU with this adapter
 * LUID drives, unless it is on one already: presenting to a display another
 * GPU drives is slow (and can stall swapchain creation for seconds).
 * 1 moved, 0 already there, -1 that GPU drives no display. */
int pal_window_move_to_gpu(pal_window *w, const uint8_t luid[8]);
/* A GPU presenter draws into the window: stop drawing frames from pal_window_present. */
void pal_window_set_external_present(pal_window *w, bool on);
/* How the window shows ([video] tela): fitted on the screen, the whole screen without
 * borders (Esc goes back to the window; F11 switches between the two) or maximized. */
enum { PAL_WINDOW_NORMAL = 0, PAL_WINDOW_FULLSCREEN = 1, PAL_WINDOW_MAXIMIZED = 2 };
void pal_window_set_mode(pal_window *w, int mode);
/* True once the user closed the window. */
bool pal_window_closed(pal_window *w);
void pal_window_destroy(pal_window *w);

/* ---- Input ----
 * One controller in the PS layout, merged from host devices: a DualSense or
 * DualShock 4 (HID), a gamepad (XInput on Windows) and the keyboard while a
 * window of this process has focus. Button bits are the ScePadButtonDataOffset values. */
enum {
    PAL_PAD_L3 = 0x2,
    PAL_PAD_R3 = 0x4,
    PAL_PAD_OPTIONS = 0x8,
    PAL_PAD_UP = 0x10,
    PAL_PAD_RIGHT = 0x20,
    PAL_PAD_DOWN = 0x40,
    PAL_PAD_LEFT = 0x80,
    PAL_PAD_L2 = 0x100,
    PAL_PAD_R2 = 0x200,
    PAL_PAD_L1 = 0x400,
    PAL_PAD_R1 = 0x800,
    PAL_PAD_TRIANGLE = 0x1000,
    PAL_PAD_CIRCLE = 0x2000,
    PAL_PAD_CROSS = 0x4000,
    PAL_PAD_SQUARE = 0x8000,
    PAL_PAD_TOUCH_PAD = 0x100000,
    /* Host only, never sent to the guest: a touch pad click on the left half
     * (keyboard). A click without a finger lands on the right half. */
    PAL_PAD_TOUCH_LEFT = 0x400000,
};

typedef struct pal_pad_state {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry; /* 0..255, 128 centered, y grows downwards */
    uint8_t l2, r2;         /* 0..255 */
    /* touchpad and motion: a PlayStation controller only (touch_w 0 otherwise) */
    uint8_t touch_count;
    struct {
        uint16_t x, y;
        uint8_t id;
    } touch[2];
    uint16_t touch_w, touch_h;
    bool motion;
    float accel[3]; /* G */
    float gyro[3];  /* rad/s */
} pal_pad_state;

void pal_input_read(pal_pad_state *out);
/* Another local player's controller (player >= 1): the player-th XInput gamepad connected
 * besides the one pal_input_read uses. False when there is none (out untouched). */
bool pal_input_read_player(uint32_t player, pal_pad_state *out);
/* Controller feedback: rumble (large: left, low frequency motor) and the
 * light bar of a PlayStation controller. */
void pal_input_set_rumble(uint8_t large, uint8_t small);
void pal_input_set_light(uint8_t r, uint8_t g, uint8_t b);
/* The host controller in use, for logs: "DualSense (USB)", "XInput", "" (keyboard only) */
const char *pal_input_device(void);
/* Probes a physical controller (not keyboard/script), also updates the native
 * keyboard gate. Safe to call without a game window, e.g. in settings. */
bool pal_input_has_controller(void);

/* Keyboard bindings. A control is a PAL_PAD_* button or a stick direction. */
#define PAL_STICK_LEFT_UP (1u << 24)
#define PAL_STICK_LEFT_DOWN (1u << 25)
#define PAL_STICK_LEFT_LEFT (1u << 26)
#define PAL_STICK_LEFT_RIGHT (1u << 27)
#define PAL_STICK_RIGHT_UP (1u << 28)
#define PAL_STICK_RIGHT_DOWN (1u << 29)
#define PAL_STICK_RIGHT_LEFT (1u << 30)
#define PAL_STICK_RIGHT_RIGHT (1u << 31)
/* Replaces the keys of one control with a comma-separated list of key names
 * ("Z,SPACE"; empty: none). Unknown names are skipped; false if any was. */
bool pal_input_bind_keys(uint32_t control, const char *keys);
/* True once per press of the performance panel key (F10) while the game window has focus. */
bool pal_input_panel_toggled(void);

/* ---- Audio ---- */
/* Host sound output: 48 kHz stereo float. `fill` runs on the audio thread and
 * writes `frames` interleaved L/R samples (silence when it has nothing). */
typedef void (*pal_audio_fill_fn)(void *user, float *out, uint32_t frames);
ps5c_err pal_audio_start(pal_audio_fill_fn fill, void *user);
void pal_audio_stop(void);

/* ---- Movie decoding (MP4: H.264 video, AAC audio) ---- */
typedef struct pal_media pal_media;
typedef struct pal_media_info {
    uint32_t width, height;         /* 0: no video stream */
    double frame_rate;              /* frames per second, 0 if unknown */
    uint32_t channels, sample_rate; /* 0: no audio stream (the PCM is 16-bit, at most stereo) */
    uint64_t duration_us;
} pal_media_info;

/* A host file, or `size` bytes of memory (copied). NULL when the host cannot
 * decode it (on Windows, Media Foundation is missing on "N" editions). */
pal_media *pal_media_open_file(const char *path);
pal_media *pal_media_open_memory(const void *data, uint64_t size);
void pal_media_get_info(pal_media *m, pal_media_info *out);
/* The next picture as NV12 into `nv12` (width * height luma bytes, then the
 * width * height / 2 interleaved CbCr): 1 = a picture, 0 = end, -1 = error. */
int pal_media_next_video(pal_media *m, uint8_t *nv12, uint64_t *time_us);
/* Up to `max_frames` interleaved 16-bit samples: frames written, 0 = end, -1 = error. */
int pal_media_next_audio(pal_media *m, int16_t *pcm, uint32_t max_frames, uint64_t *time_us);
bool pal_media_rewind(pal_media *m);
void pal_media_close(pal_media *m);

/* ---- Video elementary streams (libSceVideodec2): access units in, NV12 out ---- */
typedef struct pal_vdec pal_vdec;
typedef enum { PAL_VDEC_H264, PAL_VDEC_HEVC, PAL_VDEC_VP9 } pal_vdec_codec;
typedef struct pal_vdec_picture {
    uint32_t width, height, pitch; /* NV12: `pitch` (256-aligned) luma rows, then height / 2 CbCr rows */
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    uint64_t pts, dts, attached; /* as given with the access unit the picture came from */
    uint32_t profile, level;
    uint16_t sar_width, sar_height;
    uint8_t full_range, primaries, transfer, matrix;
    bool key, corrupt;
} pal_vdec_picture;
/* NULL when the host has no decoder for it (builds without FFmpeg). */
pal_vdec *pal_vdec_open(pal_vdec_codec codec);
/* Keep coded rows/crop metadata when the consumer crops the video itself. */
pal_vdec *pal_vdec_open_ex(pal_vdec_codec codec, bool apply_cropping);
/* Hands one access unit to the decoder and takes a picture if one is ready:
 * 1 = a picture written to `out` (`cap` bytes) and described in `pic`, 0 = none
 * yet, -1 = the decoder rejected the data, -2 = `out` too small. */
int pal_vdec_decode(pal_vdec *d, const void *au, uint64_t size, uint64_t pts, uint64_t dts, uint64_t attached,
                    uint8_t *out, uint64_t cap, pal_vdec_picture *pic);
/* A picture the decoder still holds (between access units): same results. */
int pal_vdec_flush(pal_vdec *d, uint8_t *out, uint64_t cap, pal_vdec_picture *pic);
/* End of sequence: send EOS and drain reordered pictures, one per call. */
int pal_vdec_drain(pal_vdec *d, uint8_t *out, uint64_t cap, pal_vdec_picture *pic);
void pal_vdec_reset(pal_vdec *d);
void pal_vdec_close(pal_vdec *d);

/* ---- Logging ---- */
/* log_sink_fn-compatible sink writing to the host console / debugger. */
void pal_log_sink(void *user, log_level level, const char *line, size_t len);

#endif
