/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include "pal/native_input.h"
#include "pal/pal.h"
#include "pal_win32/native_input_win32.h"

static bool g_captured;
static uint32_t g_buttons;
/* Physical set-1 scan codes -> USB HID keyboard usages. Layout-dependent
 * characters are translated separately by Windows. */
static const uint8_t scan_hid[128] = {
    [1]=41,[2]=30,[3]=31,[4]=32,[5]=33,[6]=34,[7]=35,[8]=36,[9]=37,[10]=38,[11]=39,
    [12]=45,[13]=46,[14]=42,[15]=43,[16]=20,[17]=26,[18]=8,[19]=21,[20]=23,[21]=28,
    [22]=24,[23]=12,[24]=18,[25]=19,[26]=47,[27]=48,[28]=40,[29]=224,[30]=4,[31]=22,
    [32]=7,[33]=9,[34]=10,[35]=11,[36]=13,[37]=14,[38]=15,[39]=51,[40]=52,[41]=53,
    [42]=225,[43]=49,[44]=29,[45]=27,[46]=6,[47]=25,[48]=5,[49]=17,[50]=16,[51]=54,
    [52]=55,[53]=56,[54]=229,[55]=85,[56]=226,[57]=44,[58]=57,[59]=58,[60]=59,
    [61]=60,[62]=61,[63]=62,[64]=63,[65]=64,[66]=65,[67]=66,[68]=67,[69]=83,[70]=71,
    [71]=95,[72]=96,[73]=97,[74]=86,[75]=92,[76]=93,[77]=94,[78]=87,[79]=89,[80]=90,
    [81]=91,[82]=98,[83]=99,[86]=100,[87]=68,[88]=69,[115]=135,[126]=133
};
static uint16_t key_hid(LPARAM lp) {
    unsigned scan = ((uint64_t)lp >> 16) & 127;
    if (lp & (1u << 24)) switch (scan) {
    case 28:return 88; case 29:return 228; case 53:return 84; case 56:return 230;
    case 71:return 74; case 72:return 82; case 73:return 75; case 75:return 80;
    case 77:return 79; case 79:return 77; case 80:return 81; case 81:return 78;
    case 82:return 73; case 83:return 76; case 91:return 227; case 92:return 231; case 93:return 101;
    default:return 0;
    }
    return scan_hid[scan];
}
static void capture(HWND window, bool on) {
    if (on == g_captured) return;
    g_captured = on; g_buttons = 0; native_input_release();
    if (on) {
        RECT r; GetClientRect(window, &r); MapWindowPoints(window, NULL, (POINT *)&r, 2);
        ClipCursor(&r); SetCapture(window); SetCursor(NULL);
    } else { ClipCursor(NULL); ReleaseCapture(); SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW)); }
}
void native_win32_open(HWND window) {
    if (native_mouse_enabled()) {
        RAWINPUTDEVICE device = {.usUsagePage=1,.usUsage=2,.dwFlags=0,.hwndTarget=window};
        RegisterRawInputDevices(&device, 1, sizeof(device));
    }
}
bool native_win32_message(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KILLFOCUS || msg == WM_CLOSE || (msg == WM_ACTIVATEAPP && !wp)) {
        capture(window, false); native_input_release(); return false;
    }
    if (msg == WM_CAPTURECHANGED && g_captured && (HWND)lp != window) { capture(window, false); return false; }
    if ((msg == WM_MOVE || msg == WM_SIZE) && g_captured) {
        RECT r; GetClientRect(window, &r); MapWindowPoints(window, NULL, (POINT *)&r, 2); ClipCursor(&r);
    }
    if (msg == WM_SETCURSOR && g_captured && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return true; }
    if (msg == WM_LBUTTONDOWN && native_mouse_enabled() && !g_captured && GetForegroundWindow() == window) {
        capture(window, true); return true; /* first click only captures */
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (wp == VK_F8 && native_mouse_enabled()) {
            if (down && !(lp & (1u << 30))) capture(window, !g_captured);
            return true;
        }
        if (wp == VK_F10 || wp == VK_F11) return false; /* emulator shortcuts */
        if (native_keyboard_enabled() && GetForegroundWindow() == window) {
            BYTE state[256]; WCHAR chars[4]; GetKeyboardState(state);
            native_key key = {.code=key_hid(lp), .status=1, .type=4,
                .event=down ? ((lp & (1u << 30)) ? 259u : 257u) : 258u, .time_us=pal_time_ns()/1000};
            static const int modifiers[8] = {VK_LCONTROL,VK_LSHIFT,VK_LMENU,VK_LWIN,VK_RCONTROL,VK_RSHIFT,VK_RMENU,VK_RWIN};
            for (unsigned i=0;i<8;i++) if (state[modifiers[i]] & 128) key.status |= 0x100u << i;
            if (state[VK_NUMLOCK]&1) key.status |= 0x10000;
            if (state[VK_CAPITAL]&1) key.status |= 0x20000;
            if (state[VK_SCROLL]&1) key.status |= 0x40000;
            unsigned scan = ((uint64_t)lp >> 16) & 255;
            if (down && ToUnicodeEx((UINT)wp,scan,state,chars,4,4,GetKeyboardLayout(0)) == 1) {
                key.character=(uint16_t)chars[0]; key.status |= 2;
            }
            native_input_key(&key);
        }
    }
    if (msg == WM_INPUT && native_mouse_enabled() && g_captured && GetForegroundWindow() == window) {
        RAWINPUT input; UINT size=sizeof(input);
        if (GetRawInputData((HRAWINPUT)lp,RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER)) == (UINT)-1 || input.header.dwType != RIM_TYPEMOUSE) return false;
        RAWMOUSE *m=&input.data.mouse;
        const USHORT downs[5]={RI_MOUSE_LEFT_BUTTON_DOWN,RI_MOUSE_RIGHT_BUTTON_DOWN,RI_MOUSE_MIDDLE_BUTTON_DOWN,RI_MOUSE_BUTTON_4_DOWN,RI_MOUSE_BUTTON_5_DOWN};
        for (unsigned i=0;i<5;i++) {
            if (m->usButtonFlags & downs[i]) g_buttons |= 1u << i;
            if (m->usButtonFlags & (downs[i]<<1)) g_buttons &= ~(1u << i);
        }
        native_mouse mouse={.buttons=g_buttons,.time_us=pal_time_ns()/1000};
        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) { mouse.x=m->lLastX; mouse.y=m->lLastY; }
        if (m->usButtonFlags & RI_MOUSE_WHEEL) mouse.wheel=(SHORT)m->usButtonData;
        if (m->usButtonFlags & RI_MOUSE_HWHEEL) mouse.tilt=(SHORT)m->usButtonData;
        native_input_mouse(&mouse);
    }
    return false; /* DefWindowProc must clean up foreground WM_INPUT */
}
