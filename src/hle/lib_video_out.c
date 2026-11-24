/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceVideoOut: display port, framebuffers and flips.
 *
 * One port (the main display). A host "vblank" thread ticks at 60 Hz: it
 * signals vblank events, performs the oldest pending flip (copies the
 * framebuffer to the host window) and signals flip events.
 *
 * Framebuffers are shown as 32-bit BGRX. Tiled and compressed layouts and
 * other pixel formats are presented as-is for now (they need the GPU path to
 * be decoded). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "core/user_config.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "hle/audio_mix.h"
#include "hle/video_out.h"
#include "gpu/renderer.h"
#include "pal/pal.h"
#include "pal/bundled_assets.h"

void hle_register_table(const hle_export *table, size_t count);
void agc_driver_log_stats(void);

#define LIB "libSceVideoOut"

/* Errors */
#define VO_ERR_INVALID_VALUE ((int32_t)0x80290001)
#define VO_ERR_INVALID_ADDRESS ((int32_t)0x80290002)
#define VO_ERR_INVALID_INDEX ((int32_t)0x8029000A)
#define VO_ERR_INVALID_HANDLE ((int32_t)0x8029000B)
#define VO_ERR_INVALID_EVENT_QUEUE ((int32_t)0x8029000C)
#define VO_ERR_INVALID_EVENT ((int32_t)0x8029000D)
#define VO_ERR_FLIP_QUEUE_FULL ((int32_t)0x8029000F)
#define VO_ERR_SLOT_OCCUPIED ((int32_t)0x80290010)

/* Event idents */
#define VO_EVENT_FLIP 0
#define VO_EVENT_VBLANK 1
#define VO_EVENT_PRE_VBLANK_START 2

#define PORT_HANDLE 1
#define MAX_BUFFERS 16
#define MAX_SETS 4
#define MAX_REGISTRATIONS 16
#define FLIP_QUEUE 16
#define VBLANK_NS 16666667ull
#define REFRESH_RATE_59_94HZ 3

/* PS5 ABI structures */
typedef struct vo_attribute2 {
    uint32_t reserved0, tiling_mode, aspect_ratio, width, height, pitch_in_pixel;
    uint64_t option;
    uint64_t pixel_format;
    uint64_t dcc_cb_register_clear_color;
    uint32_t dcc_control, pad0;
    uint64_t reserved1[3];
} vo_attribute2;
PS5C_STATIC_ASSERT(sizeof(vo_attribute2) == 80, "SceVideoOutBufferAttribute2");

/* PS4 ABI: SceVideoOutBufferAttribute (32-bit pixel formats such as 0x80002200) */
typedef struct vo_attribute_ps4 {
    uint32_t pixel_format, tiling_mode, aspect_ratio, width, height, pitch_in_pixel, option, reserved0;
    uint64_t reserved1;
} vo_attribute_ps4;
PS5C_STATIC_ASSERT(sizeof(vo_attribute_ps4) == 40, "SceVideoOutBufferAttribute (PS4)");

typedef struct vo_resolution_status_ps4 {
    uint32_t full_width, full_height, pane_width, pane_height;
    uint64_t refresh_rate;
    float screen_size_in_inch;
    uint16_t flags, reserved0;
    uint32_t reserved1[3];
} vo_resolution_status_ps4;
PS5C_STATIC_ASSERT(sizeof(vo_resolution_status_ps4) == 48, "SceVideoOutResolutionStatus (PS4)");

typedef struct vo_buffers {
    uint64_t data, metadata, reserved[2];
} vo_buffers;

typedef struct vo_flip_status {
    uint64_t count, process_time, reserved0;
    int64_t flip_arg;
    uint64_t reserved1, process_time_counter;
    int32_t gc_queue_num, flip_pending_num, current_buffer;
    uint32_t reserved2;
    uint64_t submit_process_time_counter;
    uint64_t reserved3[7];
} vo_flip_status;
PS5C_STATIC_ASSERT(sizeof(vo_flip_status) == 128, "SceVideoOutFlipStatus (PS5)");

typedef struct vo_vblank_status {
    uint64_t count, process_time, reserved, process_time_counter;
    uint8_t flags, phase, pad[6];
} vo_vblank_status;
PS5C_STATIC_ASSERT(sizeof(vo_vblank_status) == 40, "SceVideoOutVblankStatus (PS5)");

typedef struct vo_output_status {
    uint32_t resolution, dynamic_range;
    uint64_t refresh_rate, flags, reserved[3];
} vo_output_status;

typedef struct registration {
    uint64_t eq;
    uint64_t udata;
} registration;

static struct {
    pal_mutex *lock;
    pal_cond *vblank_cond;
    bool open;
    gmem *mem;
    vo_attribute2 sets[MAX_SETS];
    bool set_used[MAX_SETS];
    struct {
        int32_t set;
        guest_addr data;
    } buffers[MAX_BUFFERS];
    int32_t flip_rate; /* 0 = 60, 1 = 30, 2 = 20 Hz */
    struct {
        int32_t index;
        int64_t arg;
    } queue[FLIP_QUEUE];
    uint32_t queue_len;
    int32_t presenting; /* buffer being presented, -1 = none */
    vo_flip_status flip;
    vo_vblank_status vblank;
    registration flip_regs[MAX_REGISTRATIONS], vblank_regs[MAX_REGISTRATIONS];
    uint32_t flip_reg_count, vblank_reg_count;
    pal_thread *thread;
    pal_window *window;
    renderer *renderer; /* NULL: software presenter */
    bool splash_hidden; /* HideSplashScreen may precede framebuffer registration. */
    bool intro_window;  /* the intro opened the window: the splash screen comes at registration */
    char title[288];
    uint64_t start_ns;
} g_vo;

static char g_caption[64] = "ZeraPS5";

