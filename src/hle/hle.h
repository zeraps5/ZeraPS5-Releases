/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HLE: our C reimplementation of PS5 system library functions.
 *
 * HLE functions never depend on a calling convention. The CPU backend
 * captures the guest's System V argument registers into an hle_args block,
 * the function reads its arguments from there and stores its return value
 * there. This keeps HLE code plain C (any compiler, testable without a CPU).
 *
 *   static void sceKernelUsleep(hle_call *c) {
 *       uint32_t us = hle_arg_u32(c, 0);
 *       ...
 *       hle_ret_s32(c, 0);
 *   }
 */
#ifndef PS5C_HLE_H
#define PS5C_HLE_H

#include <stddef.h>

#include "base/base.h"
#include "mem/gmem.h"

/* Register snapshot shared with generated code: offsets are part of the ABI
 * between the CPU backend's thunks and hle_dispatch(). */
typedef struct hle_args {
    uint64_t gpr[6];         /* rdi, rsi, rdx, rcx, r8, r9 */
    uint64_t xmm[8][2];      /* xmm0-7 (low 128 bits) */
    uint64_t al;             /* rax at entry: vector register count for varargs */
    uint64_t stack;          /* guest address of the first stack argument */
    uint64_t stub_id;        /* which import was called */
    uint64_t ret[2];         /* rax, rdx */
    uint64_t ret_xmm[2][2];  /* xmm0, xmm1 */
} hle_args;

#define HLE_ARGS_OFF_GPR 0
#define HLE_ARGS_OFF_XMM 48
#define HLE_ARGS_OFF_AL 176
#define HLE_ARGS_OFF_STACK 184
#define HLE_ARGS_OFF_STUB 192
#define HLE_ARGS_OFF_RET 200
#define HLE_ARGS_OFF_RET_XMM 216
#define HLE_ARGS_SIZE 248

PS5C_STATIC_ASSERT(offsetof(hle_args, xmm) == HLE_ARGS_OFF_XMM, "hle_args.xmm");
PS5C_STATIC_ASSERT(offsetof(hle_args, al) == HLE_ARGS_OFF_AL, "hle_args.al");
PS5C_STATIC_ASSERT(offsetof(hle_args, stack) == HLE_ARGS_OFF_STACK, "hle_args.stack");
PS5C_STATIC_ASSERT(offsetof(hle_args, stub_id) == HLE_ARGS_OFF_STUB, "hle_args.stub_id");
PS5C_STATIC_ASSERT(offsetof(hle_args, ret) == HLE_ARGS_OFF_RET, "hle_args.ret");
PS5C_STATIC_ASSERT(offsetof(hle_args, ret_xmm) == HLE_ARGS_OFF_RET_XMM, "hle_args.ret_xmm");
PS5C_STATIC_ASSERT(sizeof(hle_args) == HLE_ARGS_SIZE, "hle_args size");

struct ps5_runtime;
struct guest_thread;

typedef struct hle_call {
    hle_args *a;
    struct ps5_runtime *rt;
    struct guest_thread *thread;
    gmem *mem;
} hle_call;

typedef void (*hle_fn)(hle_call *c);
/* Initializes an HLE data object in its guest slot (IMPORT_DATA_SLOT_SIZE bytes). */
typedef void (*hle_data_init_fn)(struct ps5_runtime *rt, void *slot, size_t size);

/* HLE_NATIVE emits a guest ABI bridge into a function's import slot. Needed
 * for operations such as setjmp which must see the caller's saved registers,
 * before the host dispatcher has changed them. data_init initializes the
 * IMPORT_STUB_SIZE-byte code slot for this kind. */
typedef enum { HLE_FUNC, HLE_DATA, HLE_NATIVE } hle_kind;

typedef struct hle_export {
    const char *library;
    const char *name;
    hle_kind kind;
    hle_fn fn;
    hle_data_init_fn data_init;
} hle_export;

#define HLE_FUNC_ENTRY(lib, name, fn) {lib, name, HLE_FUNC, fn, NULL}
#define HLE_DATA_ENTRY(lib, name, init) {lib, name, HLE_DATA, NULL, init}
#define HLE_NATIVE_ENTRY(lib, name, init) {lib, name, HLE_NATIVE, NULL, init}

/* ---- Registry ---- */
ps5c_err hle_registry_init(void);
void hle_registry_free(void);
const hle_export *hle_find(const char *library, uint64_t nid);
/* First export with this NID in any library (for dlsym). */
const hle_export *hle_find_any(uint64_t nid);
/* module_set hle_query callback */
bool hle_query(void *user, const char *library, uint64_t nid);
uint32_t hle_registry_count(void);
uint32_t hle_library_handle(const char *library);
const char *hle_library_from_handle(uint32_t handle);

