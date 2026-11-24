/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host cameras through Media Foundation (pal_camera.h). The libraries are loaded
 * at run time, as for movies: Windows "N" editions ship without them. The source
 * reader converts what the camera sends (MJPEG, NV12, YUY2...) to RGB32; a
 * thread keeps the newest picture, readers scale it to YUYV.
 * A PS4 Camera (OV580) is read as it comes: YUY2 3448x808 at 60 frames a second,
 * rows of a 48-pixel header, the left lens (1280 pixels), the right lens, then
 * smaller copies; rows 0..799 hold the lenses. Each lens is kept as YUYV 1280x800,
 * the PS camera library's own picture. Opened by name, a camera still waiting for
 * its firmware (USB Boot) gets the user's file first (pal/ps4_camera.h). */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal_camera.h"
#include "pal/ps4_camera.h"

#include <stdio.h>

#define PS4_FRAME_W 3448u
#define PS4_FRAME_H 808u
#define PS4_LENS_X 48u /* the left lens; the right one follows it */
#define PS4_LENS_W 1280u
#define PS4_LENS_H 800u
#define PS4_FIRMWARE "user/ps4_camera_firmware.bin"

typedef HRESULT(WINAPI *startup_fn)(ULONG, DWORD);
typedef HRESULT(WINAPI *create_attributes_fn)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *create_media_type_fn)(IMFMediaType **);
typedef HRESULT(WINAPI *enum_devices_fn)(IMFAttributes *, IMFActivate ***, UINT32 *);
typedef HRESULT(WINAPI *reader_from_source_fn)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **);

static struct {
    INIT_ONCE once;
    bool ok;
    create_attributes_fn create_attributes;
    create_media_type_fn create_media_type;
    enum_devices_fn enum_devices;
    reader_from_source_fn reader_from_source;
} g_mf = {INIT_ONCE_STATIC_INIT, false, NULL, NULL, NULL, NULL};

#define VIDEO_STREAM ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)

/* GetProcAddress into a typed function pointer (-Wpedantic: no casts between them) */
#define LOAD(dst, module, name)                                          \
    do {                                                                 \
        FARPROC p_ = (module) ? GetProcAddress((module), (name)) : NULL; \
        memcpy(&(dst), &p_, sizeof(dst));                                \
    } while (0)

static BOOL CALLBACK load_mf(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    HMODULE plat = LoadLibraryW(L"mfplat.dll"), mf = LoadLibraryW(L"mf.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
    startup_fn startup;
    LOAD(startup, plat, "MFStartup");
    LOAD(g_mf.create_attributes, plat, "MFCreateAttributes");
    LOAD(g_mf.create_media_type, plat, "MFCreateMediaType");
    LOAD(g_mf.enum_devices, mf, "MFEnumDeviceSources");
    LOAD(g_mf.reader_from_source, rw, "MFCreateSourceReaderFromMediaSource");
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_mf.ok = startup && g_mf.create_attributes && g_mf.create_media_type && g_mf.enum_devices && g_mf.reader_from_source &&
              SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE));
    if (!g_mf.ok) LOG_W(HLE, "camera: Media Foundation is not available (Windows N without the Media Feature Pack?)");
    return TRUE;
}

static bool mf_ready(void) {
    InitOnceExecuteOnce(&g_mf.once, load_mf, NULL, NULL);
    if (g_mf.ok) CoInitializeEx(NULL, COINIT_MULTITHREADED); /* this thread; S_FALSE when it already was */
    return g_mf.ok;
}

struct pal_camera {
    IMFActivate *activate;
    IMFMediaSource *source;
    IMFSourceReader *reader;
    HANDLE thread;
    volatile LONG stop;
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    uint8_t *bgra; /* lock: the newest picture, top-down rows of w * 4 bytes */
    uint32_t w, h;
    uint64_t seq;
    int32_t stride; /* from the media type, for buffers without a 2D interface */
    char name[128];
    bool ps4;          /* a PS4 Camera: raw YUY2, two lenses */
    uint8_t *lens[2];  /* lock (ps4): the newest picture of each lens, YUYV 1280x800 */
};