void video_out_set_caption(const char *emulator) {
    snprintf(g_caption, sizeof(g_caption), "%s", emulator);
}

/* ---- Event data: time[0:12) | count[12:16) | payload << 16 ---- */

static int64_t make_event_data(int64_t old_data, void *ctx) {
    uint64_t counter = ((uint64_t)old_data >> 12) & 0xF;
    if (counter != 0xF) counter++;
    uint64_t time = (pal_time_ns() / 1000u) & 0xFFF;
    uint64_t payload = (uint64_t)*(const int64_t *)ctx;
    return (int64_t)(time | (counter << 12) | ((payload & 0x0000FFFFFFFFFFFFull) << 16));
}

static void trigger_all(const registration *regs, uint32_t n, uint64_t ident, int64_t payload) {
    for (uint32_t i = 0; i < n; i++) {
        equeue_trigger(regs[i].eq, ident, EVFILT_VIDEO_OUT_, make_event_data, &payload, 0, false);
    }
}

/* ---- Presentation ---- */

static void present(int32_t index) {
    if (!g_vo.window || index < 0 || index >= MAX_BUFFERS || g_vo.buffers[index].set < 0) return;
    const vo_attribute2 *a = &g_vo.sets[g_vo.buffers[index].set];
    uint32_t pitch_px = a->pitch_in_pixel ? a->pitch_in_pixel : a->width;
    if (g_vo.renderer) {
        renderer_surface s = {g_vo.buffers[index].data, a->width, a->height, pitch_px, a->pixel_format, a->tiling_mode};
        renderer_present(g_vo.renderer, &s);
        return;
    }
    uint64_t bytes = (uint64_t)pitch_px * a->height * 4;
    const void *pixels = gmem_host(g_vo.mem, g_vo.buffers[index].data, bytes);
    if (pixels) pal_window_present(g_vo.window, pixels, a->width, a->height, pitch_px * 4);
}

/* Frame rate in the window title. */
static void update_title(uint64_t flips) {
    static uint64_t last_ns, last_flips;
    uint64_t now = pal_time_ns();
    if (g_vo.window && last_ns) {
        double fps = (double)(flips - last_flips) * 1e9 / (double)(now - last_ns);
        char title[352];
        snprintf(title, sizeof(title), "%s - %.0f fps - %llu frames%s", g_vo.title, fps,
                 (unsigned long long)flips, g_vo.renderer ? " (Vulkan)" : " (no GPU renderer)");
        pal_window_set_title(g_vo.window, title);
    }
    last_ns = now;
    last_flips = flips;
}

static int32_t vblank_thread(void *arg) {
    PS5C_UNUSED(arg);
    uint64_t next = pal_time_ns() + VBLANK_NS;
    for (;;) {
        uint64_t now = pal_time_ns();
        if (next > now) pal_sleep_ns(next - now);
        next += VBLANK_NS;
        if (pal_time_ns() > next + 10 * VBLANK_NS) next = pal_time_ns() + VBLANK_NS; /* don't try to catch up */

        if (g_vo.window && pal_window_closed(g_vo.window)) {
            LOG_I(GPU, "window closed: exiting");
            if (g_vo.renderer) renderer_save_caches(g_vo.renderer);
            pal_exit_process(0);
        }

        pal_mutex_lock(g_vo.lock);
        uint64_t t = pal_time_ns() - g_vo.start_ns;
        g_vo.vblank.count++;
        g_vo.vblank.process_time = t / 1000u;
        g_vo.vblank.process_time_counter = t;
        trigger_all(g_vo.vblank_regs, g_vo.vblank_reg_count, VO_EVENT_VBLANK, (int64_t)g_vo.vblank.count);

        bool do_flip = g_vo.queue_len > 0 && g_vo.vblank.count % (uint64_t)(g_vo.flip_rate + 1) == 0;
        int32_t index = -1;
        int64_t flip_arg = 0;
        if (do_flip) {
            index = g_vo.queue[0].index;
            flip_arg = g_vo.queue[0].arg;
            memmove(&g_vo.queue[0], &g_vo.queue[1], (g_vo.queue_len - 1) * sizeof(g_vo.queue[0]));
            g_vo.queue_len--;
            g_vo.presenting = index;
        }
        pal_mutex_unlock(g_vo.lock);

        if (do_flip) {
            present(index);
            pal_mutex_lock(g_vo.lock);
            g_vo.presenting = -1;
            g_vo.flip.count++;
            gpu_note_video_flip();
            g_vo.flip.process_time = t / 1000u;
            g_vo.flip.process_time_counter = t;
            g_vo.flip.flip_arg = flip_arg;
            g_vo.flip.current_buffer = index;
            g_vo.flip.flip_pending_num = (int32_t)g_vo.queue_len;
            trigger_all(g_vo.flip_regs, g_vo.flip_reg_count, VO_EVENT_FLIP, flip_arg);
            pal_mutex_unlock(g_vo.lock);
        }
        pal_mutex_lock(g_vo.lock);
        pal_cond_broadcast(g_vo.vblank_cond);
        uint64_t vblanks = g_vo.vblank.count, flips = g_vo.flip.count;
        pal_mutex_unlock(g_vo.lock);
        if (vblanks % 60 == 0) update_title(flips);
        if (vblanks % 300 == 0) {
            LOG_I(GPU, "vblank %llu: %llu frames flipped", (unsigned long long)vblanks, (unsigned long long)flips);
            agc_driver_log_stats();
        }
    }
    return 0;
}

uint64_t video_out_flip_count(void) {
    if (!g_vo.lock) return 0;
    pal_mutex_lock(g_vo.lock);
    uint64_t n = g_vo.flip.count;
    pal_mutex_unlock(g_vo.lock);
    return n;
}