/* ---- Argument access ---- */
uint64_t hle_arg(hle_call *c, uint32_t index); /* integer/pointer argument (stack beyond 6) */
static inline uint32_t hle_arg_u32(hle_call *c, uint32_t i) { return (uint32_t)hle_arg(c, i); }
static inline int32_t hle_arg_s32(hle_call *c, uint32_t i) { return (int32_t)(uint32_t)hle_arg(c, i); }
static inline guest_addr hle_arg_ptr(hle_call *c, uint32_t i) { return hle_arg(c, i); }

static inline void hle_ret_u64(hle_call *c, uint64_t v) { c->a->ret[0] = v; }
static inline void hle_ret_s32(hle_call *c, int32_t v) { c->a->ret[0] = (uint64_t)(int64_t)v; }
static inline void hle_ret_u32(hle_call *c, uint32_t v) { c->a->ret[0] = v; }

/* Host pointer to guest memory, NULL if the range isn't mapped. */
void *hle_mem(hle_call *c, guest_addr addr, uint64_t size);
/* Copies a NUL-terminated guest string into `buf` (truncating). False if unreadable. */
bool hle_read_str(hle_call *c, guest_addr addr, char *buf, size_t buf_size);

/* The current time of a FreeBSD clock id (0 realtime, 4 monotonic, ...); false if unknown */
bool kernel_clock_now(int32_t id, int64_t *sec, int64_t *nsec);

/* ---- Dispatch (called by the CPU backend's thunks, host ABI) ---- */
typedef struct hle_stub_info {
    const char *library;
    uint64_t nid;
    const char *name;      /* from the NID database, NULL if unknown */
    const hle_export *exp; /* NULL if not implemented */
    uint64_t calls;
} hle_stub_info;

/* Per-library registration functions (one per lib_*.c). */
void hle_register_libkernel(void);
void hle_register_libkernel_sync(void);
void hle_register_libkernel_thread(void);
void hle_register_libkernel_memory(void);
void hle_register_libc_internal(void);
void hle_register_libc_memory(void);
void hle_register_libc_process(void);
void hle_register_libc_jumps(void);
void hle_libc_reset(struct ps5_runtime *rt);
int64_t hle_libc_write_text(hle_call *c, guest_addr stream, const char *text, size_t size);
void hle_register_libkernel_fs(void);
void hle_register_system(void);
void hle_register_agc_driver(void);
void hle_register_equeue(void);
void hle_register_video_out(void);
void hle_register_libkernel_apr(void);
void hle_register_services(void);
void hle_register_input(void);
void hle_register_audio_out(void);
void hle_register_audio_out2(void);
void hle_register_fiber(void);
void hle_register_ult(void);
void hle_register_platform(void);
void hle_register_psml(void);
/* sceKernelIsTrinityMode(): the game runs in PS5 Pro mode (lib_kernel.c) */
bool hle_kernel_trinity_mode(void);
/* The executable's SDK version (procparam), as sceKernelGetCompiledSdkVersion reports it */
uint32_t hle_kernel_compiled_sdk_version(hle_call *c);
void hle_register_json2(void);
void hle_register_rtc(void);
void hle_register_net_offline(void);
void hle_register_save_data(void);
void hle_register_save_data_dir(void);
void hle_register_dialogs(void);
void hle_register_services_offline(void);
void hle_register_av_player(void);
void hle_register_videodec2(void);
void hle_register_vdecsw(void);
void hle_vdecsw_reset(struct ps5_runtime *rt);
void hle_register_ampr(void);
void hle_register_png_enc(void);
void hle_register_png_dec(void);
void hle_register_jpeg_dec(void);
void hle_register_audiodec(void);
void hle_register_compat_stubs(void);
void hle_register_zlib(void);
void hle_register_audio3d(void);
void hle_register_jpeg_enc(void);
void hle_register_camera(void);
void hle_register_ajm(void);
void hle_register_acm(void);
void hle_register_ngs2(void);
void hle_register_font(void);
void hle_register_font_text(void); /* lib_font_text.c, from hle_register_font */
/* lib_font.c: a font's glyph metrics (SceFontGlyphMetrics) at its pixel scale */
void font_object_metrics(hle_call *c, guest_addr font, uint32_t code, float out[8]);
void hle_register_voice(void);
void hle_register_np_trophy(void);
void hle_register_playgo(void);
void hle_playgo_reset(struct ps5_runtime *rt);

/* Host directory for the running game's user data: user/<game folder> (lib_kernel_fs.c). */
const char *hle_user_dir(struct ps5_runtime *rt);
/* Run-time mount points of the guest file system (lib_kernel_fs.c). */
bool hle_fs_mount(const char *point, const char *host_dir);
void hle_fs_unmount(const char *point);

/* Common SCE error codes */
#define SCE_KERNEL_ERROR_EINVAL ((int32_t)0x80020016)
#define SCE_KERNEL_ERROR_EFAULT ((int32_t)0x8002000E)
#define SCE_KERNEL_ERROR_ENOSYS ((int32_t)0x8002004E)
#define SCE_KERNEL_ERROR_ESRCH ((int32_t)0x80020003)
#define SCE_KERNEL_ERROR_ENOMEM ((int32_t)0x8002000C)

#endif
