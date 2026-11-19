/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sony controllers over raw HID. Report layouts follow Linux's
 * drivers/hid/hid-playstation.c:
 *   DualSense input: USB report 0x01 (64 bytes), Bluetooth 0x31 (78 bytes,
 *     data one byte later; Bluetooth sends a short 0x01 report until feature
 *     report 0x05 is read). Output: USB 0x02, Bluetooth 0x31 with a sequence
 *     tag and a CRC-32 (seeded with 0xA2) in the last 4 bytes.
 *   DualShock 4 (USB only here): input 0x01, output 0x05. */
#include "pal_win32/dualsense.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <string.h>

#include "pal/pal.h"

#define SONY_VID 0x054C

typedef struct out_state {
    uint8_t large, small, r, g, b;
} out_state;

static SRWLOCK g_lock = SRWLOCK_INIT;
static ds_state g_state;          /* newest input, g_lock */
static out_state g_out = {0, 0, 0, 0, 255}; /* wanted output, g_lock; blue as on the console */
static HANDLE g_out_event;        /* the output changed */
static HANDLE g_dev = INVALID_HANDLE_VALUE; /* g_lock (for ds_stop_output) */
static ds_kind g_dev_kind;
static bool g_dev_bt;
static uint32_t g_out_len;
static uint8_t g_out_seq;
static SRWLOCK g_start_lock = SRWLOCK_INIT;
static HANDLE g_thread, g_quit; /* g_start_lock */
static volatile LONG g_started;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

/* An output report for the current device in `buf` (g_out_len bytes: Windows
 * wants the longest output report's size). setup: the first one after
 * connecting, which stops the DualSense's own light bar animation. */
static void build_output(uint8_t *buf, const out_state *o, bool setup) {
    memset(buf, 0, g_out_len);
    if (g_dev_kind == DS_DUALSHOCK4) {
        buf[0] = 0x05;
        buf[1] = 0x07; /* rumble, light bar, flash */
        buf[2] = 0x04;
        buf[4] = o->small;
        buf[5] = o->large;
        buf[6] = o->r, buf[7] = o->g, buf[8] = o->b;
        return;
    }
    uint8_t *c;
    if (g_dev_bt) {
        buf[0] = 0x31;
        buf[1] = (uint8_t)(g_out_seq++ << 4);
        buf[2] = 0x10;
        c = buf + 3;
    } else {
        buf[0] = 0x02;
        c = buf + 1;
    }
    if (setup) {
        c[38] = 0x02; /* light bar setup */
        c[41] = 0x02; /* light out: ends the startup animation */
    } else {
        c[0] = 0x01 | 0x02; /* rumble in compatibility mode, haptics select */
        c[38] = 0x04;       /* the same on firmware 2.24 and later */
        c[1] = 0x04 | 0x10; /* light bar, player indicator */
        c[2] = o->small;
        c[3] = o->large;
        c[43] = 0x04; /* player 1: the middle LED */
        c[44] = o->r, c[45] = o->g, c[46] = o->b;
    }
    if (g_dev_bt) {
        static const uint8_t seed = 0xA2;
        uint32_t crc = ~crc32_update(crc32_update(0xFFFFFFFFu, &seed, 1), buf, 74);
        memcpy(buf + 74, &crc, 4);
    }
}

static bool write_report(HANDLE h, const uint8_t *buf) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;
    DWORD done = 0;
    bool ok = WriteFile(h, buf, g_out_len, NULL, &ov) || GetLastError() == ERROR_IO_PENDING;
    if (ok && WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0) {
        CancelIoEx(h, &ov);
        ok = false;
    }
    if (ok) ok = GetOverlappedResult(h, &ov, &done, TRUE) != 0;
    else GetOverlappedResult(h, &ov, &done, TRUE);
    CloseHandle(ov.hEvent);
    return ok;
}