/* The system splash screen: the package's sce_sys/pic1.dds (BC7), shown until
 * the game calls sceSystemServiceHideSplashScreen. */
int kernel_fs_resolve(hle_call *c, const char *guest, char *host, size_t host_size);

static void show_splash(hle_call *c, renderer *r) {
    pal_mutex_lock(g_vo.lock);
    bool hidden = g_vo.splash_hidden;
    pal_mutex_unlock(g_vo.lock);
    if (hidden) {
        LOG_I(GPU, "splash screen skipped: already hidden by the game");
        return;
    }
    char path[1024];
    if (kernel_fs_resolve(c, "/app0/sce_sys/pic1.dds", path, sizeof(path)) != 0) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *data = NULL;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 148 && fseek(f, 0, SEEK_SET) == 0) {
        data = (uint8_t *)malloc((size_t)size);
        if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    if (!data) return;
    uint32_t height, width, dxgi = 0;
    memcpy(&height, data + 12, 4);
    memcpy(&width, data + 16, 4);
    if (!memcmp(data + 84, "DX10", 4)) memcpy(&dxgi, data + 128, 4);
    /* DXGI_FORMAT_BC7_UNORM (98) / BC7_UNORM_SRGB (99) */
    /* Serialize the final show with HideSplashScreen too: the request can
     * arrive while the DDS is being read. Both paths take VideoOut then renderer. */
    pal_mutex_lock(g_vo.lock);
    if (g_vo.splash_hidden) {
        LOG_I(GPU, "splash screen skipped: hidden while loading its image");
    } else if (memcmp(data, "DDS ", 4) || (dxgi != 98 && dxgi != 99) ||
        !renderer_set_splash(r, data + 148, (size_t)size - 148, width, height, dxgi == 99))
        LOG_W(GPU, "splash screen %s not shown (format %u)", path, dxgi);
    else
        LOG_I(GPU, "splash screen %ux%u shown", width, height);
    pal_mutex_unlock(g_vo.lock);
    free(data);
}

void video_out_hide_splash(void) {
    pal_mutex_lock(g_vo.lock);
    g_vo.splash_hidden = true;
    if (g_vo.renderer) renderer_hide_splash(g_vo.renderer);
    pal_mutex_unlock(g_vo.lock);
}

struct renderer *video_out_renderer(void) { return g_vo.renderer; }

/* ---- Port ---- */

static bool check_handle(hle_call *c, uint32_t i) { return g_vo.open && hle_arg_s32(c, i) == PORT_HANDLE; }

/* int sceVideoOutOpen(SceUserServiceUserId user, int bus_type, int index, const void *param) */
static void open_(hle_call *c) {
    pal_mutex_lock(g_vo.lock);
    if (!g_vo.open) {
        g_vo.open = true;
        g_vo.start_ns = pal_time_ns();
        g_vo.presenting = -1;
        /* before the first flip there is no last flip: PSPHD (the PSP classics) waits for
         * a flip event while flipArg equals the buffer it is about to draw (0) */
        g_vo.flip.flip_arg = -1;
        g_vo.flip.current_buffer = -1;
        for (int i = 0; i < MAX_BUFFERS; i++) g_vo.buffers[i].set = -1;
        g_vo.mem = c->mem;
        if (!g_vo.thread && pal_thread_create(&g_vo.thread, vblank_thread, NULL, "videoout vblank", 0) != PS5C_OK) {
            LOG_E(GPU, "cannot start the vblank thread");
        }
    }
    pal_mutex_unlock(g_vo.lock);
    LOG_I(GPU, "sceVideoOutOpen(bus %d) -> handle %d", hle_arg_s32(c, 1), PORT_HANDLE);
    hle_ret_s32(c, PORT_HANDLE);
}

static void close_(hle_call *c) { hle_ret_s32(c, check_handle(c, 0) ? 0 : VO_ERR_INVALID_HANDLE); }

/* void sceVideoOutSetBufferAttribute2(attr *a, uint64_t pixel_format, uint32_t tiling_mode, uint32_t width,
 *                                     uint32_t height, uint64_t option, uint32_t dcc_control, uint64_t dcc_clear) */
static void set_buffer_attribute2(hle_call *c) {
    vo_attribute2 *a = (vo_attribute2 *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(vo_attribute2));
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->pixel_format = hle_arg(c, 1);
    a->tiling_mode = hle_arg_u32(c, 2);
    a->width = hle_arg_u32(c, 3);
    a->height = hle_arg_u32(c, 4);
    a->option = hle_arg(c, 5);
    a->dcc_control = hle_arg_u32(c, 6);
    a->dcc_cb_register_clear_color = hle_arg(c, 7);
}

static int32_t store_set(hle_call *c, int32_t set, guest_addr attr_addr) {
    const vo_attribute2 *a = (const vo_attribute2 *)hle_mem(c, attr_addr, sizeof(vo_attribute2));
    if (!a) return VO_ERR_INVALID_ADDRESS;
    if (set < 0 || set >= MAX_SETS) return VO_ERR_INVALID_VALUE;
    g_vo.sets[set] = *a;
    g_vo.set_used[set] = true;
    return 0;
}