/* The video capture devices; the caller releases each and CoTaskMemFrees the list */
static bool devices(IMFActivate ***list, UINT32 *count) {
    IMFAttributes *a = NULL;
    *list = NULL;
    *count = 0;
    if (FAILED(g_mf.create_attributes(&a, 1))) return false;
    IMFAttributes_SetGUID(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    HRESULT hr = g_mf.enum_devices(a, list, count);
    IMFAttributes_Release(a);
    return SUCCEEDED(hr);
}

static void device_name(IMFActivate *act, char *out, size_t size) {
    WCHAR *w = NULL;
    UINT32 len = 0;
    out[0] = 0;
    if (SUCCEEDED(IMFActivate_GetAllocatedString(act, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &w, &len)) && w) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL);
        out[size - 1] = 0;
        CoTaskMemFree(w);
    }
}

static bool contains_nocase(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s; s++)
        if (!_strnicmp(s, what, n)) return true;
    return false;
}

/* The PS4 camera's own device: its picture is both lenses side by side at 3448x808 */
static bool ps4_camera_device(const char *name) { return contains_nocase(name, "OV580"); }

uint32_t pal_camera_list(char (*names)[128], uint32_t max) {
    if (!mf_ready()) return 0;
    IMFActivate **list;
    UINT32 count;
    if (!devices(&list, &count)) return 0;
    for (UINT32 i = 0; i < count; i++) {
        if (i < max) device_name(list[i], names[i], sizeof(names[i]));
        IMFActivate_Release(list[i]);
    }
    CoTaskMemFree(list);
    return count;
}

static uint64_t type_u64(IMFMediaType *t, REFGUID key) {
    UINT64 v = 0;
    return SUCCEEDED(IMFMediaType_GetUINT64(t, key, &v)) ? v : 0;
}

/* The native format closest to 1280x720 at 25 frames a second or more (the PS camera
 * picture is 1280x800): less work converting than a full HD one, sharper than VGA */
static void choose_native_format(pal_camera *c) {
    IMFMediaType *best = NULL;
    uint64_t best_score = UINT64_MAX;
    for (DWORD k = 0;; k++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFSourceReader_GetNativeMediaType(c->reader, VIDEO_STREAM, k, &t))) break;
        uint64_t size = type_u64(t, &MF_MT_FRAME_SIZE), rate = type_u64(t, &MF_MT_FRAME_RATE);
        uint32_t w = (uint32_t)(size >> 32), h = (uint32_t)size;
        double fps = (uint32_t)rate ? (double)(rate >> 32) / (double)(uint32_t)rate : 0.0;
        uint64_t area = (uint64_t)w * h, want = 1280ull * 720;
        uint64_t score = (area > want ? area - want : want - area) + (fps < 24.5 ? (1ull << 40) : 0);
        if (w && h && score < best_score) {
            if (best) IMFMediaType_Release(best);
            best = t, best_score = score;
        } else {
            IMFMediaType_Release(t);
        }
    }
    if (best) {
        IMFSourceReader_SetCurrentMediaType(c->reader, VIDEO_STREAM, NULL, best);
        IMFMediaType_Release(best);
    }
}

/* PS4 Camera: its full-size YUY2 picture at the highest rate it offers */
static bool choose_ps4_format(pal_camera *c) {
    IMFMediaType *best = NULL;
    double best_fps = 0.0;
    for (DWORD k = 0;; k++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFSourceReader_GetNativeMediaType(c->reader, VIDEO_STREAM, k, &t))) break;
        GUID sub;
        uint64_t size = type_u64(t, &MF_MT_FRAME_SIZE), rate = type_u64(t, &MF_MT_FRAME_RATE);
        double fps = (uint32_t)rate ? (double)(rate >> 32) / (double)(uint32_t)rate : 0.0;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_YUY2) &&
            (uint32_t)(size >> 32) == PS4_FRAME_W && (uint32_t)size == PS4_FRAME_H && fps > best_fps) {
            if (best) IMFMediaType_Release(best);
            best = t, best_fps = fps;
        } else {
            IMFMediaType_Release(t);
        }
    }
    bool ok = best && SUCCEEDED(IMFSourceReader_SetCurrentMediaType(c->reader, VIDEO_STREAM, NULL, best));
    if (best) IMFMediaType_Release(best);
    if (!ok) return false;
    c->w = PS4_FRAME_W, c->h = PS4_FRAME_H, c->stride = (int32_t)(PS4_FRAME_W * 2);
    for (int e = 0; e < 2; e++) c->lens[e] = (uint8_t *)calloc((size_t)PS4_LENS_W * PS4_LENS_H, 2);
    return c->lens[0] && c->lens[1];
}

/* A PS4 Camera still waiting for its firmware (USB Boot) gets the user's file, then
 * comes back as a camera a few seconds later. False: nothing was sent. */
