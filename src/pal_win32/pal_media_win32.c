/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Movie decoding through Media Foundation (the fallback when FFmpeg cannot)'s source reader (H.264/AAC in MP4).
 * mfplat.dll and mfreadwrite.dll are loaded at run time: Windows "N" editions
 * ship without them, and the emulator must still start there (no movies). */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>

#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "pal/pal.h"
#include "pal/pal_media_backends.h"

typedef HRESULT(WINAPI *mf_startup_fn)(ULONG, DWORD);
typedef HRESULT(WINAPI *mf_create_attributes_fn)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *mf_create_media_type_fn)(IMFMediaType **);
typedef HRESULT(WINAPI *mf_create_byte_stream_on_stream_fn)(IStream *, IMFByteStream **);
typedef HRESULT(WINAPI *mf_reader_from_url_fn)(LPCWSTR, IMFAttributes *, IMFSourceReader **);
typedef HRESULT(WINAPI *mf_reader_from_stream_fn)(IMFByteStream *, IMFAttributes *, IMFSourceReader **);

static struct {
    INIT_ONCE once;
    bool ok;
    mf_create_attributes_fn create_attributes;
    mf_create_media_type_fn create_media_type;
    mf_create_byte_stream_on_stream_fn byte_stream_on_stream;
    mf_reader_from_url_fn reader_from_url;
    mf_reader_from_stream_fn reader_from_stream;
} g_mf = {INIT_ONCE_STATIC_INIT, false, NULL, NULL, NULL, NULL, NULL};

#define VIDEO_STREAM ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)
#define AUDIO_STREAM ((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM)

struct mf_media {
    IMFSourceReader *reader;
    pal_media_info info;
    uint32_t frame_w, frame_h; /* decoded surface (1920x1088 for a 1080p picture) */
    int32_t stride;            /* from the media type when the buffer has no 2D interface */
    uint32_t crop_x, crop_y;
    bool video_end, audio_end;
    int16_t *pending;          /* decoded PCM not handed out yet */
    uint32_t pending_frames, pending_pos, pending_cap;
    uint64_t pending_time_us;
};

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
    HMODULE plat = LoadLibraryW(L"mfplat.dll"), rw = LoadLibraryW(L"mfreadwrite.dll");
    mf_startup_fn startup;
    LOAD(startup, plat, "MFStartup");
    LOAD(g_mf.create_attributes, plat, "MFCreateAttributes");
    LOAD(g_mf.create_media_type, plat, "MFCreateMediaType");
    LOAD(g_mf.byte_stream_on_stream, plat, "MFCreateMFByteStreamOnStream");
    LOAD(g_mf.reader_from_url, rw, "MFCreateSourceReaderFromURL");
    LOAD(g_mf.reader_from_stream, rw, "MFCreateSourceReaderFromByteStream");
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_mf.ok = startup && g_mf.create_attributes && g_mf.create_media_type && g_mf.byte_stream_on_stream &&
              g_mf.reader_from_url && g_mf.reader_from_stream && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE));
    if (!g_mf.ok) LOG_W(HLE, "movies: Media Foundation is not available (Windows N without the Media Feature Pack?)");
    return TRUE;
}

static bool mf_ready(void) {
    InitOnceExecuteOnce(&g_mf.once, load_mf, NULL, NULL);
    if (g_mf.ok) CoInitializeEx(NULL, COINIT_MULTITHREADED); /* this thread; S_FALSE when it already was */
    return g_mf.ok;
}

static uint64_t attr_u64(IMFMediaType *t, REFGUID key) {
    UINT64 v = 0;
    return SUCCEEDED(IMFMediaType_GetUINT64(t, key, &v)) ? v : 0;
}

static uint32_t attr_u32(IMFMediaType *t, REFGUID key, uint32_t def) {
    UINT32 v = 0;
    return SUCCEEDED(IMFMediaType_GetUINT32(t, key, &v)) ? v : def;
}

/* Reads the negotiated picture layout: surface size, visible area, stride */
static void video_layout(mf_media *m) {
    IMFMediaType *t = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(m->reader, VIDEO_STREAM, &t))) return;
    uint64_t size = attr_u64(t, &MF_MT_FRAME_SIZE);
    m->frame_w = (uint32_t)(size >> 32);
    m->frame_h = (uint32_t)size;
    m->info.width = m->frame_w;
    m->info.height = m->frame_h;
    m->crop_x = m->crop_y = 0;
    MFVideoArea area;
    UINT32 got = 0;
    if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), &got)) &&
        got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
        m->crop_x = (uint32_t)(area.OffsetX.value > 0 ? area.OffsetX.value : 0);
        m->crop_y = (uint32_t)(area.OffsetY.value > 0 ? area.OffsetY.value : 0);
        m->info.width = (uint32_t)area.Area.cx;
        m->info.height = (uint32_t)area.Area.cy;
    }
    m->stride = (int32_t)attr_u32(t, &MF_MT_DEFAULT_STRIDE, m->frame_w);
    uint64_t rate = attr_u64(t, &MF_MT_FRAME_RATE);
    if ((uint32_t)rate) m->info.frame_rate = (double)(rate >> 32) / (double)(uint32_t)rate;
    IMFMediaType_Release(t);
}