/* The host window, titled "ZeraPS5 0.1.0 build 77 - Game Name (PPSA01234)", and its renderer */
static void create_window(ps5_runtime *rt, gmem *mem, uint32_t w, uint32_t h) {
    char name[160], id[32];
    if (!settings_param_title((int32_t)settings_get_int("geral", "idioma", USER_LANGUAGE_DEFAULT), name, sizeof(name)))
        snprintf(name, sizeof(name), "%s", module_set_main(rt->ms)->name);
    if (settings_param_string("titleId", id, sizeof(id)))
        snprintf(g_vo.title, sizeof(g_vo.title), "%s - %s (%s)", g_caption, name, id);
    else
        snprintf(g_vo.title, sizeof(g_vo.title), "%s - %s", g_caption, name);
    if (pal_window_open(g_vo.title, w, h, &g_vo.window) != PS5C_OK) {
        LOG_W(GPU, "cannot open the host window");
        g_vo.window = NULL;
        return;
    }
    /* [video] tela: 0 window, 1 full screen (Esc leaves it), 2 maximized window */
    pal_window_set_mode(g_vo.window, (int)settings_get_int("video", "tela", PAL_WINDOW_NORMAL));
    renderer_set_cache_dir(hle_user_dir(rt));
    renderer *r = renderer_create(g_vo.window, mem);
    pal_mutex_lock(g_vo.lock);
    g_vo.renderer = r;
    pal_mutex_unlock(g_vo.lock);
}

/* The host window and renderer, at the first framebuffer registration (unless the intro opened
 * them); the splash screen then. */
static void open_window(hle_call *c, const vo_attribute2 *attr) {
    const vo_attribute2 a = *attr;
    if (!g_vo.window) {
        uint32_t w = a.width, h = a.height;
        while (w > 1920) { /* keep the initial window on screen */
            w /= 2;
            h /= 2;
        }
        create_window(c->rt, c->mem, w, h);
        if (g_vo.renderer) show_splash(c, g_vo.renderer);
    } else if (g_vo.intro_window) {
        g_vo.intro_window = false;
        if (g_vo.renderer) show_splash(c, g_vo.renderer);
    }
}

/* NV12 (limited range, BT.601) to top-down BGRA */
static void intro_convert(const uint8_t *nv12, uint8_t *out, uint32_t w, uint32_t h) {
    const uint8_t *uv = nv12 + (size_t)w * h;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = nv12 + (size_t)y * w, *c = uv + (size_t)(y / 2) * w;
        uint8_t *o = out + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            int l = 298 * ((int)row[x] - 16), u = (int)c[x & ~1u] - 128, v = (int)c[x | 1u] - 128;
            int r = (l + 409 * v + 128) >> 8, g = (l - 100 * u - 208 * v + 128) >> 8, b = (l + 516 * u + 128) >> 8;
            o[4 * x] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[4 * x + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[4 * x + 2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[4 * x + 3] = 255;
        }
    }
}

/* A local clip overrides the executable resource. The immutable resource is
 * installed and rolled back with the executable by the public release installer. */
static pal_media *intro_open(void) {
    static const char path[] = "data/intro.mp4";
    FILE *file = fopen(path, "rb");
    if (file) {
        fclose(file);
        return pal_media_open_file(path);
    }
    size_t size = 0;
    const void *data = pal_bundled_asset(PS5C_BUNDLED_INTRO, &size);
    return data ? pal_media_open_memory(data, size) : NULL;
}

/* The intro's sound as 48 kHz stereo 16-bit frames (*frames of them); NULL if it has none */
static int16_t *intro_sound(uint32_t *frames) {
    *frames = 0;
    pal_media *m = intro_open();
    pal_media_info info = {0};
    if (m) pal_media_get_info(m, &info);
    int16_t *in = NULL, *out = NULL;
    uint32_t ch = info.channels, n = 0, cap = info.sample_rate * 30; /* a short clip: 30 s at most */
    if (m && ch && ch <= 2 && info.sample_rate) in = (int16_t *)malloc((size_t)cap * ch * 2);
    int got;
    uint64_t t;
    while (in && n < cap && (got = pal_media_next_audio(m, in + (size_t)n * ch, cap - n < 4096 ? cap - n : 4096, &t)) > 0)
        n += (uint32_t)got;
    if (m) pal_media_close(m);
    uint32_t total = n ? (uint32_t)((uint64_t)n * 48000 / info.sample_rate) : 0;
    if (total) out = (int16_t *)malloc((size_t)total * 4);
    for (uint32_t i = 0; out && i < total; i++) { /* linear resampling */
        uint64_t pos = (uint64_t)i * info.sample_rate * 256 / 48000;
        uint32_t k = (uint32_t)(pos >> 8), f = (uint32_t)(pos & 255), k1 = k + 1 < n ? k + 1 : k;
        for (uint32_t c = 0; c < 2; c++) {
            uint32_t sc = ch == 2 ? c : 0;
            int a = in[(size_t)k * ch + sc], b = in[(size_t)k1 * ch + sc];
            out[2 * i + c] = (int16_t)(a + ((b - a) * (int)f) / 256);
        }
    }
    free(in);
    if (out) *frames = total;
    return out;
}

void video_out_intro(ps5_runtime *rt) {
    if (!settings_get_int("geral", "abertura", 1) || getenv("PS5C_NO_INTRO") || g_vo.window) return;
    pal_media *m = intro_open();
    pal_media_info info = {0};
    if (m) pal_media_get_info(m, &info);
    uint8_t *nv12 = NULL, *bgra = NULL;
    if (m && info.width && info.height) {
        nv12 = (uint8_t *)malloc((size_t)info.width * (info.height + 1) * 3 / 2);
        bgra = (uint8_t *)malloc((size_t)info.width * info.height * 4);
    }
    if (nv12 && bgra) create_window(rt, &rt->mem, 1920, 1080);
    renderer *r = g_vo.renderer;
    if (r) {
        g_vo.intro_window = true;
        uint32_t sound_frames = 0, played = 0;
        int16_t *sound = intro_sound(&sound_frames);
        int stream = sound ? audio_mix_open() : -1;
        uint64_t t, start = 0, shown = 0;
        while (!pal_window_closed(g_vo.window) && pal_media_next_video(m, nv12, &t) > 0) {
            intro_convert(nv12, bgra, info.width, info.height);
            if (!shown) start = pal_time_ns() - t * 1000u;
            for (;;) { /* until the picture's time, the sound kept ahead */
                while (stream >= 0 && played < sound_frames && audio_mix_queued(stream) < AUDIO_QUEUE_TARGET * 2) {
                    uint32_t n = sound_frames - played < 1024 ? sound_frames - played : 1024;
                    audio_mix_write(stream, sound + (size_t)played * 2, n, 2, false, 1.0f, 1.0f);
                    played += n;
                }
                uint64_t due = start + t * 1000u, now = pal_time_ns();
                if (now >= due) break;
                pal_sleep_ns(due - now < 5000000u ? due - now : 5000000u);
            }
            if (!renderer_show_picture(r, bgra, info.width, info.height)) break;
            shown++;
        }
        for (uint64_t until = pal_time_ns() + 300000000u; stream >= 0 && audio_mix_queued(stream) && pal_time_ns() < until;)
            pal_sleep_ns(10000000u); /* its last sound */
        if (stream >= 0) audio_mix_close(stream);
        free(sound);
        renderer_hide_picture(r);
        LOG_I(GPU, "intro video: %llu pictures", (unsigned long long)shown);
    }
    if (m) pal_media_close(m);
    free(nv12);
    free(bgra);
}