static bool ps4_camera_start(void) {
    ps4_camera_state s = ps4_camera_query();
    if (s == PS4_CAMERA_LOADER_NO_DRIVER) {
        LOG_W(HLE, "camera: the PS4 Camera waits for its firmware, but its loader (USB Boot 05A9:0580) has no "
                   "WinUSB driver (install it with Zadig)");
        return false;
    }
    if (s != PS4_CAMERA_LOADER) return false;
    FILE *f = fopen(PS4_FIRMWARE, "rb");
    if (!f) {
        LOG_W(HLE, "camera: the PS4 Camera waits for its firmware: put your copy at %s", PS4_FIRMWARE);
        return false;
    }
    static uint8_t fw[1u << 20];
    size_t n = fread(fw, 1, sizeof(fw), f);
    fclose(f);
    if (!n || n == sizeof(fw) || ps4_camera_load_firmware(fw, n)) return false;
    for (int i = 0; i < 80 && ps4_camera_query() != PS4_CAMERA_READY; i++) Sleep(100);
    return true;
}

static bool read_layout(pal_camera *c) {
    if (c->ps4) return true; /* fixed: choose_ps4_format */
    IMFMediaType *t = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(c->reader, VIDEO_STREAM, &t))) return false;
    uint64_t size = type_u64(t, &MF_MT_FRAME_SIZE);
    UINT32 stride = 0;
    uint32_t w = (uint32_t)(size >> 32), h = (uint32_t)size;
    c->stride = SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &stride)) ? (int32_t)stride : (int32_t)(w * 4);
    IMFMediaType_Release(t);
    if (!w || !h || w > 8192 || h > 8192) return false;
    AcquireSRWLockExclusive(&c->lock);
    if (w != c->w || h != c->h) {
        uint8_t *p = (uint8_t *)realloc(c->bgra, (size_t)w * h * 4);
        if (p) c->bgra = p, c->w = w, c->h = h;
    }
    bool ok = c->bgra && c->w == w;
    ReleaseSRWLockExclusive(&c->lock);
    return ok;
}

/* PS4 Camera: rows 0..799 of each lens, as they come */
static void store_ps4_sample(pal_camera *c, IMFSample *sample) {
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) return;
    BYTE *p = NULL;
    DWORD len = 0;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
        if (len >= (DWORD)(PS4_FRAME_W * 2 * PS4_LENS_H)) {
            AcquireSRWLockExclusive(&c->lock);
            for (uint32_t y = 0; y < PS4_LENS_H; y++)
                for (uint32_t e = 0; e < 2; e++)
                    memcpy(c->lens[e] + (size_t)y * PS4_LENS_W * 2,
                           p + ((size_t)y * PS4_FRAME_W + PS4_LENS_X + e * PS4_LENS_W) * 2, PS4_LENS_W * 2);
            c->seq++;
            WakeAllConditionVariable(&c->changed);
            ReleaseSRWLockExclusive(&c->lock);
        }
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_Release(buf);
}

static void store_sample(pal_camera *c, IMFSample *sample) {
    if (c->ps4) {
        store_ps4_sample(c, sample);
        return;
    }
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) return;
    IMF2DBuffer *b2 = NULL;
    BYTE *scan0 = NULL;
    LONG pitch = 0;
    DWORD len = 0;
    bool locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
                    SUCCEEDED(IMF2DBuffer_Lock2D(b2, &scan0, &pitch));
    if (!locked2d) {
        if (b2) IMF2DBuffer_Release(b2), b2 = NULL;
        if (FAILED(IMFMediaBuffer_Lock(buf, &scan0, NULL, &len))) scan0 = NULL;
        pitch = c->stride;
        /* a negative stride: bottom-up rows, the first one at the end */
        if (scan0 && pitch < 0) scan0 += (size_t)(-pitch) * (c->h - 1);
    }
    AcquireSRWLockExclusive(&c->lock);
    if (scan0 && c->bgra && (locked2d || len >= (DWORD)((size_t)(pitch < 0 ? -pitch : pitch) * c->h))) {
        for (uint32_t y = 0; y < c->h; y++) memcpy(c->bgra + (size_t)y * c->w * 4, scan0 + (ptrdiff_t)y * pitch, (size_t)c->w * 4);
        c->seq++;
        WakeAllConditionVariable(&c->changed);
    }
    ReleaseSRWLockExclusive(&c->lock);
    if (locked2d) IMF2DBuffer_Unlock2D(b2);
    else if (scan0) IMFMediaBuffer_Unlock(buf);
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(buf);
}

