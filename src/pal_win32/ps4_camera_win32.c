/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PS4 Camera firmware loading over WinUSB (see pal/ps4_camera.h). The loader
 * takes the image in 512-byte vendor control writes (request 0, wValue = offset
 * within a 64 KiB bank, wIndex = bank starting at 0x14), then a write of 0x5B
 * at 0x2200/0x8018 starts it; the device leaves the bus and comes back as
 * 05A9:058B ("USB Camera-OV580", a UVC camera; 058A on some units). Same sequence as the open-source ps4eye loaders. */
#include "pal/ps4_camera.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <setupapi.h>
#include <winusb.h>

#include "base/base.h"
#include "base/log.h"

#define CAM_VID L"VID_05A9"
#define CAM_LOADER L"PID_0580"
#define CAM_READY L"PID_058B"
#define CAM_READY_OLD L"PID_058A"

/* The first present USB device whose instance id names `pid`; its set and data in *set, *info */
static bool find_device(const wchar_t *pid, HDEVINFO *set, SP_DEVINFO_DATA *info) {
    *set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (*set == INVALID_HANDLE_VALUE) return false;
    for (DWORD i = 0;; i++) {
        info->cbSize = sizeof(*info);
        if (!SetupDiEnumDeviceInfo(*set, i, info)) break;
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(*set, info, id, PS5C_ARRAY_COUNT(id), NULL)) continue;
        _wcsupr(id);
        if (wcsstr(id, CAM_VID) && wcsstr(id, pid)) return true;
    }
    SetupDiDestroyDeviceInfoList(*set);
    *set = INVALID_HANDLE_VALUE;
    return false;
}

static bool uses_winusb(HDEVINFO set, SP_DEVINFO_DATA *info) {
    wchar_t service[64] = {0};
    if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_SERVICE, NULL, (BYTE *)service, sizeof(service) - 2, NULL))
        return false;
    return !_wcsicmp(service, L"WinUSB");
}

ps4_camera_state ps4_camera_query(void) {
    HDEVINFO set;
    SP_DEVINFO_DATA info;
    if (find_device(CAM_READY, &set, &info) || find_device(CAM_READY_OLD, &set, &info)) {
        SetupDiDestroyDeviceInfoList(set);
        return PS4_CAMERA_READY;
    }
    if (!find_device(CAM_LOADER, &set, &info)) return PS4_CAMERA_ABSENT;
    bool winusb = uses_winusb(set, &info);
    SetupDiDestroyDeviceInfoList(set);
    return winusb ? PS4_CAMERA_LOADER : PS4_CAMERA_LOADER_NO_DRIVER;
}

const char *ps4_camera_state_name(ps4_camera_state s) {
    switch (s) {
    case PS4_CAMERA_LOADER: return "connected, firmware not loaded";
    case PS4_CAMERA_LOADER_NO_DRIVER: return "connected, no WinUSB driver for the loader (USB Boot 05A9:0580)";
    case PS4_CAMERA_READY: return "ready (firmware running)";
    default: return "not connected";
    }
}

/* The device interface path of the loader: WinUSB registers it under the GUID its INF set */
static bool loader_path(wchar_t *path, size_t cap) {
    HDEVINFO set;
    SP_DEVINFO_DATA info;
    if (!find_device(CAM_LOADER, &set, &info)) return false;
    bool found = false;
    HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    wchar_t guids[512] = {0};
    if (key != INVALID_HANDLE_VALUE) {
        DWORD size = sizeof(guids) - 4;
        if (RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, NULL, (BYTE *)guids, &size) != ERROR_SUCCESS) {
            size = sizeof(guids) - 4;
            if (RegQueryValueExW(key, L"DeviceInterfaceGUID", NULL, NULL, (BYTE *)guids, &size) != ERROR_SUCCESS) guids[0] = 0;
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    GUID guid;
    if (!guids[0] || CLSIDFromString(guids, &guid) != NOERROR) return false; /* the first of the list */
    HDEVINFO ifs = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (ifs == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd = {.cbSize = sizeof(ifd)};
    if (SetupDiEnumDeviceInterfaces(ifs, NULL, &guid, 0, &ifd)) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char raw[2048]; } u;
        u.d.cbSize = sizeof(u.d);
        if (SetupDiGetDeviceInterfaceDetailW(ifs, &ifd, &u.d, sizeof(u), NULL, NULL) && wcslen(u.d.DevicePath) < cap) {
            wcscpy(path, u.d.DevicePath);
            found = true;
        }
    }
    SetupDiDestroyDeviceInfoList(ifs);
    return found;
}

static bool control_out(WINUSB_INTERFACE_HANDLE h, uint16_t value, uint16_t index, const uint8_t *data, uint16_t size) {
    WINUSB_SETUP_PACKET s = {.RequestType = 0x40, .Request = 0, .Value = value, .Index = index, .Length = size};
    ULONG sent = 0;
    return WinUsb_ControlTransfer(h, s, (PUCHAR)data, size, &sent, NULL) && sent == size;
}

int ps4_camera_load_firmware(const uint8_t *fw, size_t size) {
    if (!fw || !size) return -1;
    wchar_t path[1024];
    if (!loader_path(path, PS5C_ARRAY_COUNT(path))) {
        LOG_E(CORE, "PS4 camera: loader device %s", ps4_camera_state_name(ps4_camera_query()));
        return -2;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LOG_E(CORE, "PS4 camera: cannot open the loader device (error %lu)", GetLastError());
        return -3;
    }
    WINUSB_INTERFACE_HANDLE h;
    if (!WinUsb_Initialize(file, &h)) {
        LOG_E(CORE, "PS4 camera: WinUSB refused the loader device (error %lu)", GetLastError());
        CloseHandle(file);
        return -4;
    }
    ULONG timeout = 2000;
    WinUsb_SetPipePolicy(h, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    int rc = 0;
    uint32_t value = 0, index = 0x14;
    for (size_t at = 0; at < size; at += 512) {
        uint16_t n = (uint16_t)(size - at < 512 ? size - at : 512);
        if (!control_out(h, (uint16_t)value, (uint16_t)index, fw + at, n)) {
            LOG_E(CORE, "PS4 camera: firmware write at 0x%zx failed (error %lu)", at, GetLastError());
            rc = -5;
            break;
        }
        value += 512;
        if (value >= 0x10000) value = 0, index++;
    }
    if (!rc) {
        uint8_t start = 0x5B;
        control_out(h, 0x2200, 0x8018, &start, 1); /* the device resets: no reliable reply */
        LOG_I(CORE, "PS4 camera: firmware sent (%zu bytes); the camera restarts as a UVC camera", size);
    }
    WinUsb_Free(h);
    CloseHandle(file);
    return rc;
}
