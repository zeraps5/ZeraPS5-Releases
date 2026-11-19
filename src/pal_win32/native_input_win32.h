/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_NATIVE_INPUT_WIN32_H
#define PS5C_NATIVE_INPUT_WIN32_H
#include <stdbool.h>
#include <windows.h>
void native_win32_open(HWND window);
/* Called on the window's UI thread. True means handled (result=0). */
bool native_win32_message(HWND window, UINT msg, WPARAM wp, LPARAM lp);
#endif