static DWORD WINAPI capture_thread(LPVOID arg) {
    pal_camera *c = (pal_camera *)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    uint32_t failures = 0;
    while (!c->stop) {
        DWORD stream = 0, flags = 0;
        LONGLONG time = 0;
        IMFSample *sample = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(c->reader, VIDEO_STREAM, 0, &stream, &flags, &time, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            if (++failures == 1) LOG_W(HLE, "camera '%s': capture stopped (0x%08lx)", c->name, (unsigned long)hr);
            if (sample) IMFSample_Release(sample);
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) read_layout(c);
        if (sample) {
            store_sample(c, sample);
            IMFSample_Release(sample);
        }
    }
    AcquireSRWLockExclusive(&c->lock);
    WakeAllConditionVariable(&c->changed);
    ReleaseSRWLockExclusive(&c->lock);
    CoUninitialize();
    return 0;
}

/* The capture device for `match` (see pal_camera_open) into c->activate and c->name */
static UINT32 find_camera(pal_camera *c, const char *match) {
    IMFActivate **list;
    UINT32 count;
    if (!devices(&list, &count)) return 0;
    for (UINT32 i = 0; i < count; i++) {
        char name[128];
        device_name(list[i], name, sizeof(name));
        bool wanted = match && *match ? contains_nocase(name, match) : !ps4_camera_device(name);
        if (!c->activate && wanted) {
            c->activate = list[i];
            memcpy(c->name, name, sizeof(name));
        } else {
            IMFActivate_Release(list[i]);
        }
    }
    CoTaskMemFree(list);
    return count;
}