static ds_kind kind_of(USHORT vid, USHORT pid) {
    if (vid != SONY_VID) return DS_NONE;
    switch (pid) {
    case 0x0CE6: return DS_DUALSENSE;
    case 0x0DF2: return DS_DUALSENSE_EDGE;
    case 0x05C4:
    case 0x09CC: return DS_DUALSHOCK4;
    default: return DS_NONE;
    }
}

/* Opens the first supported controller: handle, input report size. */
static HANDLE open_controller(uint32_t *in_len) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA itf;
    itf.cbSize = sizeof(itf);
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &itf); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_W d;
            uint8_t raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024 * sizeof(wchar_t)];
        } detail;
        detail.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &itf, &detail.d, sizeof(detail), NULL, NULL)) continue;
        /* the path names the vendor: skip the rest without opening them */
        if (!wcsstr(detail.d.DevicePath, L"054c") && !wcsstr(detail.d.DevicePath, L"054C")) continue;
        HANDLE h = CreateFileW(detail.d.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        ds_kind kind = DS_NONE;
        if (HidD_GetAttributes(h, &attr)) kind = kind_of(attr.VendorID, attr.ProductID);
        if (kind != DS_NONE && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength >= 64 &&
                caps.InputReportByteLength <= 1024 && caps.OutputReportByteLength >= 32 &&
                caps.OutputReportByteLength <= 1024) {
                bool bt = caps.InputReportByteLength != 64;
                if (!(kind == DS_DUALSHOCK4 && bt) && !(bt && caps.OutputReportByteLength < 78)) {
                    if (bt) { /* reading the calibration switches Bluetooth to the full reports */
                        uint8_t feat[1024];
                        memset(feat, 0, sizeof(feat));
                        feat[0] = 0x05;
                        ULONG n = caps.FeatureReportByteLength;
                        if (n >= 41 && n <= sizeof(feat)) HidD_GetFeature(h, feat, n);
                    }
                    g_dev_kind = kind;
                    g_dev_bt = bt;
                    g_out_len = caps.OutputReportByteLength;
                    *in_len = caps.InputReportByteLength;
                    found = h;
                }
            }
            HidD_FreePreparsedData(pp);
        }
        if (found == INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static const uint32_t k_hat[9] = {
    PAL_PAD_UP, PAL_PAD_UP | PAL_PAD_RIGHT, PAL_PAD_RIGHT, PAL_PAD_DOWN | PAL_PAD_RIGHT, PAL_PAD_DOWN,
    PAL_PAD_DOWN | PAL_PAD_LEFT, PAL_PAD_LEFT, PAL_PAD_UP | PAL_PAD_LEFT, 0,
};

static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static void touch_point(ds_state *s, int i, const uint8_t *p) {
    s->touch[i].down = !(p[0] & 0x80);
    s->touch[i].id = p[0] & 0x7F;
    s->touch[i].x = (uint16_t)(p[1] | (p[2] & 0x0F) << 8);
    s->touch[i].y = (uint16_t)(p[2] >> 4 | p[3] << 4);
}

/* Decodes one input report; false for reports of other kinds. */
static bool parse_report(const uint8_t *buf, uint32_t len, ds_state *s) {
    const uint8_t *p;
    if (g_dev_kind == DS_DUALSHOCK4) {
        if (buf[0] != 0x01 || len < 42) return false;
        p = buf + 1;
    } else if (g_dev_bt) {
        if (buf[0] != 0x31 || len < 78) return false;
        p = buf + 2;
    } else {
        if (buf[0] != 0x01 || len < 64) return false;
        p = buf + 1;
    }
    bool ds4 = g_dev_kind == DS_DUALSHOCK4;
    /* DualShock 4: sticks, 3 button bytes, triggers; DualSense: sticks, triggers, sequence, 4 button bytes */
    const uint8_t *b = p + (ds4 ? 4 : 7);
    s->lx = p[0], s->ly = p[1], s->rx = p[2], s->ry = p[3];
    s->l2 = ds4 ? p[7] : p[4];
    s->r2 = ds4 ? p[8] : p[5];
    uint32_t bt = k_hat[(b[0] & 0x0F) < 8 ? (b[0] & 0x0F) : 8];
    if (b[0] & 0x10) bt |= PAL_PAD_SQUARE;
    if (b[0] & 0x20) bt |= PAL_PAD_CROSS;
    if (b[0] & 0x40) bt |= PAL_PAD_CIRCLE;
    if (b[0] & 0x80) bt |= PAL_PAD_TRIANGLE;
    if (b[1] & 0x01) bt |= PAL_PAD_L1;
    if (b[1] & 0x02) bt |= PAL_PAD_R1;
    if (b[1] & 0x04) bt |= PAL_PAD_L2;
    if (b[1] & 0x08) bt |= PAL_PAD_R2;
    if (b[1] & 0x20) bt |= PAL_PAD_OPTIONS;
    if (b[1] & 0x40) bt |= PAL_PAD_L3;
    if (b[1] & 0x80) bt |= PAL_PAD_R3;
    if (b[2] & 0x02) bt |= PAL_PAD_TOUCH_PAD;
    s->buttons = bt;
    s->create = (b[1] & 0x10) != 0;
    s->ps = (b[2] & 0x01) != 0;
    s->mute = !ds4 && (b[2] & 0x04);
    /* motion, raw: about 1/16 degree per second and 1/8192 G per unit (no calibration) */
    const uint8_t *gyro = p + (ds4 ? 12 : 15), *acc = p + (ds4 ? 18 : 21);
    for (int i = 0; i < 3; i++) {
        s->gyro[i] = (float)le16(gyro + 2 * i) / 16.0f * 0.0174532925f;
        s->accel[i] = (float)le16(acc + 2 * i) / 8192.0f;
    }
    touch_point(s, 0, p + (ds4 ? 34 : 32));
    touch_point(s, 1, p + (ds4 ? 38 : 36));
    s->touch_w = 1920;
    s->touch_h = ds4 ? 942 : 1080;
    uint8_t st = p[ds4 ? 29 : 52], level = st & 0x0F;
    if (ds4) {
        s->charging = (st & 0x10) && level < 11;
        s->battery = (uint8_t)(level >= 10 ? 100 : level * 10 + 5);
    } else {
        uint8_t charge = st >> 4;
        s->charging = charge == 1;
        s->battery = (uint8_t)(charge == 2 ? 100 : level * 10 + 5 > 100 ? 100 : level * 10 + 5);
    }
    return true;
}

static DWORD WINAPI reader_thread(void *arg) {
    (void)arg;
    uint8_t buf[1024], obuf[1024];
    HANDLE rd_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (;;) {
        uint32_t in_len = 0;
        HANDLE h = open_controller(&in_len);
        if (h == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(g_quit, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        AcquireSRWLockExclusive(&g_lock);
        g_dev = h;
        out_state o = g_out;
        ReleaseSRWLockExclusive(&g_lock);
        build_output(obuf, &o, true);
        write_report(h, obuf);
        build_output(obuf, &o, false);
        write_report(h, obuf);
        uint32_t idle_ms = 0, reports = 0;
        bool pending = false, alive = true;
        OVERLAPPED ov;
        while (alive) {
            if (!pending) {
                memset(&ov, 0, sizeof(ov));
                ResetEvent(rd_event);
                ov.hEvent = rd_event;
                if (!ReadFile(h, buf, in_len, NULL, &ov) && GetLastError() != ERROR_IO_PENDING) break;
                pending = true;
            }
            HANDLE waits[3] = {rd_event, g_out_event, g_quit};
            DWORD r = WaitForMultipleObjects(3, waits, FALSE, 250);
            if (r == WAIT_OBJECT_0 + 2) {
                alive = false;
            } else if (r == WAIT_OBJECT_0 + 1) {
                AcquireSRWLockShared(&g_lock);
                o = g_out;
                ReleaseSRWLockShared(&g_lock);
                build_output(obuf, &o, false);
                if (!write_report(h, obuf)) alive = false;
            } else if (r == WAIT_OBJECT_0) {
                DWORD got = 0;
                pending = false;
                if (!GetOverlappedResult(h, &ov, &got, FALSE)) break;
                idle_ms = 0;
                ds_state s;
                AcquireSRWLockShared(&g_lock);
                s = g_state;
                ReleaseSRWLockShared(&g_lock);
                if (parse_report(buf, got, &s)) {
                    s.kind = g_dev_kind;
                    s.bluetooth = g_dev_bt;
                    s.reports = ++reports;
                    AcquireSRWLockExclusive(&g_lock);
                    g_state = s;
                    ReleaseSRWLockExclusive(&g_lock);
                }
            } else if ((idle_ms += 250) >= 3000) { /* a Bluetooth link can drop without an error */
                alive = false;
            }
        }
        if (pending) {
            CancelIoEx(h, &ov);
            DWORD got;
            GetOverlappedResult(h, &ov, &got, TRUE);
        }
        AcquireSRWLockExclusive(&g_lock);
        g_dev = INVALID_HANDLE_VALUE;
        memset(&g_state, 0, sizeof(g_state));
        ReleaseSRWLockExclusive(&g_lock);
        CloseHandle(h);
        if (WaitForSingleObject(g_quit, 500) == WAIT_OBJECT_0) break;
    }
    CloseHandle(rd_event);
    return 0;
}

void ds_start(void) {
    /* fast path: games poll the pad all the time */
    if (g_started) return;
    AcquireSRWLockExclusive(&g_start_lock);
    if (!g_thread) {
        static bool once;
        if (!once) {
            g_out_event = CreateEventW(NULL, FALSE, FALSE, NULL);
            g_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
            atexit(ds_stop_output);
            once = true;
        }
        ResetEvent(g_quit);
        g_thread = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    }
    g_started = g_thread != NULL;
    ReleaseSRWLockExclusive(&g_start_lock);
}

void ds_stop(void) {
    AcquireSRWLockExclusive(&g_start_lock);
    if (g_thread) {
        ds_stop_output();
        SetEvent(g_quit);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
        g_started = 0;
    }
    ReleaseSRWLockExclusive(&g_start_lock);
}

bool ds_read(ds_state *out) {
    AcquireSRWLockShared(&g_lock);
    *out = g_state;
    ReleaseSRWLockShared(&g_lock);
    return out->kind != DS_NONE;
}

static void set_output(const out_state *o) {
    AcquireSRWLockExclusive(&g_lock);
    bool changed = memcmp(&g_out, o, sizeof(*o)) != 0;
    g_out = *o;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed && g_out_event) SetEvent(g_out_event);
}

void ds_set_rumble(uint8_t large, uint8_t small) {
    AcquireSRWLockShared(&g_lock);
    out_state o = g_out;
    ReleaseSRWLockShared(&g_lock);
    o.large = large, o.small = small;
    set_output(&o);
}

void ds_set_light(uint8_t r, uint8_t g, uint8_t b) {
    AcquireSRWLockShared(&g_lock);
    out_state o = g_out;
    ReleaseSRWLockShared(&g_lock);
    o.r = r, o.g = g, o.b = b;
    set_output(&o);
}

void ds_stop_output(void) {
    uint8_t buf[1024];
    AcquireSRWLockExclusive(&g_lock);
    g_out.large = g_out.small = 0;
    out_state o = g_out;
    HANDLE h = g_dev;
    if (h != INVALID_HANDLE_VALUE) build_output(buf, &o, false);
    ReleaseSRWLockExclusive(&g_lock);
    if (h != INVALID_HANDLE_VALUE) write_report(h, buf);
}

const char *ds_name(const ds_state *s) {
    switch (s->kind) {
    case DS_DUALSENSE: return s->bluetooth ? "DualSense (Bluetooth)" : "DualSense (USB)";
    case DS_DUALSENSE_EDGE: return s->bluetooth ? "DualSense Edge (Bluetooth)" : "DualSense Edge (USB)";
    case DS_DUALSHOCK4: return "DualShock 4 (USB)";
    default: return "";
    }
}