static bool select_output(mf_media *m, DWORD stream, REFGUID major, REFGUID subtype, uint32_t channels, uint32_t rate) {
    IMFMediaType *t = NULL;
    if (FAILED(g_mf.create_media_type(&t))) return false;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, major);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, subtype);
    if (channels) {
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, channels);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    }
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(m->reader, stream, NULL, t);
    IMFMediaType_Release(t);
    return SUCCEEDED(hr);
}

static bool setup_audio(mf_media *m) {
    IMFMediaType *native = NULL;
    if (FAILED(IMFSourceReader_GetNativeMediaType(m->reader, AUDIO_STREAM, 0, &native))) return false;
    uint32_t channels = attr_u32(native, &MF_MT_AUDIO_NUM_CHANNELS, 2);
    uint32_t rate = attr_u32(native, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    IMFMediaType_Release(native);
    if (channels > 2) channels = 2;
    /* 16-bit PCM at the source's rate; the decoder downmixes to stereo */
    if (!select_output(m, AUDIO_STREAM, &MFMediaType_Audio, &MFAudioFormat_PCM, channels, rate) &&
        !select_output(m, AUDIO_STREAM, &MFMediaType_Audio, &MFAudioFormat_PCM, 2, 48000))
        return false;
    IMFMediaType *t = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(m->reader, AUDIO_STREAM, &t))) return false;
    m->info.channels = attr_u32(t, &MF_MT_AUDIO_NUM_CHANNELS, 0);
    m->info.sample_rate = attr_u32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    uint32_t bits = attr_u32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    IMFMediaType_Release(t);
    return bits == 16 && m->info.channels >= 1 && m->info.channels <= 2;
}

static mf_media *finish_open(IMFSourceReader *reader) {
    mf_media *m = (mf_media *)calloc(1, sizeof(mf_media));
    if (!m) {
        IMFSourceReader_Release(reader);
        return NULL;
    }
    m->reader = reader;
    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(IMFSourceReader_SetStreamSelection(reader, VIDEO_STREAM, TRUE)) &&
        select_output(m, VIDEO_STREAM, &MFMediaType_Video, &MFVideoFormat_NV12, 0, 0)) {
        video_layout(m);
    } else {
        IMFSourceReader_SetStreamSelection(reader, VIDEO_STREAM, FALSE);
        m->video_end = true;
    }
    if (FAILED(IMFSourceReader_SetStreamSelection(reader, AUDIO_STREAM, TRUE)) || !setup_audio(m)) {
        IMFSourceReader_SetStreamSelection(reader, AUDIO_STREAM, FALSE);
        m->info.channels = m->info.sample_rate = 0;
        m->audio_end = true;
    }
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(reader, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &v)) &&
        v.vt == VT_UI8)
        m->info.duration_us = v.uhVal.QuadPart / 10;
    PropVariantClear(&v);
    if (!m->info.width && !m->info.channels) {
        mf_media_close(m);
        return NULL;
    }
    return m;
}