pal_camera *pal_camera_open(const char *match) {
    if (!mf_ready()) return NULL;
    pal_camera *c = (pal_camera *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    UINT32 count = find_camera(c, match);
    /* asked for the PS4 Camera by name: it may still wait for its firmware */
    if (!c->activate && match && ps4_camera_device(match) && ps4_camera_start()) {
        for (int i = 0; i < 30 && !c->activate; i++) { /* Media Foundation lists it a little after Plug and Play */
            Sleep(100);
            count = find_camera(c, match);
        }
    }
    if (!c->activate) {
        LOG_I(HLE, "camera: no host camera%s%s%s (%u found)", match && *match ? " named like '" : "",
              match && *match ? match : "", match && *match ? "'" : "", (unsigned)count);
        free(c);
        return NULL;
    }
    InitializeSRWLock(&c->lock);
    InitializeConditionVariable(&c->changed);
    IMFAttributes *a = NULL;
    bool ok = SUCCEEDED(IMFActivate_ActivateObject(c->activate, &IID_IMFMediaSource, (void **)&c->source)) &&
              SUCCEEDED(g_mf.create_attributes(&a, 1));
    if (ok) {
        IMFAttributes_SetUINT32(a, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ok = SUCCEEDED(g_mf.reader_from_source(c->source, a, &c->reader));
    }
    if (a) IMFAttributes_Release(a);
    if (ok) {
        c->ps4 = ps4_camera_device(c->name);
        IMFMediaType *t = NULL;
        if (c->ps4) ok = choose_ps4_format(c);
        else choose_native_format(c);
        ok = ok && (c->ps4 || SUCCEEDED(g_mf.create_media_type(&t)));
        if (ok && t) {
            IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
            IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
            ok = SUCCEEDED(IMFSourceReader_SetCurrentMediaType(c->reader, VIDEO_STREAM, NULL, t));
            IMFMediaType_Release(t);
        }
        ok = ok && read_layout(c);
    }
    if (ok) c->thread = CreateThread(NULL, 0, capture_thread, c, 0, NULL);
    if (!ok || !c->thread) {
        LOG_W(HLE, "camera '%s': cannot capture from it", c->name);
        pal_camera_close(c);
        return NULL;
    }
    LOG_I(HLE, "camera: capturing from '%s' at %ux%u%s", c->name, c->w, c->h, c->ps4 ? " (PS4 Camera: two lenses)" : "");
    return c;
}

void pal_camera_close(pal_camera *c) {
    if (!c) return;
    InterlockedExchange(&c->stop, 1);
    if (c->thread) {
        /* ReadSample returns with the next picture; Shutdown ends a stalled one */
        if (WaitForSingleObject(c->thread, 1000) != WAIT_OBJECT_0 && c->source) IMFMediaSource_Shutdown(c->source);
        WaitForSingleObject(c->thread, 2000);
        CloseHandle(c->thread);
    }
    if (c->reader) IMFSourceReader_Release(c->reader);
    if (c->source) {
        IMFMediaSource_Shutdown(c->source);
        IMFMediaSource_Release(c->source);
    }
    if (c->activate) {
        IMFActivate_ShutdownObject(c->activate);
        IMFActivate_Release(c->activate);
    }
    free(c->bgra);
    free(c->lens[0]);
    free(c->lens[1]);
    free(c);
}

bool pal_camera_stereo(const pal_camera *c) { return c && c->ps4; }

bool pal_camera_ps4_waiting(void) { return ps4_camera_query() == PS4_CAMERA_LOADER; }

static inline uint8_t clamp_u8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* A lens picture (YUYV 1280x800) at w x h: copied, or nearest pixel pairs */
static void read_lens(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h) {
    if (w == PS4_LENS_W && h == PS4_LENS_H) {
        memcpy(dst, src, (size_t)w * h * 2);
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)((uint64_t)y * PS4_LENS_H / h) * PS4_LENS_W * 2;
        uint8_t *out = dst + (size_t)y * w * 2;
        for (uint32_t x = 0; x < w; x += 2, out += 4) {
            uint32_t s0 = (uint32_t)((uint64_t)x * PS4_LENS_W / w), s1 = (uint32_t)((uint64_t)(x + 1) * PS4_LENS_W / w);
            const uint8_t *pair = row + (size_t)(s0 & ~1u) * 2; /* Y0 U Y1 V */
            out[0] = row[(size_t)s0 * 2];
            out[1] = pair[1];
            out[2] = row[(size_t)s1 * 2];
            out[3] = pair[3];
        }
    }
}

bool pal_camera_read(pal_camera *c, uint8_t *dst, uint32_t w, uint32_t h, uint64_t *seq, uint32_t wait_ms) {
    return pal_camera_read_eye(c, 0, dst, w, h, seq, wait_ms);
}

bool pal_camera_read_eye(pal_camera *c, uint32_t eye, uint8_t *dst, uint32_t w, uint32_t h, uint64_t *seq,
                         uint32_t wait_ms) {
    if (!c || !w || !h || (w & 1)) return false;
    AcquireSRWLockExclusive(&c->lock);
    ULONGLONG deadline = GetTickCount64() + wait_ms;
    while (c->seq <= *seq && !c->stop) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline || !SleepConditionVariableSRW(&c->changed, &c->lock, (DWORD)(deadline - now), 0)) break;
    }
    if (c->ps4) {
        bool have = c->seq != 0;
        if (have) read_lens(c->lens[eye & 1], dst, w, h), *seq = c->seq;
        ReleaseSRWLockExclusive(&c->lock);
        return have;
    }
    bool have = c->seq != 0 && c->bgra;
    if (have) {
        /* the middle of the picture at the asked aspect ratio, nearest pixels, BT.601 */
        uint32_t sw = c->w, sh = c->h, cx = 0, cy = 0, cw = sw, ch = sh;
        if ((uint64_t)sw * h > (uint64_t)sh * w) cw = (uint32_t)((uint64_t)sh * w / h), cx = (sw - cw) / 2;
        else ch = (uint32_t)((uint64_t)sw * h / w), cy = (sh - ch) / 2;
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = c->bgra + (size_t)(cy + (uint64_t)y * ch / h) * sw * 4;
            uint8_t *out = dst + (size_t)y * w * 2;
            for (uint32_t x = 0; x < w; x += 2) {
                const uint8_t *p0 = row + (size_t)(cx + (uint64_t)x * cw / w) * 4;
                const uint8_t *p1 = row + (size_t)(cx + (uint64_t)(x + 1) * cw / w) * 4;
                int b = p0[0] + p1[0], g = p0[1] + p1[1], r = p0[2] + p1[2]; /* sums of the pair */
                out[0] = clamp_u8(((66 * p0[2] + 129 * p0[1] + 25 * p0[0] + 128) >> 8) + 16);
                out[1] = clamp_u8(((-38 * r - 74 * g + 112 * b + 256) >> 9) + 128);
                out[2] = clamp_u8(((66 * p1[2] + 129 * p1[1] + 25 * p1[0] + 128) >> 8) + 16);
                out[3] = clamp_u8(((112 * r - 94 * g - 18 * b + 256) >> 9) + 128);
                out += 4;
            }
        }
        *seq = c->seq;
    }
    ReleaseSRWLockExclusive(&c->lock);
    return have;
}