/* the buffer slots start..start+num-1 hold no set but (re-registering) `set` */
static bool slots_free_locked(int32_t start, int32_t num, int32_t set) {
    for (int32_t i = start; i < start + num; i++)
        if (g_vo.buffers[i].set >= 0 && g_vo.buffers[i].set != set) return false;
    return true;
}

/* int sceVideoOutRegisterBuffers2(int handle, int set_index, int start, const SceVideoOutBuffers *buffers,
 *                                 int num, const attr *a, int category, void *option) */
static void register_buffers2(hle_call *c) {
    if (!check_handle(c, 0)) {
        hle_ret_s32(c, VO_ERR_INVALID_HANDLE);
        return;
    }
    int32_t set = hle_arg_s32(c, 1), start = hle_arg_s32(c, 2), num = hle_arg_s32(c, 4);
    if (start < 0 || num <= 0 || start + num > MAX_BUFFERS) {
        hle_ret_s32(c, VO_ERR_INVALID_INDEX);
        return;
    }
    const vo_buffers *bufs = (const vo_buffers *)hle_mem(c, hle_arg_ptr(c, 3), (uint64_t)num * sizeof(vo_buffers));
    if (!bufs) {
        hle_ret_s32(c, VO_ERR_INVALID_ADDRESS);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    /* slots another set holds stay its own until unregistered (rps5e 6b0d37c) */
    int32_t err = slots_free_locked(start, num, set) ? store_set(c, set, hle_arg_ptr(c, 5)) : VO_ERR_SLOT_OCCUPIED;
    if (err == 0) {
        for (int32_t i = 0; i < num; i++) {
            g_vo.buffers[start + i].set = set;
            g_vo.buffers[start + i].data = bufs[i].data;
        }
    }
    const vo_attribute2 a = g_vo.sets[set < 0 || set >= MAX_SETS ? 0 : set];
    pal_mutex_unlock(g_vo.lock);
    if (err) {
        hle_ret_s32(c, err);
        return;
    }
    LOG_I(GPU, "registered %d framebuffers (set %d): %ux%u, pixel format 0x%llx, tiling %u, first at 0x%llx", num,
          set, a.width, a.height, (unsigned long long)a.pixel_format, a.tiling_mode,
          (unsigned long long)bufs[0].data);
    open_window(c, &a);
    hle_ret_s32(c, set);
}

/* PS4: void sceVideoOutSetBufferAttribute(attr *a, uint32_t pixel_format, uint32_t tiling_mode,
 *      uint32_t aspect_ratio, uint32_t width, uint32_t height, uint32_t pitch_in_pixel) */
static void set_buffer_attribute(hle_call *c) {
    vo_attribute_ps4 *a = (vo_attribute_ps4 *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(vo_attribute_ps4));
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->pixel_format = hle_arg_u32(c, 1);
    a->tiling_mode = hle_arg_u32(c, 2);
    a->aspect_ratio = hle_arg_u32(c, 3);
    a->width = hle_arg_u32(c, 4);
    a->height = hle_arg_u32(c, 5);
    a->pitch_in_pixel = hle_arg_u32(c, 6);
}

/* PS4: int sceVideoOutRegisterBuffers(int handle, int start, void *const *addresses, int num,
 *      const SceVideoOutBufferAttribute *a): the set index it took, kept as a PS5 attribute */
static void register_buffers(hle_call *c) {
    if (!check_handle(c, 0)) {
        hle_ret_s32(c, VO_ERR_INVALID_HANDLE);
        return;
    }
    int32_t start = hle_arg_s32(c, 1), num = hle_arg_s32(c, 3);
    if (start < 0 || num <= 0 || start + num > MAX_BUFFERS) {
        hle_ret_s32(c, VO_ERR_INVALID_INDEX);
        return;
    }
    const uint64_t *addrs = (const uint64_t *)hle_mem(c, hle_arg_ptr(c, 2), (uint64_t)num * 8);
    const vo_attribute_ps4 *p = (const vo_attribute_ps4 *)hle_mem(c, hle_arg_ptr(c, 4), sizeof(vo_attribute_ps4));
    if (!addrs || !p) {
        hle_ret_s32(c, VO_ERR_INVALID_ADDRESS);
        return;
    }
    vo_attribute2 a;
    memset(&a, 0, sizeof(a));
    a.pixel_format = p->pixel_format;
    a.tiling_mode = p->tiling_mode;
    a.aspect_ratio = p->aspect_ratio;
    a.width = p->width;
    a.height = p->height;
    a.pitch_in_pixel = p->pitch_in_pixel;
    a.option = p->option;
    pal_mutex_lock(g_vo.lock);
    if (!slots_free_locked(start, num, -1)) {
        pal_mutex_unlock(g_vo.lock);
        hle_ret_s32(c, VO_ERR_SLOT_OCCUPIED);
        return;
    }
    int32_t set = 0;
    while (set < MAX_SETS && g_vo.set_used[set]) set++;
    if (set < MAX_SETS) {
        g_vo.sets[set] = a;
        g_vo.set_used[set] = true;
        for (int32_t i = 0; i < num; i++) {
            g_vo.buffers[start + i].set = set;
            g_vo.buffers[start + i].data = addrs[i];
        }
    }
    pal_mutex_unlock(g_vo.lock);
    if (set == MAX_SETS) {
        hle_ret_s32(c, VO_ERR_INVALID_VALUE);
        return;
    }
    LOG_I(GPU, "registered %d PS4 framebuffers (set %d): %ux%u, pixel format 0x%x, tiling %u, first at 0x%llx", num,
          set, p->width, p->height, p->pixel_format, p->tiling_mode, (unsigned long long)addrs[0]);
    open_window(c, &a);
    hle_ret_s32(c, set);
}

static void submit_change_buffer_attribute2(hle_call *c) {
    if (!check_handle(c, 0)) {
        hle_ret_s32(c, VO_ERR_INVALID_HANDLE);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    int32_t err = store_set(c, hle_arg_s32(c, 1), hle_arg_ptr(c, 2));
    pal_mutex_unlock(g_vo.lock);
    hle_ret_s32(c, err);
}

static void unregister_buffers(hle_call *c) {
    int32_t set = hle_arg_s32(c, 1);
    if (!check_handle(c, 0) || set < 0 || set >= MAX_SETS) {
        hle_ret_s32(c, VO_ERR_INVALID_VALUE);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (g_vo.buffers[i].set == set) g_vo.buffers[i].set = -1;
    }
    g_vo.set_used[set] = false;
    pal_mutex_unlock(g_vo.lock);
    hle_ret_s32(c, 0);
}

static void set_flip_rate(hle_call *c) {
    int32_t rate = hle_arg_s32(c, 1);
    if (!check_handle(c, 0) || rate < 0 || rate > 2) {
        hle_ret_s32(c, VO_ERR_INVALID_VALUE);
        return;
    }
    g_vo.flip_rate = rate;
    hle_ret_s32(c, 0);
}

static int32_t submit_flip_locked(int32_t index, int64_t arg) {
    int32_t ret = 0;
    if (g_vo.queue_len == FLIP_QUEUE) {
        ret = VO_ERR_FLIP_QUEUE_FULL;
    } else {
        g_vo.queue[g_vo.queue_len].index = index;
        g_vo.queue[g_vo.queue_len].arg = arg;
        g_vo.queue_len++;
        g_vo.flip.flip_pending_num = (int32_t)g_vo.queue_len;
        g_vo.flip.submit_process_time_counter = pal_time_ns() - g_vo.start_ns;
    }
    return ret;
}

int32_t video_out_submit_flip(int32_t handle, int32_t index, int32_t mode, int64_t arg) {
    PS5C_UNUSED(mode);
    if (!g_vo.open || handle != PORT_HANDLE) return VO_ERR_INVALID_HANDLE;
    if (index < -1 || index >= MAX_BUFFERS) return VO_ERR_INVALID_INDEX;
    pal_mutex_lock(g_vo.lock);
    int32_t ret = submit_flip_locked(index, arg);
    pal_mutex_unlock(g_vo.lock);
    return ret;
}

int32_t video_out_submit_flip_wait(int32_t handle, int32_t index, int32_t mode, int64_t arg, uint64_t timeout_ns) {
    PS5C_UNUSED(mode);
    if (!g_vo.open || handle != PORT_HANDLE) return VO_ERR_INVALID_HANDLE;
    if (index < -1 || index >= MAX_BUFFERS) return VO_ERR_INVALID_INDEX;
    uint64_t deadline = pal_time_ns() + timeout_ns;
    pal_mutex_lock(g_vo.lock);
    for (;;) {
        uint64_t now = pal_time_ns();
        if (g_vo.queue_len < FLIP_QUEUE || now >= deadline) break;
        pal_cond_wait_ns(g_vo.vblank_cond, g_vo.lock, deadline - now);
    }
    int32_t ret = submit_flip_locked(index, arg);
    pal_mutex_unlock(g_vo.lock);
    return ret;
}

static bool buffer_busy(int32_t index) {
    if (g_vo.presenting == index) return true;
    for (uint32_t i = 0; i < g_vo.queue_len; i++)
        if (g_vo.queue[i].index == index) return true;
    return false;
}

void video_out_wait_buffer(int32_t handle, int32_t index, uint64_t timeout_ns) {
    if (!g_vo.open || handle != PORT_HANDLE || index < 0 || index >= MAX_BUFFERS) return;
    uint64_t deadline = pal_time_ns() + timeout_ns;
    pal_mutex_lock(g_vo.lock);
    while (buffer_busy(index)) {
        uint64_t now = pal_time_ns();
        if (now >= deadline) break;
        pal_cond_wait_ns(g_vo.vblank_cond, g_vo.lock, deadline - now);
    }
    pal_mutex_unlock(g_vo.lock);
}

/* int sceVideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg) */
static void submit_flip(hle_call *c) {
    hle_ret_s32(c, video_out_submit_flip(hle_arg_s32(c, 0), hle_arg_s32(c, 1), hle_arg_s32(c, 2), (int64_t)hle_arg(c, 3)));
}

static void is_flip_pending(hle_call *c) {
    if (!check_handle(c, 0)) {
        hle_ret_s32(c, VO_ERR_INVALID_HANDLE);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    int32_t pending = (int32_t)g_vo.queue_len;
    pal_mutex_unlock(g_vo.lock);
    hle_ret_s32(c, pending);
}

static void get_flip_status(hle_call *c) {
    vo_flip_status *out = (vo_flip_status *)hle_mem(c, hle_arg_ptr(c, 1), sizeof(vo_flip_status));
    if (!check_handle(c, 0) || !out) {
        hle_ret_s32(c, out ? VO_ERR_INVALID_HANDLE : VO_ERR_INVALID_ADDRESS);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    *out = g_vo.flip;
    pal_mutex_unlock(g_vo.lock);
    hle_ret_s32(c, 0);
}

static void get_vblank_status(hle_call *c) {
    vo_vblank_status *out = (vo_vblank_status *)hle_mem(c, hle_arg_ptr(c, 1), sizeof(vo_vblank_status));
    if (!check_handle(c, 0) || !out) {
        hle_ret_s32(c, out ? VO_ERR_INVALID_HANDLE : VO_ERR_INVALID_ADDRESS);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    *out = g_vo.vblank;
    pal_mutex_unlock(g_vo.lock);
    hle_ret_s32(c, 0);
}

static void wait_vblank(hle_call *c) {
    if (!check_handle(c, 0)) {
        hle_ret_s32(c, VO_ERR_INVALID_HANDLE);
        return;
    }
    pal_mutex_lock(g_vo.lock);
    uint64_t count = g_vo.vblank.count;
    while (g_vo.vblank.count == count) pal_cond_wait(g_vo.vblank_cond, g_vo.lock);
    pal_mutex_unlock(g_vo.lock);
    hle_ret_s32(c, 0);
}

static void get_output_status(hle_call *c) {
    vo_output_status *out = (vo_output_status *)hle_mem(c, hle_arg_ptr(c, 1), sizeof(vo_output_status));
    if (!check_handle(c, 0) || !out) {
        hle_ret_s32(c, out ? VO_ERR_INVALID_HANDLE : VO_ERR_INVALID_ADDRESS);
        return;
    }
    memset(out, 0, sizeof(*out));
    /* A 1080p TV by default: games that detect the output resolution (param
     * attribute3 bit 2) then render a quarter of the pixels of 4K.
     * A 4K TV when the user settings say so (settings_output_4k). */
    out->resolution = settings_output_4k() ? 2 : 1; /* 1: 1080p, 2: 2160p */
    out->dynamic_range = 1;
    out->refresh_rate = REFRESH_RATE_59_94HZ;
    hle_ret_s32(c, 0);
}

/* PS4: int sceVideoOutGetResolutionStatus(int handle, SceVideoOutResolutionStatus *status): a 1080p TV */
static void get_resolution_status(hle_call *c) {
    vo_resolution_status_ps4 *out =
        (vo_resolution_status_ps4 *)hle_mem(c, hle_arg_ptr(c, 1), sizeof(vo_resolution_status_ps4));
    if (!check_handle(c, 0) || !out) {
        hle_ret_s32(c, out ? VO_ERR_INVALID_HANDLE : VO_ERR_INVALID_ADDRESS);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->full_width = out->pane_width = 1920;
    out->full_height = out->pane_height = 1080;
    out->refresh_rate = REFRESH_RATE_59_94HZ;
    out->screen_size_in_inch = 50.0f;
    hle_ret_s32(c, 0);
}

static void ok_(hle_call *c) { hle_ret_s32(c, 0); }

/* Color settings { float gamma; u32 reserved[3]; }: sceVideoOutColorSettingsSetGamma(settings *, float gamma)
 * stores the gamma, sceVideoOutAdjustColor(handle, const settings *) applies nothing (the host
 * display has its own). The _ variants take the structure size as the last argument. */
static void color_settings_set_gamma(hle_call *c) {
    uint32_t *settings = (uint32_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    if (!settings) {
        hle_ret_s32(c, VO_ERR_INVALID_ADDRESS);
        return;
    }
    settings[0] = (uint32_t)c->a->xmm[0][0];
    hle_ret_s32(c, 0);
}


static void ok_unsupported(hle_call *c) { hle_ret_s32(c, 0); }

static void initialize_output_options(hle_call *c) {
    void *out = hle_mem(c, hle_arg_ptr(c, 0), 64);
    if (out) memset(out, 0, 64);
    hle_ret_s32(c, out ? 0 : VO_ERR_INVALID_ADDRESS);
}

/* ---- Events ---- */

static int32_t add_event(hle_call *c, registration *regs, uint32_t *count, uint64_t ident) {
    uint64_t eq = hle_arg(c, 0);
    if (!check_handle(c, 1)) return VO_ERR_INVALID_HANDLE;
    if (!equeue_add(eq, ident, EVFILT_VIDEO_OUT_, hle_arg(c, 2))) return VO_ERR_INVALID_EVENT_QUEUE;
    pal_mutex_lock(g_vo.lock);
    int32_t ret = 0;
    uint32_t i = 0;
    while (i < *count && regs[i].eq != eq) i++;
    if (i == *count) {
        if (*count == MAX_REGISTRATIONS) ret = VO_ERR_INVALID_EVENT_QUEUE;
        else (*count)++;
    }
    if (ret == 0) {
        regs[i].eq = eq;
        regs[i].udata = hle_arg(c, 2);
    }
    pal_mutex_unlock(g_vo.lock);
    return ret;
}

static int32_t delete_event(hle_call *c, registration *regs, uint32_t *count, uint64_t ident) {
    uint64_t eq = hle_arg(c, 0);
    pal_mutex_lock(g_vo.lock);
    for (uint32_t i = 0; i < *count; i++) {
        if (regs[i].eq == eq) {
            regs[i] = regs[--*count];
            break;
        }
    }
    pal_mutex_unlock(g_vo.lock);
    equeue_remove(eq, ident, EVFILT_VIDEO_OUT_);
    return 0;
}

static void accept_set_mode_event(hle_call *c) { hle_ret_s32(c, 0); }

static void add_flip_event(hle_call *c) {
    hle_ret_s32(c, add_event(c, g_vo.flip_regs, &g_vo.flip_reg_count, VO_EVENT_FLIP));
}
static void add_vblank_event(hle_call *c) {
    hle_ret_s32(c, add_event(c, g_vo.vblank_regs, &g_vo.vblank_reg_count, VO_EVENT_VBLANK));
}
static void delete_flip_event(hle_call *c) {
    hle_ret_s32(c, delete_event(c, g_vo.flip_regs, &g_vo.flip_reg_count, VO_EVENT_FLIP));
}
static void delete_vblank_event(hle_call *c) {
    hle_ret_s32(c, delete_event(c, g_vo.vblank_regs, &g_vo.vblank_reg_count, VO_EVENT_VBLANK));
}

static const guest_kevent *video_event(hle_call *c, int32_t *err) {
    const guest_kevent *ev = (const guest_kevent *)hle_mem(c, hle_arg_ptr(c, 0), sizeof(guest_kevent));
    *err = !ev ? VO_ERR_INVALID_ADDRESS : ev->filter != EVFILT_VIDEO_OUT_ ? VO_ERR_INVALID_EVENT : 0;
    return ev;
}

static void get_event_id(hle_call *c) {
    int32_t err;
    const guest_kevent *ev = video_event(c, &err);
    hle_ret_s32(c, err ? err : ev->ident <= VO_EVENT_PRE_VBLANK_START ? (int32_t)ev->ident : VO_ERR_INVALID_EVENT);
}

/* int sceVideoOutGetEventData(const SceKernelEvent *ev, int64_t *data): payload above bit 16 */
static void get_event_data(hle_call *c) {
    int32_t err;
    const guest_kevent *ev = video_event(c, &err);
    int64_t *out = (int64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (!err && !out) err = VO_ERR_INVALID_ADDRESS;
    if (!err) {
        uint64_t d = (uint64_t)ev->data >> 16;
        if (ev->ident == VO_EVENT_FLIP && ((uint64_t)ev->data & 0x8000000000000000ull)) d |= 0xFFFF000000000000ull;
        *out = (int64_t)d;
    }
    hle_ret_s32(c, err);
}

static void get_event_count(hle_call *c) {
    int32_t err;
    const guest_kevent *ev = video_event(c, &err);
    hle_ret_s32(c, err ? err : (int32_t)(((uint64_t)ev->data >> 12) & 0xF));
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(LIB, "sceVideoOutOpen", open_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutClose", close_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSetBufferAttribute2", set_buffer_attribute2),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutRegisterBuffers2", register_buffers2),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSubmitChangeBufferAttribute2", submit_change_buffer_attribute2),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSetBufferAttribute", set_buffer_attribute), /* PS4 */
    HLE_FUNC_ENTRY(LIB, "sceVideoOutRegisterBuffers", register_buffers),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetResolutionStatus", get_resolution_status),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutUnregisterBuffers", unregister_buffers),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSetFlipRate", set_flip_rate),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSubmitFlip", submit_flip),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutIsFlipPending", is_flip_pending),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetFlipStatus", get_flip_status),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetVblankStatus", get_vblank_status),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutWaitVblank", wait_vblank),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetOutputStatus", get_output_status),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutConfigureOutput", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutInitializeOutputOptions", initialize_output_options),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutColorSettingsSetGamma", color_settings_set_gamma),
    /* int sceVideoOutIsOutputSupported(bus type, output mode, ...): the extra modes games ask
     * about (HDR and high refresh outputs) are not offered: 0 */
    HLE_FUNC_ENTRY(LIB, "sceVideoOutIsOutputSupported", ok_unsupported),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutColorSettingsSetGamma_", color_settings_set_gamma),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutAdjustColor", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutAllowOutputResolutionWqhdDetection", ok_), /* output stays the window's size */
    HLE_FUNC_ENTRY("libSceVideoOutVrrStatus", "#kP2L8t3j-aM", ok_), /* name unknown; fixed refresh rate */
    HLE_FUNC_ENTRY("libSceVideoOutVrrStatus", "#LibwuIonIBw", ok_), /* name unknown: (handle, 1, 0x10), like an event add; the status never changes */
    HLE_FUNC_ENTRY(LIB, "sceVideoOutAddOutputModeEvent", ok_), /* the output mode never changes */
    HLE_FUNC_ENTRY(LIB, "sceVideoOutVrrPegToFixedRate", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutVrrUnpegFromFixedRate", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutAdjustColor_", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSetWindowModeMargins", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutLatencyControlWaitBeforeInput", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutLatencyMeasureSetStartPoint", ok_),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutAddFlipEvent", add_flip_event),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutAddVblankEvent", add_vblank_event),
    /* the output mode never changes here, so a set mode event never fires */
    HLE_FUNC_ENTRY(LIB, "sceVideoOutSysAddSetModeEvent2", accept_set_mode_event),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutDeleteOutputModeEvent", accept_set_mode_event),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutDeleteFlipEvent", delete_flip_event),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutDeleteVblankEvent", delete_vblank_event),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetEventId", get_event_id),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetEventData", get_event_data),
    HLE_FUNC_ENTRY(LIB, "sceVideoOutGetEventCount", get_event_count),
};

void hle_register_video_out(void) {
    if (!g_vo.lock && (pal_mutex_create(&g_vo.lock) != PS5C_OK || pal_cond_create(&g_vo.vblank_cond) != PS5C_OK)) {
        LOG_E(GPU, "cannot create the VideoOut lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