static IMFAttributes *reader_attributes(void) {
    IMFAttributes *a = NULL;
    if (FAILED(g_mf.create_attributes(&a, 2))) return NULL;
    IMFAttributes_SetUINT32(a, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    return a;
}

mf_media *mf_media_open_file(const char *path) {
    if (!mf_ready()) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wide = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (!wide) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
    for (wchar_t *p = wide; *p; p++)
        if (*p == L'/') *p = L'\\';
    IMFAttributes *a = reader_attributes();
    IMFSourceReader *reader = NULL;
    HRESULT hr = g_mf.reader_from_url(wide, a, &reader);
    if (a) IMFAttributes_Release(a);
    free(wide);
    if (FAILED(hr)) {
        LOG_W(HLE, "movies: cannot open %s (0x%08lx)", path, (unsigned long)hr);
        return NULL;
    }
    return finish_open(reader);
}

mf_media *mf_media_open_memory(const void *data, uint64_t size) {
    if (!mf_ready() || size > 0xFFFFFFFFu) return NULL;
    IStream *s = SHCreateMemStream((const BYTE *)data, (UINT)size);
    IMFByteStream *bs = NULL;
    if (!s || FAILED(g_mf.byte_stream_on_stream(s, &bs))) {
        if (s) IStream_Release(s);
        return NULL;
    }
    IStream_Release(s);
    IMFAttributes *a = reader_attributes();
    IMFSourceReader *reader = NULL;
    HRESULT hr = g_mf.reader_from_stream(bs, a, &reader);
    if (a) IMFAttributes_Release(a);
    IMFByteStream_Release(bs);
    if (FAILED(hr)) {
        LOG_W(HLE, "movies: cannot decode a %llu-byte movie (0x%08lx)", (unsigned long long)size, (unsigned long)hr);
        return NULL;
    }
    return finish_open(reader);
}

void mf_media_get_info(mf_media *m, pal_media_info *out) { *out = m->info; }

/* The next sample of `stream`: 1 = sample, 0 = end, -1 = error */
static int read_sample(mf_media *m, DWORD stream, IMFSample **out, uint64_t *time_us) {
    for (;;) {
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(m->reader, stream, 0, &index, &flags, &ts, &s);
        if (FAILED(hr)) return -1;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED && stream == VIDEO_STREAM) video_layout(m);
        if (s) {
            *out = s;
            *time_us = ts > 0 ? (uint64_t)ts / 10 : 0;
            return 1;
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return 0;
    }
}

int mf_media_next_video(mf_media *m, uint8_t *nv12, uint64_t *time_us) {
    if (m->video_end) return 0;
    IMFSample *s = NULL;
    int r = read_sample(m, VIDEO_STREAM, &s, time_us);
    if (r <= 0) {
        m->video_end = true;
        return r;
    }
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) {
        IMFSample_Release(s);
        return -1;
    }
    IMF2DBuffer *b2 = NULL;
    BYTE *base = NULL;
    LONG pitch = m->stride;
    DWORD len = 0;
    bool locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
                    SUCCEEDED(IMF2DBuffer_Lock2D(b2, &base, &pitch));
    if (!locked2d && FAILED(IMFMediaBuffer_Lock(buf, &base, NULL, &len))) base = NULL;
    uint32_t w = m->info.width, h = m->info.height;
    if (base && pitch > 0 && (uint32_t)pitch >= w) {
        const BYTE *luma = base + (size_t)m->crop_y * (size_t)pitch + m->crop_x;
        const BYTE *chroma = base + (size_t)pitch * m->frame_h + (size_t)(m->crop_y / 2) * (size_t)pitch + (m->crop_x & ~1u);
        for (uint32_t y = 0; y < h; y++) memcpy(nv12 + (size_t)y * w, luma + (size_t)y * (size_t)pitch, w);
        uint8_t *uv = nv12 + (size_t)w * h;
        for (uint32_t y = 0; y < h / 2; y++) memcpy(uv + (size_t)y * w, chroma + (size_t)y * (size_t)pitch, w);
        r = 1;
    } else {
        r = -1;
    }
    if (locked2d) IMF2DBuffer_Unlock2D(b2);
    else if (base) IMFMediaBuffer_Unlock(buf);
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(s);
    return r;
}

int mf_media_next_audio(mf_media *m, int16_t *pcm, uint32_t max_frames, uint64_t *time_us) {
    uint32_t ch = m->info.channels;
    if (!ch) return 0;
    while (m->pending_pos >= m->pending_frames) {
        if (m->audio_end) return 0;
        IMFSample *s = NULL;
        uint64_t ts = 0;
        int r = read_sample(m, AUDIO_STREAM, &s, &ts);
        if (r <= 0) {
            m->audio_end = true;
            return r;
        }
        IMFMediaBuffer *buf = NULL;
        BYTE *data = NULL;
        DWORD len = 0;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &buf)) && SUCCEEDED(IMFMediaBuffer_Lock(buf, &data, NULL, &len))) {
            uint32_t frames = len / (ch * 2);
            if (frames > m->pending_cap) {
                int16_t *grown = (int16_t *)realloc(m->pending, (size_t)frames * ch * 2);
                if (grown) {
                    m->pending = grown;
                    m->pending_cap = frames;
                }
            }
            if (frames > m->pending_cap) frames = m->pending_cap;
            if (frames) memcpy(m->pending, data, (size_t)frames * ch * 2);
            m->pending_frames = frames;
            m->pending_pos = 0;
            m->pending_time_us = ts;
            IMFMediaBuffer_Unlock(buf);
        }
        if (buf) IMFMediaBuffer_Release(buf);
        IMFSample_Release(s);
    }
    uint32_t n = m->pending_frames - m->pending_pos;
    if (n > max_frames) n = max_frames;
    memcpy(pcm, m->pending + (size_t)m->pending_pos * ch, (size_t)n * ch * 2);
    *time_us = m->pending_time_us + (uint64_t)m->pending_pos * 1000000u / m->info.sample_rate;
    m->pending_pos += n;
    return (int)n;
}

bool mf_media_rewind(mf_media *m) {
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = 0;
    HRESULT hr = IMFSourceReader_SetCurrentPosition(m->reader, &GUID_NULL, &v);
    if (FAILED(hr)) return false;
    m->video_end = m->info.width == 0;
    m->audio_end = m->info.channels == 0;
    m->pending_frames = m->pending_pos = 0;
    return true;
}

void mf_media_close(mf_media *m) {
    if (!m) return;
    if (m->reader) IMFSourceReader_Release(m->reader);
    free(m->pending);
    free(m);
}
