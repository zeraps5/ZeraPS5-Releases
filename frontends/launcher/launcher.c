/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ZeraPS5 launcher (Windows): lists the games of the user's game folders,
 * runs them through ps5c.exe, edits the user settings (user\config\zeraps5.ini
 * and per-game user\config\<id>.ini, which ps5c reads itself), keeps each
 * run's log (user\logs\<id>.log), packs a problem report
 * (user\reports\*.zip) and installs new builds. Everything lives in the
 * launcher's folder: the package is portable. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>

#include "base/ini.h"
#include "base/log_record.h"
#include "core/user_config.h"
#include "core/compat_catalog.h"
#include "core/game_patch.h"
#include "pal/pal.h"
#include "pal/module_paths.h"
#include "pal/pal_exfat.h"
#include "pal/pal_pkg.h"
#include "pal_win32/dualsense.h"
#include "pal_win32/report_session.h"
#include "pal_win32/report_archive.h"
#include "pal_win32/keynames.h"
#include "ps5c/build_info.h"
#include "ps5c/version.h"
#include "report_privacy.h"
#include "report_quality.h"
#include "localization.h"

static unsigned g_ui_locale;
static const wchar_t *ui_string(const wchar_t *text) { return ui_translate(g_ui_locale,text); }
static DWORD ui_layout_style(void) { return ui_languages[g_ui_locale].rtl ? WS_EX_LAYOUTRTL|WS_EX_RTLREADING : 0; }
static const wchar_t *ui_key(const wchar_t *text) {
    if(!text)return NULL;
    for(unsigned n=0;n<sizeof(ui_messages)/sizeof(ui_messages[0]);n++)
        if(!wcscmp(text,ui_messages[n][0]) || !wcscmp(text,ui_messages[n][g_ui_locale]))return ui_messages[n][0];
    return NULL;
}
static int ui_message_box(HWND owner,const wchar_t *text,const wchar_t *caption,UINT flags) {
    TASKDIALOG_BUTTON buttons[2];
    unsigned count=1;
    buttons[0]=(TASKDIALOG_BUTTON){IDOK,ui_string(L"OK")};
    if((flags&MB_TYPEMASK)==MB_YESNO) {
        buttons[0]=(TASKDIALOG_BUTTON){IDYES,ui_string(L"Sim")};
        buttons[1]=(TASKDIALOG_BUTTON){IDNO,ui_string(L"Não")};count=2;
    }
    TASKDIALOGCONFIG config={0};config.cbSize=sizeof(config);config.hwndParent=owner;
    config.dwFlags=TDF_SIZE_TO_CONTENT|TDF_POSITION_RELATIVE_TO_WINDOW|TDF_ALLOW_DIALOG_CANCELLATION;
    if(ui_languages[g_ui_locale].rtl)config.dwFlags|=TDF_RTL_LAYOUT;
    config.pszWindowTitle=caption;config.pszContent=ui_string(text);
    config.cButtons=count;config.pButtons=buttons;
    config.nDefaultButton=count>1 && (flags&MB_DEFBUTTON2)?buttons[1].nButtonID:buttons[0].nButtonID;
    if((flags&MB_ICONMASK)==MB_ICONERROR)config.pszMainIcon=TD_ERROR_ICON;
    else if((flags&MB_ICONMASK)==MB_ICONWARNING)config.pszMainIcon=TD_WARNING_ICON;
    else if((flags&MB_ICONMASK)==MB_ICONINFORMATION)config.pszMainIcon=TD_INFORMATION_ICON;
    typedef HRESULT (WINAPI *task_dialog_fn)(const TASKDIALOGCONFIG *,int *,int *,BOOL *);
    HMODULE controls=GetModuleHandleW(L"comctl32.dll");
    task_dialog_fn show_dialog=NULL;
    FARPROC address=controls?GetProcAddress(controls,"TaskDialogIndirect"):NULL;
    memcpy(&show_dialog,&address,sizeof(show_dialog));
    int choice=IDCANCEL;
    if(show_dialog && SUCCEEDED(show_dialog(&config,&choice,NULL,NULL)))return choice;
    /* Recovery on an OS without Common Controls 6 still shows translated text. */
    if(ui_languages[g_ui_locale].rtl)flags|=MB_RTLREADING|MB_RIGHT;
    return MessageBoxW(owner,ui_string(text),caption,flags);
}
#define MessageBoxW ui_message_box

#define APP_NAME L"ZeraPS5"
#define MAX_GAMES 512
#define ICON_SIZE 48

/* ---- GDI+ flat API (only what the game icons need) ---- */

typedef struct gdip_input {
    UINT32 version;
    void *debug_callback;
    BOOL no_background_thread, no_external_codecs;
} gdip_input;
int WINAPI GdiplusStartup(ULONG_PTR *token, const gdip_input *input, void *output);
int WINAPI GdipCreateBitmapFromFile(const WCHAR *file, void **bitmap);
int WINAPI GdipGetImageThumbnail(void *image, UINT w, UINT h, void **thumb, void *abort_cb, void *data);
int WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap, HBITMAP *out, DWORD background);
int WINAPI GdipDisposeImage(void *image);

/* ---- State ---- */

typedef struct game {
    wchar_t dir[MAX_PATH];  /* the folder holding eboot.bin */
    wchar_t title[256];
    wchar_t id[32];         /* title ID, or the folder name without one */
    wchar_t version[32];
    bool is_exfat;
    bool is_pkg;            /* dir is a .pkg file: listed from its metadata, not runnable yet */
    bool pkg_retail;
    bool pro;               /* param.json attribute3 bit 0x400000: PS5 Pro Enhanced */
    bool ps4;               /* a PS4 game: sce_sys/param.sfo, or a CUSA folder */
    int image;
} game;

static game g_games[MAX_GAMES];
static int g_count;
static wchar_t g_base[MAX_PATH]; /* the launcher's folder */
static HWND g_main, g_list, g_status, g_banner, g_buttons[6], g_update_button;
static HWND g_keyboard_button, g_keyboard_hint;
static HFONT g_font;
static int g_dpi = 96;
static PROCESS_INFORMATION g_proc; /* the game running, if any */
static int g_proc_game = -1;
static report_session_store g_proc_session;
static log_session_meta g_proc_meta;
static wchar_t g_play_id[32];
static uint64_t g_play_started, g_play_saved, g_play_total;
static FILETIME g_play_started_wall;
static wchar_t g_session_unavailable[MAX_GAMES][32];
static bool g_session_failure_overflow;
static void poll_game(void);
static bool report_collection_idle(void);

enum { PLAYTIME_COLUMN = 5, PATH_COLUMN };
static const wchar_t *const k_game_columns[] = {
    L"Jogo", L"Tipo", L"ID", L"Versão", L"Console", L"Tempo jogado", L"Caminho"
};
static const int k_game_widths[] = {330, 75, 100, 85, 90, 130, 320};

enum { ID_LIST = 100, ID_PLAY, ID_CONFIG, ID_LOG, ID_REPORT, ID_REFRESH, ID_SETTINGS, ID_TIMER, ID_CHECK_UPDATES,
       ID_MAIN_KEYS, ID_MODS, ID_SHORTCUT, ID_PATCHES };
static void on_command(int id);
static wchar_t g_autoplay[MAX_PATH]; /* --jogar <game folder>: a desktop shortcut starts that game */
static const wchar_t *const k_button_text[6] = {L"Jogar",         L"Configurar jogo", L"Ver log", L"Reportar problema",
                                                L"↻ Recarregar", L"Configurações"};

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void utf8_to_wide(const char *s, wchar_t *out, int cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = 0;
}

static void wide_to_utf8(const wchar_t *s, char *out, int cap);
static bool file_exists(const wchar_t *path) {
    char virtual_path[4 * MAX_PATH];
    wide_to_utf8(path, virtual_path, sizeof(virtual_path));
    if (exfat_path(virtual_path)) {
        pal_path_info info;
        return pal_path_stat(virtual_path, &info) == PS5C_OK && !info.is_dir;
    }
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- param.json: a small JSON walker reporting every scalar with its path ---- */

typedef struct jp {
    const char *p, *end;
    int depth;
    void (*visit)(void *user, const char *path, const char *value);
    void *user;
} jp;

static void jp_ws(jp *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

/* A string (escapes decoded to UTF-8) into out; false on malformed input */
static bool jp_string(jp *j, char *out, size_t cap) {
    size_t n = 0;
    if (j->p >= j->end || *j->p != '"') return false;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        uint32_t c = (uint8_t)*j->p++;
        if (c == '\\' && j->p < j->end) {
            c = (uint8_t)*j->p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r' || c == 'b' || c == 'f') c = ' ';
            else if (c == 'u' && j->end - j->p >= 4) {
                char hex[5] = {j->p[0], j->p[1], j->p[2], j->p[3], 0};
                c = (uint32_t)strtoul(hex, NULL, 16);
                j->p += 4;
                if (c >= 0xD800 && c < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    char lo[5] = {j->p[2], j->p[3], j->p[4], j->p[5], 0};
                    c = 0x10000 + ((c - 0xD800) << 10) + ((uint32_t)strtoul(lo, NULL, 16) - 0xDC00);
                    j->p += 6;
                }
                /* UTF-8 encode */
                char b[4];
                int k = 0;
                if (c < 0x80) b[k++] = (char)c;
                else if (c < 0x800) b[k++] = (char)(0xC0 | c >> 6), b[k++] = (char)(0x80 | (c & 0x3F));
                else if (c < 0x10000)
                    b[k++] = (char)(0xE0 | c >> 12), b[k++] = (char)(0x80 | ((c >> 6) & 0x3F)), b[k++] = (char)(0x80 | (c & 0x3F));
                else
                    b[k++] = (char)(0xF0 | c >> 18), b[k++] = (char)(0x80 | ((c >> 12) & 0x3F)),
                    b[k++] = (char)(0x80 | ((c >> 6) & 0x3F)), b[k++] = (char)(0x80 | (c & 0x3F));
                for (int i = 0; i < k; i++)
                    if (n + 1 < cap) out[n++] = b[i];
                continue;
            }
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (j->p >= j->end) return false;
    j->p++;
    out[n] = 0;
    return true;
}

static bool jp_value(jp *j, char *path, size_t plen) {
    char buf[512];
    jp_ws(j);
    if (j->p >= j->end || ++j->depth > 32) return false;
    bool ok = true;
    if (*j->p == '{' || *j->p == '[') {
        bool object = *j->p == '{';
        char close = object ? '}' : ']';
        j->p++;
        jp_ws(j);
        if (j->p < j->end && *j->p == close) {
            j->p++;
        } else {
            for (;;) {
                size_t len = strlen(path);
                if (object) {
                    jp_ws(j);
                    if (!jp_string(j, buf, sizeof(buf))) return false;
                    jp_ws(j);
                    if (j->p >= j->end || *j->p++ != ':') return false;
                    snprintf(path + len, plen - len, "%s%s", len ? "." : "", buf);
                } else {
                    snprintf(path + len, plen - len, "[]");
                }
                ok = jp_value(j, path, plen);
                path[len] = 0;
                if (!ok) return false;
                jp_ws(j);
                if (j->p < j->end && *j->p == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->end && *j->p == close) {
                    j->p++;
                    break;
                }
                return false;
            }
        }
    } else if (*j->p == '"') {
        ok = jp_string(j, buf, sizeof(buf));
        if (ok) j->visit(j->user, path, buf);
    } else {
        size_t n = 0;
        while (j->p < j->end && !strchr(",}] \t\r\n", *j->p)) {
            if (n + 1 < sizeof(buf)) buf[n++] = *j->p;
            j->p++;
        }
        buf[n] = 0;
        j->visit(j->user, path, buf);
    }
    j->depth--;
    return ok;
}

typedef struct param_info {
    char id[32], version[32], default_lang[16];
    uint64_t attribute3; /* features: 0x40 120 Hz, 0x400 PS VR2, 0x40000 VRR, 0x400000 PS5 Pro, 0x800000 8K on Pro */
    char titles[40][2][160]; /* (language, title) */
    int title_count;
} param_info;

static void param_visit(void *user, const char *path, const char *value) {
    param_info *p = (param_info *)user;
    if (!strcmp(path, "titleId")) snprintf(p->id, sizeof(p->id), "%s", value);
    else if (!strcmp(path, "contentVersion")) snprintf(p->version, sizeof(p->version), "%s", value);
    else if (!strcmp(path, "attribute3")) p->attribute3 = strtoull(value, NULL, 10);
    else if (!strcmp(path, "localizedParameters.defaultLanguage")) snprintf(p->default_lang, sizeof(p->default_lang), "%s", value);
    else if (!strncmp(path, "localizedParameters.", 20) && p->title_count < 40) {
        const char *lang = path + 20, *dot = strchr(lang, '.');
        if (dot && !strcmp(dot, ".titleName") && dot - lang < 16) {
            snprintf(p->titles[p->title_count][0], 16, "%.*s", (int)(dot - lang), lang);
            snprintf(p->titles[p->title_count][1], 160, "%s", value);
            p->title_count++;
        }
    }
}

static const char *param_title(const param_info *p, const char *lang) {
    for (int i = 0; i < p->title_count; i++)
        if (!strcmp(p->titles[i][0], lang)) return p->titles[i][1];
    return NULL;
}

static bool parse_param(const char *data, uint64_t size, param_info *out) {
    memset(out, 0, sizeof(*out));
    jp j = {data, data + size, 0, param_visit, out};
    if (size >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) j.p += 3;
    char path_buf[256] = "";
    return jp_value(&j, path_buf, sizeof(path_buf));
}

static bool read_param(const wchar_t *path, param_info *out) {
    memset(out, 0, sizeof(*out));
    char utf8[4 * MAX_PATH];
    wide_to_utf8(path, utf8, sizeof(utf8));
    pal_file *f = NULL;
    uint64_t size = 0, got = 0;
    if (pal_file_open(utf8, PAL_FILE_READ, &f) != PS5C_OK) return false;
    bool ok = pal_file_size(f, &size) == PS5C_OK && size && size < (8u << 20);
    char *data = ok ? (char *)malloc((size_t)size) : NULL;
    ok = data && pal_file_read(f, data, size, &got) == PS5C_OK && got == size;
    pal_file_close(f);
    if (ok) ok = parse_param(data, size, out);
    free(data);
    return ok;
}

static void fill_list(void);
static void check_updates(const char *source, bool manual);
static const char *update_source(void);
static const char *report_source(void);

/* ---- User settings: user\config\zeraps5.ini and <id>.ini (see core/user_config.h) ---- */

static ini *g_global;
static bool g_system_cached;
static char g_system_setting[MODULE_PATH_CAP];
static module_path_result g_system_result;

static void wide_to_utf8(const wchar_t *s, char *out, int cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL)) out[0] = 0;
}

static ini *ini_read_file(const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    char *data = size < (4u << 20) ? (char *)malloc(size + 1) : NULL;
    ini *doc = NULL;
    if (data && ReadFile(f, data, size, &got, NULL) && got == size) doc = ini_parse(data, size);
    free(data);
    CloseHandle(f);
    return doc;
}

static bool ini_write_file(const ini *doc, const wchar_t *path) {
    CreateDirectoryW(L"user", NULL);
    CreateDirectoryW(L"user\\config", NULL);
    size_t len;
    char *text = ini_text(doc, &len);
    if (!text) return false;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, (DWORD)len, &put, NULL) && put == len;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(text);
    return ok;
}

static void global_path(wchar_t *out) { swprintf(out, MAX_PATH, L"%ls\\user\\config\\zeraps5.ini", g_base); }

/* The global file, created with the defaults only when it does not exist:
 * the user's own file is never replaced. */
static void load_global(void) {
    wchar_t path[MAX_PATH];
    global_path(path);
    ini_free(g_global);
    g_global = ini_read_file(path);
    if (!g_global) {
        g_global = ini_parse(user_config_default_text, strlen(user_config_default_text));
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) ini_write_file(g_global, path);
    }
    g_ui_locale=ui_language_index(ini_get(g_global,"interface","idioma"));
    g_system_cached=false;
}

static void save_global(void) {
    wchar_t path[MAX_PATH];
    global_path(path);
    if (!ini_write_file(g_global, path))
        MessageBoxW(g_main, ui_string(L"Não foi possível salvar user\\config\\zeraps5.ini."), APP_NAME, MB_ICONERROR);
}

/* A folder setting as a full path: relative ones are inside the launcher's folder */
static void full_path(const char *value, wchar_t *out) {
    wchar_t w[MAX_PATH];
    utf8_to_wide(value, w, MAX_PATH);
    for (wchar_t *c = w; *c; c++)
        if (*c == L'/') *c = L'\\';
    if ((w[0] && w[1] == L':') || (w[0] == L'\\' && w[1] == L'\\')) swprintf(out, MAX_PATH, L"%ls", w);
    else swprintf(out, MAX_PATH, L"%ls\\%ls", g_base, w);
}

/* A folder inside the launcher's folder is stored relative to it (the package can move) */
static void relative_path(const wchar_t *path, char *out, int cap) {
    size_t n = wcslen(g_base);
    const wchar_t *p = path;
    if (!_wcsnicmp(path, g_base, n) && path[n] == L'\\') p = path + n + 1;
    wide_to_utf8(p, out, cap);
}

/* [pastas] jogos1, jogos2, ...: the game folders, in file order */
static int game_folders(const char **out, int max) {
    const char *keys[64];
    uint32_t n = ini_keys(g_global, "pastas", keys, 64);
    int count = 0;
    for (uint32_t i = 0; i < n && i < 64 && count < max; i++) {
        if (_strnicmp(keys[i], "jogos", 5)) continue;
        const char *v = ini_get(g_global, "pastas", keys[i]);
        if (v && *v) out[count++] = v;
    }
    return count;
}

static void system_folder(wchar_t *out) {
    const char *v = ini_get(g_global, "pastas", "sistema");
    if(!v || !*v)v="system/modules";
    if(!g_system_cached || strcmp(v,g_system_setting)) {
        char base[4*MAX_PATH];wide_to_utf8(g_base,base,sizeof(base));
        snprintf(g_system_setting,sizeof(g_system_setting),"%s",v);
        module_path_resolve(base,v,true,&g_system_result);g_system_cached=true;
    }
    if(g_system_result.origin!=MODULE_PATH_NONE)utf8_to_wide(g_system_result.directory,out,MAX_PATH);
    else full_path(v,out);
}

static void game_config_path(const game *g, wchar_t *out) {
    swprintf(out, MAX_PATH, L"%ls\\user\\config\\%ls.ini", g_base, g->id);
}

typedef struct choice {
    int value;
    const wchar_t *name;
} choice;

/* SCE system language codes */
static const choice k_languages[] = {
    {17, L"Português (Brasil)"}, {1, L"English (US)"}, {18, L"English (UK)"}, {3, L"Español"},
    {2, L"Français"},            {4, L"Deutsch"},      {5, L"Italiano"},      {7, L"Português (Portugal)"},
    {0, L"日本語"},
};
#define LANGUAGE_COUNT ((int)(sizeof(k_languages) / sizeof(k_languages[0])))
static const choice k_resolutions[] = {{1080, L"1080p"}, {2160, L"4K (2160p)"}};
#define RESOLUTION_COUNT 2
/* [geral] cpu_antiga: replacing the instructions processors before Intel Haswell / AMD Ryzen lack */
static const choice k_cpu_modes[] = {
    {0, L"Automático (só se o processador não tiver AVX2)"}, {1, L"Sempre ligado (teste)"}, {2, L"Desligado"}};
#define CPU_MODE_COUNT 3
/* the console the game is told it runs on (PS5 Pro: only games made for it; experimental) */
static const choice k_consoles[] = {{0, L"PS5"}, {1, L"PS5 Pro (experimental)"}};
#define CONSOLE_COUNT 2
/* Unreal games: the scene's share of their 4K output ([geral] escala_unreal, core/ue_scale.h) */
static const choice k_ue_scales[] = {{0, L"Do jogo"}, {75, L"75%"}, {67, L"67% (~1440p)"}, {50, L"50% (~1080p)"}, {33, L"33% (~720p)"}};
#define UE_SCALE_COUNT 5
/* the size games render at where it can be set ([geral] resolucao_interna: game fixes in
 * core/game_fixes.c, Unreal games through core/ue_scale.c unless escala_unreal is chosen) */
static const choice k_internal_res[] = {{0, L"Do jogo"}, {1440, L"1440p"}, {1080, L"1080p"},
                                        {900, L"900p"},   {720, L"720p"},   {540, L"540p"}};
#define INTERNAL_RES_COUNT 6

/* a [geral] resolucao_interna value the list has, else 0 (the game's own size) */
static int internal_res_value(int64_t v) {
    for (int i = 0; i < INTERNAL_RES_COUNT; i++)
        if (k_internal_res[i].value == v) return (int)v;
    return 0;
}
/* [video] tela: how the game window shows (pal_window_set_mode) */
/* [geral] nitidez_fsr: the RCAS strength of the FSR 1 upscale (vk_renderer.c fsr_sharpness) */
static const choice k_fsr_sharpness[] = {{0, L"Suave"}, {1, L"Normal"}, {2, L"Forte"}};
#define FSR_SHARPNESS_COUNT 3
static const choice k_screen_modes[] = {{0, L"Janela"}, {1, L"Tela cheia (Esc sai)"}, {2, L"Janela maximizada"}};
#define SCREEN_MODE_COUNT 3

static const wchar_t *choice_name(const choice *c, int n, int value) {
    for (int i = 0; i < n; i++)
        if (c[i].value == value) return c[i].name;
    return L"?";
}

/* ---- Dialog helpers ---- */

static HWND add_control(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                        int id) {
    DWORD ex = !wcscmp(cls, L"EDIT") || !wcscmp(cls, L"LISTBOX") ? WS_EX_CLIENTEDGE : 0;
    HWND c = CreateWindowExW(ex|ui_layout_style(), cls, ui_string(text), WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, NULL, NULL);
    const wchar_t *key=ui_key(text);
    if(key)SetPropW(c,L"ZeraPS5.I18N",(HANDLE)key);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HWND add_button(HWND parent, const wchar_t *text, int x, int y, int w, int id) {
    return add_control(parent, L"BUTTON", text, WS_TABSTOP | BS_MULTILINE | (id == IDOK ? BS_DEFPUSHBUTTON : 0), x, y, w, 32, id);
}

/* A combo of choices; with def_name, a first entry "Padrão (def_name)" stands for "not set" (-1) */
static HWND add_combo(HWND parent, int x, int y, int w, const choice *c, int n, int current, const wchar_t *def_name) {
    HWND cb = add_control(parent, WC_COMBOBOXW, NULL, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 260, 0);
    int sel = 0;
    if (def_name) {
        wchar_t text[128];
        swprintf(text, 128, ui_string(L"Padrão (%ls)"), def_name);
        LRESULT item = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)text);
        if (item >= 0) SendMessageW(cb, CB_SETITEMDATA, (WPARAM)item, -1);
    }
    for (int i = 0; i < n; i++) {
        LRESULT item = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)ui_string(c[i].name));
        if (item < 0) continue;
        SendMessageW(cb, CB_SETITEMDATA, (WPARAM)item, c[i].value);
        if (c[i].value == current) sel = (int)item;
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)sel, 0);
    return cb;
}

static int combo_value(HWND cb) {
    LRESULT sel = SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return sel == CB_ERR ? -1 : (int)SendMessageW(cb, CB_GETITEMDATA, (WPARAM)sel, 0);
}

/* Keep a percentage set by hand in the INI available alongside the presets. */
static HWND add_ue_scale_combo(HWND parent, int x, int y, int w, int64_t current, const wchar_t *def_name) {
    if (current < -1 || current >= 100) current = 0;
    for (int i = 0; i < UE_SCALE_COUNT; i++)
        if (k_ue_scales[i].value == current)
            return add_combo(parent, x, y, w, k_ue_scales, UE_SCALE_COUNT, (int)current, def_name);
    if (current < 0) return add_combo(parent, x, y, w, k_ue_scales, UE_SCALE_COUNT, -1, def_name);

    choice scales[UE_SCALE_COUNT + 1];
    wchar_t percent[16];
    memcpy(scales, k_ue_scales, sizeof(k_ue_scales));
    swprintf(percent, 16, L"%d%%", (int)current);
    scales[UE_SCALE_COUNT] = (choice){(int)current, percent};
    return add_combo(parent, x, y, w, scales, UE_SCALE_COUNT + 1, (int)current, def_name);
}

static HWND create_dialog(const wchar_t *cls, const wchar_t *title, int w, int h, HWND owner) {
    RECT r = {0, 0, S(w), S(h)};
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    RECT o;
    GetWindowRect(owner, &o);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|ui_layout_style(), cls, ui_string(title), WS_CAPTION | WS_SYSMENU, o.left + ((o.right - o.left) - ww) / 2,
                           o.top + ((o.bottom - o.top) - wh) / 2, ww, wh, owner, NULL, NULL, NULL);
    const wchar_t *key=ui_key(title);
    if(key)SetPropW(dialog,L"ZeraPS5.I18N",(HANDLE)key);
    return dialog;
}

/* Shows a dialog window and runs it until it is destroyed; its owner waits */
static void run_modal(HWND w, HWND owner, bool dialog_keys) {
    EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    HWND first=GetNextDlgTabItem(w,NULL,FALSE);
    if(first)SetFocus(first);
    MSG msg;
    while (IsWindow(w) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!dialog_keys || !IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

enum { ID_UI_LANGUAGES = 900, ID_UI_LANG_FIRST = 920 };
static HWND g_ui_choice;
static void ui_apply_main(void);
static BOOL CALLBACK ui_apply_control(HWND window,LPARAM unused) {
    (void)unused;
    LONG_PTR style=GetWindowLongPtrW(window,GWL_EXSTYLE);
    style &= ~(LONG_PTR)(WS_EX_LAYOUTRTL|WS_EX_RTLREADING);
    if(!GetPropW(window,L"ZeraPS5.LTR"))style|=ui_layout_style();
    SetWindowLongPtrW(window,GWL_EXSTYLE,style);
    const wchar_t *key=(const wchar_t *)GetPropW(window,L"ZeraPS5.I18N");
    if(key)SetWindowTextW(window,ui_string(key));
    SetWindowPos(window,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    InvalidateRect(window,NULL,TRUE);
    return TRUE;
}

/* A launcher preference only: never writes geral/idioma or per-game settings. */
static bool ui_save_preference(unsigned selected) {
    if(selected>=UI_LANGUAGE_COUNT || !g_global)return false;
    const char *before=ini_get(g_global,"interface","idioma");char old[32];
    bool existed=before!=NULL;
    snprintf(old,sizeof(old),"%s",before?before:"");
    ini_set(g_global,"interface","idioma",ui_languages[selected].tag);
    wchar_t path[MAX_PATH];global_path(path);
    if(!ini_write_file(g_global,path)) {
        ini_set(g_global,"interface","idioma",existed?old:NULL);
        return false;
    }
    g_ui_locale=selected;
    return true;
}

static LRESULT CALLBACK languages_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_COMMAND && LOWORD(wp)==IDOK) {
        LRESULT selected=SendMessageW(g_ui_choice,CB_GETCURSEL,0,0);
        if(selected==CB_ERR || !ui_save_preference((unsigned)selected)) {
            MessageBoxW(window,L"Falha ao salvar configurações.",APP_NAME,MB_ICONERROR);return 0;
        }
        HWND owner=GetWindow(window,GW_OWNER);
        DestroyWindow(window);
        ui_apply_main();
        if(owner && owner!=g_main) {ui_apply_control(owner,0);EnumChildWindows(owner,ui_apply_control,0);}
        return 0;
    }
    if(message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wp)==IDCANCEL)) {DestroyWindow(window);return 0;}
    return DefWindowProcW(window,message,wp,lp);
}

static void configure_languages(HWND owner) {
    HWND dialog=create_dialog(L"ZeraPS5Languages",L"Idiomas",560,245,owner);
    add_control(dialog,L"STATIC",L"Idioma da interface:",0,16,16,528,22,0);
    g_ui_choice=add_control(dialog,WC_COMBOBOXW,NULL,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,16,45,528,350,0);
    for(unsigned n=0;n<UI_LANGUAGE_COUNT;n++)SendMessageW(g_ui_choice,CB_ADDSTRING,0,(LPARAM)ui_languages[n].name);
    SendMessageW(g_ui_choice,CB_SETCURSEL,g_ui_locale,0);
    add_control(dialog,L"STATIC",L"Alterar o idioma do launcher não altera o idioma do jogo.",0,16,88,528,42,0);
    add_control(dialog,L"STATIC",L"A interface muda ao salvar. O jogo usa as suas próprias opções de idioma.",0,16,134,528,48,0);
    add_button(dialog,L"Salvar",312,198,110,IDOK);
    add_button(dialog,L"Cancelar",434,198,110,IDCANCEL);
    run_modal(dialog,owner,true);
}

static BOOL CALLBACK ui_folder_child(HWND child,LPARAM unused) {
    (void)unused;
    wchar_t cls[32];GetClassNameW(child,cls,32);
    if(!_wcsicmp(cls,L"BUTTON")) {
        int id=GetDlgCtrlID(child);
        SetWindowTextW(child,ui_string(id==IDOK?L"OK":id==IDCANCEL?L"Cancelar":L"Criar nova pasta"));
    } else if(!_wcsicmp(cls,L"STATIC")) {
        /* The instruction is supplied by BROWSEINFO in the selected language.
         * Other nonempty static labels belong to the path entry. */
        wchar_t label[512];GetWindowTextW(child,label,512);
        if(label[0] && !ui_key(label))SetWindowTextW(child,ui_string(L"Pasta"));
    }
    return TRUE;
}
static int CALLBACK ui_folder_ready(HWND dialog,UINT message,LPARAM value,LPARAM title) {
    (void)value;
    if(message==BFFM_INITIALIZED) {
        SetWindowTextW(dialog,(const wchar_t *)title);
        SetWindowLongPtrW(dialog,GWL_EXSTYLE,GetWindowLongPtrW(dialog,GWL_EXSTYLE)|ui_layout_style());
        EnumChildWindows(dialog,ui_folder_child,0);
    }
    return 0;
}
static bool browse_folder(HWND owner, const wchar_t *title, wchar_t *out) {
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn=ui_folder_ready;bi.lParam=(LPARAM)title;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return false;
    bool ok = SHGetPathFromIDListW(id, out) != FALSE;
    CoTaskMemFree(id);
    return ok;
}

/* ---- Per-game settings ---- */

/* Resolve the same update/decrypted overlay as the core. A base game's old
 * version must never make an unknown update appear compatible in this UI. */
static bool patch_content(const game *g, char *file, size_t cap, param_info *param) {
    char base[4 * MAX_PATH], update[4 * MAX_PATH], original[4 * MAX_PATH];
    wide_to_utf8(g->dir, base, sizeof(base));
    snprintf(original, sizeof(original), "%s", base);
    bool over = game_patch_find(base, update, sizeof(update));
    if (!over && game_patch_base_of(base, original, sizeof(original))) {
        snprintf(update, sizeof(update), "%s", base);
        snprintf(base, sizeof(base), "%s", original);
        over = true;
    }
    snprintf(file, cap, "%s/eboot.bin", over ? update : base);
    pal_path_info st;
    if (over && (pal_path_stat(file, &st) != PS5C_OK || st.is_dir))
        snprintf(file, cap, "%s/eboot.bin", base);
    char path[4 * MAX_PATH + 32];
    wchar_t wide[4 * MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", over ? update : base);
    utf8_to_wide(path, wide, PS5C_ARRAY_COUNT(wide));
    /* An installed update with invalid metadata must not inherit the base
     * version. The CLI also selects this param.json whenever it exists. */
    if (pal_path_stat(path, &st) == PS5C_OK) return read_param(wide, param);
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", base);
    utf8_to_wide(path, wide, PS5C_ARRAY_COUNT(wide));
    return read_param(wide, param);
}

enum { MAX_COMPAT_ROWS = 64 };
static HWND g_patch_list;
static int g_patch_game;
static const compat_patch *g_patch_rows[MAX_COMPAT_ROWS];
static bool g_patch_ready[MAX_COMPAT_ROWS];
static size_t g_patch_count;

static LRESULT CALLBACK patches_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == g_patch_list &&
        ((NMHDR *)lp)->code == LVN_ITEMCHANGING) {
        NMLISTVIEW *change = (NMLISTVIEW *)lp;
        if (change->iItem >= 0 && (size_t)change->iItem < g_patch_count &&
            !g_patch_ready[change->iItem] && (change->uChanged & LVIF_STATE) &&
            ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        wchar_t path[MAX_PATH];
        game_config_path(&g_games[g_patch_game], path);
        ini *doc = ini_read_file(path);
        if (!doc) doc = ini_parse("", 0);
        for (size_t i = 0; i < g_patch_count; i++) {
            if (!g_patch_ready[i]) continue; /* retain preference across unknown updates */
            ini_set(doc, "patches", g_patch_rows[i]->id,
                    ListView_GetCheckState(g_patch_list, (int)i) ? "1" : "0");
        }
        bool saved = ini_write_file(doc, path);
        ini_free(doc);
        if (!saved) {
            MessageBoxW(w, L"Não foi possível salvar os patches deste jogo.", APP_NAME, MB_ICONERROR);
            return 0;
        }
        DestroyWindow(w);
        return 0;
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void configure_patches(int gi, HWND owner) {
    const game *g = &g_games[gi];
    char id[128], file[4 * MAX_PATH + 32];
    wide_to_utf8(g->id, id, sizeof(id));
    g_patch_game = gi;
    g_patch_count = 0;
    for (size_t i = 0; i < compat_patch_count() && g_patch_count < MAX_COMPAT_ROWS; i++) {
        const compat_patch *p = compat_patch_at(i);
        if (!strcmp(p->title_id, id)) g_patch_rows[g_patch_count++] = p;
    }
    param_info effective = {0};
    bool have_content = !g->is_pkg && patch_content(g, file, sizeof(file), &effective);
    uint8_t *data = NULL;
    uint64_t size = 0;
    bool need_hash = false;
    for (size_t i = 0; i < g_patch_count; i++)
        need_hash |= have_content && !strcmp(effective.version, g_patch_rows[i]->version);
    if (need_hash && pal_file_read_all(file, &data, &size) != PS5C_OK) data = NULL;
    HWND w = create_dialog(L"ZeraPS5Patches", ui_string(L"Patches de compatibilidade"), 700, 380, owner);
    wchar_t heading[600];
    swprintf(heading, PS5C_ARRAY_COUNT(heading), L"%ls — %hs | %ls %hs", g->title, id,
             ui_string(L"Versão em uso:"), have_content ? effective.version : "?");
    add_control(w, L"STATIC", heading, 0, 16, 14, 668, 40, 0);
    add_control(w, L"STATIC", ui_string(L"Aplicados em memória ao iniciar. Os arquivos do jogo permanecem intactos."),
                0, 16, 58, 668, 36, 0);
    g_patch_list = add_control(w, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                               16, 100, 668, 126, 0);
    ListView_SetExtendedListViewStyle(g_patch_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    const wchar_t *cols[] = {L"Patch", L"Versão suportada", L"Compatibilidade"};
    const int widths[] = {280, 125, 240};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c = {0};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)ui_string(cols[i]);
        c.cx = S(widths[i]);
        ListView_InsertColumn(g_patch_list, i, &c);
    }
    wchar_t config[MAX_PATH];
    game_config_path(g, config);
    ini *doc = ini_read_file(config);
    for (size_t i = 0; i < g_patch_count; i++) {
        const compat_patch *p = g_patch_rows[i];
        compat_patch_status status = compat_patch_check(p, effective.id, effective.version, data, (size_t)size);
        g_patch_ready[i] = status == COMPAT_PATCH_READY;
        wchar_t name[256], version[64];
        utf8_to_wide(p->name, name, PS5C_ARRAY_COUNT(name));
        utf8_to_wide(p->version, version, PS5C_ARRAY_COUNT(version));
        LVITEMW item = {0}; item.mask = LVIF_TEXT; item.iItem = (int)i; item.pszText = (wchar_t *)ui_string(name);
        ListView_InsertItem(g_patch_list, &item);
        ListView_SetItemText(g_patch_list, (int)i, 1, version);
        const wchar_t *label = status == COMPAT_PATCH_READY ? L"Compatível — assinatura conferida" :
                               status == COMPAT_PATCH_WRONG_VERSION ? L"Versão não suportada" :
                               !have_content || !data ? L"Executável não verificado" : L"Executável não reconhecido";
        ListView_SetItemText(g_patch_list, (int)i, 2, (wchar_t *)ui_string(label));
        if (g_patch_ready[i])
            ListView_SetCheckState(g_patch_list, (int)i, ini_get_int(doc, "patches", p->id,
                ini_get_int(g_global, "patches", p->id, p->default_enabled)) == 1);
    }
    ini_free(doc);
    free(data);
    if (g_patch_count) {
        wchar_t description[700];
        utf8_to_wide(g_patch_rows[0]->description, description, PS5C_ARRAY_COUNT(description));
        add_control(w, L"STATIC", ui_string(description), 0, 16, 238, 668, 52, 0);
    } else add_control(w, L"STATIC", ui_string(L"Nenhum patch de compatibilidade cadastrado para este ID."), 0, 16, 238, 668, 52, 0);
    add_control(w, L"STATIC", ui_string(L"Após atualizar o jogo, versões não reconhecidas ficam bloqueadas. Alterações valem na próxima execução."),
                0, 16, 294, 668, 36, 0);
    add_button(w, ui_string(L"Salvar"), 494, 338, 90, IDOK);
    add_button(w, ui_string(L"Cancelar"), 594, 338, 90, IDCANCEL);
    run_modal(w, owner, true);
}

static int g_cfg_game;
static HWND g_cfg_lang, g_cfg_res, g_cfg_console, g_cfg_ue, g_cfg_internal;

static LRESULT CALLBACK game_config_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == ID_PATCHES) {
        configure_patches(g_cfg_game, w);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        const game *g = &g_games[g_cfg_game];
        wchar_t path[MAX_PATH];
        game_config_path(g, path);
        ini *doc = ini_read_file(path);
        if (!doc) {
            char title[512], text[700];
            wide_to_utf8(g->title, title, sizeof(title));
            snprintf(text, sizeof(text),
                     "; ZeraPS5 - configurações só deste jogo: %s\n; o que não estiver aqui vem de zeraps5.ini\n", title);
            doc = ini_parse(text, strlen(text));
        }
        int lang = combo_value(g_cfg_lang), res = combo_value(g_cfg_res);
        char v[16];
        snprintf(v, sizeof(v), "%d", lang);
        ini_set(doc, "geral", "idioma", lang < 0 ? NULL : v);
        snprintf(v, sizeof(v), "%d", res);
        ini_set(doc, "geral", "resolucao", res < 0 ? NULL : v);
        int console = combo_value(g_cfg_console);
        snprintf(v, sizeof(v), "%d", console);
        ini_set(doc, "geral", "console", console < 0 ? NULL : v);
        int ue = combo_value(g_cfg_ue);
        snprintf(v, sizeof(v), "%d", ue);
        ini_set(doc, "geral", "escala_unreal", ue < 0 ? NULL : v);
        int internal = combo_value(g_cfg_internal);
        snprintf(v, sizeof(v), "%d", internal);
        ini_set(doc, "geral", "resolucao_interna", internal < 0 ? NULL : v);
        if (!ini_write_file(doc, path))
            MessageBoxW(w, ui_string(L"Não foi possível salvar a configuração do jogo."), APP_NAME, MB_ICONERROR);
        ini_free(doc);
        DestroyWindow(w);
        return 0;
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void configure_game(int gi) {
    const game *g = &g_games[gi];
    wchar_t title[320], path[MAX_PATH];
    swprintf(title, 320, ui_string(L"Configurar: %ls"), g->title);
    game_config_path(g, path);
    ini *doc = ini_read_file(path);
    int lang = (int)ini_get_int(doc, "geral", "idioma", -1), res = (int)ini_get_int(doc, "geral", "resolucao", -1);
    int console = (int)ini_get_int(doc, "geral", "console", -1);
    int64_t ue = ini_get_int(doc, "geral", "escala_unreal", -1);
    int64_t internal = ini_get_int(doc, "geral", "resolucao_interna", -1);
    ini_free(doc);
    g_cfg_game = gi;
    HWND w = create_dialog(L"ZeraPS5GameConfig", title, 420, 400, g_main);
    add_control(w, L"STATIC", ui_string(L"Só para este jogo. \"Padrão\" segue as Configurações gerais."), 0, 16, 14, 390, 20, 0);
    add_control(w, L"STATIC", ui_string(L"Idioma do console:"), 0, 16, 48, 150, 20, 0);
    int glang = (int)ini_get_int(g_global, "geral", "idioma", USER_LANGUAGE_DEFAULT);
    int gres = (int)ini_get_int(g_global, "geral", "resolucao", USER_RESOLUTION_DEFAULT);
    g_cfg_lang = add_combo(w, 170, 44, 230, k_languages, LANGUAGE_COUNT, lang, choice_name(k_languages, LANGUAGE_COUNT, glang));
    add_control(w, L"STATIC", ui_string(L"Resolução da TV:"), 0, 16, 84, 150, 20, 0);
    g_cfg_res = add_combo(w, 170, 80, 230, k_resolutions, RESOLUTION_COUNT, res,
                          choice_name(k_resolutions, RESOLUTION_COUNT, gres));
    add_control(w, L"STATIC", ui_string(L"Console:"), 0, 16, 120, 150, 20, 0);
    g_cfg_console = add_combo(w, 170, 116, 230, k_consoles, CONSOLE_COUNT, console,
                              choice_name(k_consoles, CONSOLE_COUNT, (int)ini_get_int(g_global, "geral", "console", 0)));
    int64_t global_ue = ini_get_int(g_global, "geral", "escala_unreal", 0);
    wchar_t global_percent[16];
    const wchar_t *default_ue = ui_string(k_ue_scales[0].name);
    if (global_ue > 0 && global_ue < 100) {
        swprintf(global_percent, 16, L"%d%%", (int)global_ue);
        default_ue = global_percent;
    }
    int global_internal = internal_res_value(ini_get_int(g_global, "geral", "resolucao_interna", 0));
    add_control(w, L"STATIC", L"Resolução interna:", 0, 16, 156, 205, 20, 0);
    g_cfg_internal = add_combo(w, 226, 152, 174, k_internal_res, INTERNAL_RES_COUNT,
                               internal == -1 ? -1 : internal_res_value(internal),
                               ui_string(choice_name(k_internal_res, INTERNAL_RES_COUNT, global_internal)));
    add_control(w, L"STATIC", L"Jogos Unreal, Sonic Origins Plus e TMNT: Splintered Fate.", 0, 16, 184, 390, 20, 0);
    add_control(w, L"STATIC", L"Escala Unreal (avançado):", 0, 16, 216, 205, 20, 0);
    g_cfg_ue = add_ue_scale_combo(w, 226, 212, 174, ue, default_ue);
    add_control(w, L"STATIC", L"Só jogos Unreal com escala compatível.", 0, 16, 244, 390, 20, 0);
    add_control(w, L"STATIC", ui_string(g->pro ? L"Este jogo tem suporte ao PS5 Pro." : L"Este jogo não tem suporte ao PS5 Pro."), 0, 16,
                272, 390, 20, 0);
    add_button(w, ui_string(L"Patches de compatibilidade..."), 16, 304, 384, ID_PATCHES);
    add_button(w, ui_string(L"Salvar"), 210, 350, 90, IDOK);
    add_button(w, ui_string(L"Cancelar"), 310, 350, 90, IDCANCEL);
    run_modal(w, g_main, true);
}

/* ---- Keyboard controls ---- */

static char g_keys_edit[64][64]; /* per user_controls entry, being edited */
static HWND g_keys_list;
static HWND g_keys_native, g_mouse_native, g_keys_status;
static int g_captured;

static LRESULT CALLBACK capture_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        char name[32];
        key_name((int)wp, name, sizeof(name));
        if (wp == VK_ESCAPE) g_captured = -1;
        else if (name[0]) g_captured = (int)wp;
        else return 0; /* a key without a name: keep waiting */
        DestroyWindow(w);
        return 0;
    }
    case WM_CLOSE:
        g_captured = -1;
        DestroyWindow(w);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        r.top += S(20);
        SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        wchar_t control[256], line[400];
        GetWindowTextW(w, control, 256);
        swprintf(line, 400, ui_string(L"Pressione a tecla para:\n%ls\n\n(Esc cancela)"), control);
        DrawTextW(dc, line, -1, &r, DT_CENTER | DT_WORDBREAK);
        EndPaint(w, &ps);
        return 0;
    }
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* One key from the user (0 when cancelled) */
static int capture_key(HWND owner, const wchar_t *control) {
    g_captured = 0;
    HWND w = create_dialog(L"ZeraPS5Capture", control, 300, 120, owner);
    run_modal(w, owner, false);
    return g_captured > 0 ? g_captured : 0;
}

static void ui_control_label(uint32_t index,wchar_t *out,size_t cap) {
    wchar_t original[128];
    utf8_to_wide(user_controls[index].label,original,128);
    wchar_t *colon=wcschr(original,L':');
    if(!colon) { swprintf(out,cap,L"%ls",ui_string(original));return; }
    *colon=0;wchar_t *direction=colon+1;while(*direction==L' ')direction++;
    if(*direction>=L'a' && *direction<=L'z')*direction-=L'a'-L'A';
    swprintf(out,cap,ui_string(L"%ls: %ls"),ui_string(original),ui_string(direction));
}

static void keys_refresh(void) {
    ListView_DeleteAllItems(g_keys_list);
    for (uint32_t i = 0; i < user_control_count; i++) {
        wchar_t label[128], keys[128];
        ui_control_label(i,label,128);
        if(g_keys_edit[i][0])utf8_to_wide(g_keys_edit[i],keys,128);
        else swprintf(keys,128,L"%ls",ui_string(L"(nenhuma)"));
        LVITEMW it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = label;
        ListView_InsertItem(g_keys_list, &it);
        ListView_SetItemText(g_keys_list, (int)i, 1, keys);
    }
}

enum { ID_KEY_SET = 300, ID_KEY_ADD, ID_KEY_CLEAR, ID_KEY_DEFAULTS };

static void keys_edit(HWND w, int id) {
    if (id == ID_KEY_DEFAULTS) {
        for (uint32_t i = 0; i < user_control_count; i++) snprintf(g_keys_edit[i], 64, "%s", user_controls[i].keys);
        keys_refresh();
        return;
    }
    int i = ListView_GetNextItem(g_keys_list, -1, LVNI_SELECTED);
    if (i < 0) {
        MessageBoxW(w, ui_string(L"Selecione um controle na lista."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (id == ID_KEY_CLEAR) {
        g_keys_edit[i][0] = 0;
    } else {
        wchar_t label[128];
        ui_control_label(i,label,128);
        int vk = capture_key(w, label);
        if (!vk) return;
        char name[32];
        key_name(vk, name, sizeof(name));
        if (id == ID_KEY_SET || !g_keys_edit[i][0]) snprintf(g_keys_edit[i], 64, "%s", name);
        else if (strlen(g_keys_edit[i]) + strlen(name) + 2 < 64) strcat(strcat(g_keys_edit[i], ","), name);
    }
    keys_refresh();
    ListView_SetItemState(g_keys_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_keys_list, i, FALSE);
}

static LRESULT CALLBACK keys_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == 353) {
            bool pad = pal_input_has_controller();
            bool checked = SendMessageW(g_keys_native, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(g_keys_native, pad || checked);
            SetWindowTextW(g_keys_status, ui_string(pad ? L"Controle conectado: teclado nativo disponível." : L"Conecte um controle para habilitar o teclado nativo."));
        }
        return 0;
    case WM_DESTROY: KillTimer(w, 353); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) >= ID_KEY_SET && LOWORD(wp) <= ID_KEY_DEFAULTS) {
            keys_edit(w, LOWORD(wp));
        } else if (LOWORD(wp) == IDOK) {
            bool keyboard = SendMessageW(g_keys_native, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (keyboard && !pal_input_has_controller()) {
                MessageBoxW(w, ui_string(L"Conecte um controle para habilitar o teclado nativo."), APP_NAME, MB_ICONINFORMATION);
                return 0;
            }
            for (uint32_t i = 0; i < user_control_count; i++) ini_set(g_global, "teclado", user_controls[i].key, g_keys_edit[i]);
            ini_set(g_global, "entrada", "teclado_nativo", keyboard ? "1" : "0");
            ini_set(g_global, "entrada", "mouse_nativo", SendMessageW(g_mouse_native, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            save_global();
            DestroyWindow(w);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(w);
        }
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->hwndFrom == g_keys_list && ((NMHDR *)lp)->code == NM_DBLCLK) keys_edit(w, ID_KEY_SET);
        return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void configure_keys(HWND owner) {
    for (uint32_t i = 0; i < user_control_count && i < 64; i++) {
        const char *v = ini_get(g_global, "teclado", user_controls[i].key);
        snprintf(g_keys_edit[i], 64, "%s", v ? v : user_controls[i].keys);
    }
    HWND w = create_dialog(L"ZeraPS5Keys", ui_string(L"Teclado e mouse"), 520, 590, owner);
    add_control(w, L"STATIC", ui_string(L"Duplo clique troca a tecla. DualSense e Xbox funcionam sempre."), 0, 16, 12, 490,
                20, 0);
    g_keys_list = add_control(w, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                              16, 38, 360, 380, 0);
    ListView_SetExtendedListViewStyle(g_keys_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    static const wchar_t *const cols[2] = {L"Controle", L"Teclas"};
    for (int i = 0; i < 2; i++) {
        LVCOLUMNW c = {0};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)ui_string(cols[i]);
        c.cx = S(i ? 130 : 205);
        ListView_InsertColumn(g_keys_list, i, &c);
    }
    keys_refresh();
    add_button(w, ui_string(L"Trocar tecla..."), 390, 38, 116, ID_KEY_SET);
    add_button(w, ui_string(L"Adicionar tecla..."), 390, 72, 116, ID_KEY_ADD);
    add_button(w, ui_string(L"Limpar"), 390, 106, 116, ID_KEY_CLEAR);
    add_button(w, ui_string(L"Restaurar padrões"), 390, 150, 116, ID_KEY_DEFAULTS);
    g_keys_native = add_control(w, L"BUTTON", ui_string(L"Teclado nativo (jogos compatíveis)"), WS_TABSTOP | BS_AUTOCHECKBOX, 16, 430, 490, 22, 354);
    SendMessageW(g_keys_native, BM_SETCHECK, ini_get_int(g_global, "entrada", "teclado_nativo", 0) == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    g_mouse_native = add_control(w, L"BUTTON", ui_string(L"Mouse nativo (jogos compatíveis)"), WS_TABSTOP | BS_AUTOCHECKBOX, 16, 456, 490, 22, 355);
    SendMessageW(g_mouse_native, BM_SETCHECK, ini_get_int(g_global, "entrada", "mouse_nativo", 0) == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    g_keys_status = add_control(w, L"STATIC", L"", 0, 16, 484, 490, 22, 0);
    add_control(w, L"STATIC", ui_string(L"No jogo: clique captura o mouse; F8 solta ou captura."), 0, 16, 508, 490, 22, 0);
    add_button(w, ui_string(L"Salvar"), 316, 550, 90, IDOK);
    add_button(w, ui_string(L"Cancelar"), 416, 550, 90, IDCANCEL);
    SendMessageW(w, WM_TIMER, 353, 0);
    SetTimer(w, 353, 500, NULL);
    run_modal(w, owner, true);
}

/* ---- Controller: the PlayStation controller as the games will see it ---- */

static HWND g_pad_status, g_pad_live, g_pad_rumble, g_pad_players;
static const choice k_players[] = {{1, L"1 (só você)"}, {2, L"2"}, {3, L"3"}, {4, L"4"}};
static wchar_t g_pad_text[2][1024];
enum { ID_PAD_TEST = 350, ID_PAD_TIMER = 351, ID_PAD_STOP = 352 };

static void pad_set_text(HWND h, wchar_t *last, const wchar_t *text) {
    if (wcscmp(last, text)) { /* unchanged text is not redrawn: no flicker */
        wcsncpy(last, text, 1023);
        last[1023] = 0;
        SetWindowTextW(h, text);
    }
}

static void pad_refresh(void) {
    ds_state s;
    wchar_t status[256], live[1024];
    if (!ds_read(&s) || !s.reports) {
        swprintf(status, 256, ui_string(L"Nenhum controle PlayStation encontrado."));
        swprintf(live, 1024,
                 ui_string(L"Ligue o DualSense (ou DualShock 4) no cabo USB. Pelo Bluetooth: segure Create + PS até a luz "
                 L"piscar e pareie em Configurações do Windows > Bluetooth.\r\n\r\nControles de Xbox funcionam "
                 L"sempre, pelo XInput."));
    } else {
        wchar_t name[64];
        utf8_to_wide(ds_name(&s), name, 64);
        swprintf(status, 256, ui_string(L"%ls conectado — bateria %u%%%ls"), name, s.battery, s.charging ? ui_string(L" (carregando)") : L"");
        static const struct {
            uint32_t bit;
            const wchar_t *name;
        } k_buttons[] = {
            {PAL_PAD_CROSS, L"Cruz"},  {PAL_PAD_CIRCLE, L"Círculo"},   {PAL_PAD_SQUARE, L"Quadrado"},
            {PAL_PAD_TRIANGLE, L"Triângulo"}, {PAL_PAD_UP, L"Cima"}, {PAL_PAD_DOWN, L"Baixo"},
            {PAL_PAD_LEFT, L"Esquerda"}, {PAL_PAD_RIGHT, L"Direita"}, {PAL_PAD_L1, L"L1"},
            {PAL_PAD_R1, L"R1"},       {PAL_PAD_L2, L"L2"},           {PAL_PAD_R2, L"R2"},
            {PAL_PAD_L3, L"L3"},       {PAL_PAD_R3, L"R3"},           {PAL_PAD_OPTIONS, L"Options"},
            {PAL_PAD_TOUCH_PAD, L"Touchpad"},
        };
        wchar_t pressed[512] = L"";
        for (size_t i = 0; i < sizeof(k_buttons) / sizeof(k_buttons[0]); i++) {
            if (!(s.buttons & k_buttons[i].bit)) continue;
            if (pressed[0]) wcscat(pressed, L", ");
            wcscat(pressed, ui_string(k_buttons[i].name));
        }
        if (s.ps) wcscat(wcscat(pressed, pressed[0] ? L", " : L""), L"PS");
        if (s.create) wcscat(wcscat(pressed, pressed[0] ? L", " : L""), ui_string(L"Create"));
        wchar_t touch[96];
        swprintf(touch, 96, L"%ls", ui_string(L"nenhum toque"));
        if (s.touch[0].down)
            swprintf(touch, 96, L"%u, %u%ls", s.touch[0].x, s.touch[0].y, s.touch[1].down ? ui_string(L" (+1 dedo)") : L"");
        swprintf(live, 1024,
                 ui_string(L"Botões: %ls\r\n\r\nAnalógico esquerdo: %3d, %3d    direito: %3d, %3d\r\n"
                 L"Gatilhos: L2 %3u    R2 %3u\r\nTouchpad: %ls\r\nMovimento: %+.2f, %+.2f, %+.2f G"),
                 pressed[0] ? pressed : ui_string(L"(nenhum)"), s.lx - 128, s.ly - 128, s.rx - 128, s.ry - 128, s.l2, s.r2, touch,
                 s.accel[0], s.accel[1], s.accel[2]);
    }
    pad_set_text(g_pad_status, g_pad_text[0], status);
    pad_set_text(g_pad_live, g_pad_text[1], live);
}

static LRESULT CALLBACK pad_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_PAD_TIMER) pad_refresh();
        if (wp == ID_PAD_STOP) {
            KillTimer(w, ID_PAD_STOP);
            ds_set_rumble(0, 0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PAD_TEST) {
            ds_set_rumble(200, 200);
            SetTimer(w, ID_PAD_STOP, 700, NULL);
        } else if (LOWORD(wp) == IDOK) {
            ini_set(g_global, "controle", "vibracao",
                    SendMessageW(g_pad_rumble, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            char players[4];
            snprintf(players, sizeof(players), "%d", combo_value(g_pad_players));
            ini_set(g_global, "controle", "jogadores", players);
            save_global();
            DestroyWindow(w);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(w);
        }
        return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY:
        KillTimer(w, ID_PAD_TIMER);
        KillTimer(w, ID_PAD_STOP);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void configure_pad(HWND owner) {
    ds_start();
    HWND w = create_dialog(L"ZeraPS5Pad", ui_string(L"Controle"), 520, 370, owner);
    g_pad_status = add_control(w, L"STATIC", L"", 0, 16, 14, 490, 22, 0);
    g_pad_live = add_control(w, L"STATIC", L"", 0, 16, 44, 490, 150, 0);
    g_pad_text[0][0] = g_pad_text[1][0] = 0;
    g_pad_rumble = add_control(w, L"BUTTON", ui_string(L"Vibração nos jogos"), WS_TABSTOP | BS_AUTOCHECKBOX, 16, 206, 250, 22, 0);
    SendMessageW(g_pad_rumble, BM_SETCHECK, ini_get_int(g_global, "controle", "vibracao", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    add_button(w, ui_string(L"Testar vibração"), 340, 204, 166, ID_PAD_TEST);
    add_control(w, L"STATIC", ui_string(L"Jogadores locais:"), 0, 16, 244, 150, 22, 0);
    int players = ini_get_int(g_global, "controle", "jogadores", 1);
    g_pad_players = add_combo(w, 170, 240, 120, k_players, 4, players < 1 ? 1 : players > 4 ? 4 : players, NULL);
    add_control(w, L"STATIC", ui_string(L"Funcionam: DualSense e DualSense Edge (USB e Bluetooth), DualShock 4 (USB), controles de Xbox. "
                              L"Do jogador 2 em diante, um controle de Xbox para cada."),
                0, 16, 276, 490, 40, 0);
    add_button(w, ui_string(L"Salvar"), 316, 330, 90, IDOK);
    add_button(w, ui_string(L"Cancelar"), 416, 330, 90, IDCANCEL);
    pad_refresh();
    SetTimer(w, ID_PAD_TIMER, 50, NULL);
    run_modal(w, owner, true);
    ds_stop(); /* the game opens the controller itself */
}

/* ---- Host GPUs (ps5c --list-gpus: "number<TAB>dedicated|integrated<TAB>name" lines) ---- */

#define MAX_GPUS 16
static char g_gpu_names[MAX_GPUS][256];
static bool g_gpu_discrete[MAX_GPUS];
static int g_gpu_count;

/* Runs ps5c.exe with one option and returns what it printed (buf: cap bytes, ends with 0) */
static void ps5c_output(const wchar_t *option, char *buf, DWORD cap) {
    buf[0] = 0;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
    swprintf(exe, MAX_PATH, L"%ls\\ps5c.exe", g_base);
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" %ls", exe, option);
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = NULL;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_base, &si, &pi);
    CloseHandle(wr);
    DWORD len = 0, got;
    while (ok && len < cap - 1 && ReadFile(rd, buf + len, cap - 1 - len, &got, NULL) && got) len += got;
    buf[len] = 0;
    CloseHandle(rd);
    if (ok) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void list_gpus(void) {
    g_gpu_count = 0;
    char buf[8192];
    ps5c_output(L"--list-gpus", buf, sizeof(buf));
    for (char *line = buf; *line && g_gpu_count < MAX_GPUS;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *kind = strchr(line, '\t'), *name = kind ? strchr(kind + 1, '\t') : NULL;
        if (name) {
            size_t n = strlen(name + 1);
            while (n && (name[n] == '\r' || name[n] == ' ')) name[n--] = 0;
            snprintf(g_gpu_names[g_gpu_count], 256, "%s", name + 1);
            g_gpu_discrete[g_gpu_count++] = !strncmp(kind + 1, "dedicated", 9);
        }
        line = next ? next : line + strlen(line);
    }
}

/* ---- Accessories: keyboard and mouse, controller, camera ([acessorios] camera) ---- */

#define MAX_CAMERAS 16
static HWND g_acc_camera, g_acc_state;
static char g_acc_names[MAX_CAMERAS][128]; /* host cameras that are not a PS4 Camera */
static int g_acc_count;
static char g_acc_other[128];              /* a camera of the settings this PC does not have (kept) */
enum { ID_ACC_KEYS = 370, ID_ACC_PAD, ID_ACC_REFRESH };

/* combo items: 0 off, 1 the first webcam, 2 the PS4 Camera, 3+i webcam i, 99 g_acc_other */
static void accessories_fill(void) {
    char buf[8192], state[32] = "absent";
    ps5c_output(L"--list-cameras", buf, sizeof(buf));
    g_acc_count = 0;
    for (char *line = buf; *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (!strncmp(line, "ps4\t", 4)) snprintf(state, sizeof(state), "%s", line + 4);
        else if (!strncmp(line, "camera\t", 7) && !strstr(line, "OV580") && g_acc_count < MAX_CAMERAS)
            snprintf(g_acc_names[g_acc_count++], 128, "%s", line + 7);
        line = next ? next : line + n;
    }
    const char *v = ini_get(g_global, "acessorios", "camera");
    int want = 0;
    g_acc_other[0] = 0;
    if (v && (!strcmp(v, "generica") || !strcmp(v, "1"))) want = 1;
    else if (v && !strcmp(v, "ps4")) want = 2;
    else if (v && *v && strcmp(v, "desligada") && strcmp(v, "0")) {
        want = 99;
        for (int i = 0; i < g_acc_count; i++)
            if (!strcmp(g_acc_names[i], v)) want = 3 + i;
        if (want == 99) snprintf(g_acc_other, sizeof(g_acc_other), "%s", v);
    }
    SendMessageW(g_acc_camera, CB_RESETCONTENT, 0, 0);
    static const choice fixed[] = {{0, L"Desligada"}, {1, L"Webcam do PC (a primeira)"}, {2, L"PS4 Camera (com o adaptador da Sony)"}};
    int sel = 0;
    for (int i = 0; i < 3 + g_acc_count + (g_acc_other[0] != 0); i++) {
        wchar_t name[160];
        int value = i < 3 ? i : i < 3 + g_acc_count ? i : 99;
        if (i < 3) wcsncpy(name, ui_string(fixed[i].name), 159), name[159] = 0;
        else utf8_to_wide(value == 99 ? g_acc_other : g_acc_names[i - 3], name, 160);
        LRESULT item = SendMessageW(g_acc_camera, CB_ADDSTRING, 0, (LPARAM)name);
        if (item < 0) continue;
        SendMessageW(g_acc_camera, CB_SETITEMDATA, (WPARAM)item, value);
        if (value == want) sel = (int)item;
    }
    SendMessageW(g_acc_camera, CB_SETCURSEL, (WPARAM)sel, 0);
    wchar_t fw[MAX_PATH];
    swprintf(fw, MAX_PATH, L"%ls\\user\\ps4_camera_firmware.bin", g_base);
    const wchar_t *text = ui_string(L"PS4 Camera não encontrada.");
    if (!strcmp(state, "ready")) text = ui_string(L"PS4 Camera pronta.");
    else if (!strcmp(state, "driver"))
        text = ui_string(L"PS4 Camera conectada, mas sem o driver WinUSB no USB Boot (05A9:0580): instale-o com o Zadig.");
    else if (!strcmp(state, "firmware"))
        text = file_exists(fw) ? ui_string(L"PS4 Camera conectada: o firmware é enviado quando o jogo abrir a câmera.")
                               : ui_string(L"PS4 Camera conectada, mas falta o firmware do seu console em user/ps4_camera_firmware.bin.");
    wchar_t all[1024];
    swprintf(all, 1024, ui_string(L"Webcams neste PC: %d. %ls"), g_acc_count, text);
    SetWindowTextW(g_acc_state, all);
}

static LRESULT CALLBACK accessories_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ACC_KEYS: configure_keys(w); break;
        case ID_ACC_PAD: configure_pad(w); break;
        case ID_ACC_REFRESH: accessories_fill(); break;
        case IDOK: {
            int value = combo_value(g_acc_camera);
            const char *v = value == 1 ? "generica" : value == 2 ? "ps4" : value == 99 ? g_acc_other
                          : value >= 3 && value - 3 < g_acc_count ? g_acc_names[value - 3] : "desligada";
            ini_set(g_global, "acessorios", "camera", v);
            save_global();
            DestroyWindow(w);
            break;
        }
        case IDCANCEL: DestroyWindow(w); break;
        }
        return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void configure_accessories(HWND owner) {
    HWND w = create_dialog(L"ZeraPS5Accessories", ui_string(L"Acessórios"), 520, 300, owner);
    add_control(w, L"STATIC", ui_string(L"Teclado, mouse e controle:"), 0, 16, 16, 490, 20, 0);
    add_button(w, ui_string(L"Teclado e mouse..."), 16, 40, 170, ID_ACC_KEYS);
    add_button(w, ui_string(L"Controle (DualSense)..."), 196, 40, 170, ID_ACC_PAD);
    add_control(w, L"STATIC", ui_string(L"Câmera (PlayStation Camera):"), 0, 16, 92, 200, 20, 0);
    g_acc_camera = add_control(w, WC_COMBOBOXW, NULL, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 220, 88, 286, 260, 0);
    g_acc_state = add_control(w, L"STATIC", L"", 0, 16, 124, 370, 54, 0);
    add_button(w, ui_string(L"Procurar de novo"), 396, 124, 110, ID_ACC_REFRESH);
    add_control(w, L"STATIC", ui_string(L"Com uma webcam comum as duas lentes veem a mesma imagem: o que depende de profundidade não funciona."),
                0, 16, 190, 490, 40, 0);
    accessories_fill();
    add_button(w, ui_string(L"Salvar"), 316, 256, 90, IDOK);
    add_button(w, ui_string(L"Cancelar"), 416, 256, 90, IDCANCEL);
    run_modal(w, owner, true);
}

/* ---- General settings ---- */

static HWND g_set_lang, g_set_res, g_set_screen, g_set_gpu, g_set_folders, g_set_system, g_set_update, g_set_autoupd, g_set_autolog, g_set_panel,
    g_set_music, g_set_intro, g_set_nosound, g_set_cpu;
static HWND g_set_ue, g_set_internal, g_set_fsr, g_set_fsr_sharp;
static char g_set_gpu_other[256]; /* a GPU of the settings this PC does not have (kept as it is) */
enum { ID_FOLDER_ADD = 400, ID_FOLDER_REMOVE, ID_SYSTEM_BROWSE, ID_ACCESSORIES, ID_UPDATE_CHECK, ID_PAD };

static void settings_save(void) {
    char v[3 * MAX_PATH];
    wchar_t path[MAX_PATH];
    snprintf(v, sizeof(v), "%d", combo_value(g_set_lang));
    ini_set(g_global, "geral", "idioma", v);
    snprintf(v, sizeof(v), "%d", combo_value(g_set_res));
    ini_set(g_global, "geral", "resolucao", v);
    snprintf(v, sizeof(v), "%d", combo_value(g_set_screen));
    ini_set(g_global, "video", "tela", v);
    snprintf(v, sizeof(v), "%d", combo_value(g_set_ue));
    ini_set(g_global, "geral", "escala_unreal", v);
    snprintf(v, sizeof(v), "%d", combo_value(g_set_internal));
    ini_set(g_global, "geral", "resolucao_interna", v);
    snprintf(v, sizeof(v), "%d", combo_value(g_set_fsr_sharp));
    ini_set(g_global, "geral", "nitidez_fsr", v);
    /* the folder list replaces jogos1..N */
    const char *old[64];
    char keys[64][16];
    uint32_t n = ini_keys(g_global, "pastas", old, 64);
    int drop = 0;
    for (uint32_t i = 0; i < n && i < 64; i++)
        if (!_strnicmp(old[i], "jogos", 5)) snprintf(keys[drop++], 16, "%s", old[i]);
    for (int i = 0; i < drop; i++) ini_set(g_global, "pastas", keys[i], NULL);
    int count = (int)SendMessageW(g_set_folders, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < count && i < 64; i++) {
        char key[16];
        SendMessageW(g_set_folders, LB_GETTEXT, (WPARAM)i, (LPARAM)path);
        wide_to_utf8(path, v, sizeof(v));
        snprintf(key, sizeof(key), "jogos%d", i + 1);
        ini_set(g_global, "pastas", key, v);
    }
    GetWindowTextW(g_set_system, path, MAX_PATH);
    wide_to_utf8(path, v, sizeof(v));
    ini_set(g_global, "pastas", "sistema", v);
    /* the GPU by name: the numbering may differ after a driver or hardware change */
    int gpu = (int)SendMessageW(g_set_gpu, CB_GETCURSEL, 0, 0);
    const char *gname = gpu <= 0 ? "auto" : gpu <= g_gpu_count ? g_gpu_names[gpu - 1] : g_set_gpu_other;
    ini_set(g_global, "video", "placa", gname);
    ini_set(g_global, "atualizacao", "ao_iniciar", SendMessageW(g_set_autoupd, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
    ini_set(g_global, "relatorios", "enviar_ao_fechar", SendMessageW(g_set_autolog, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
    ini_set(g_global, "geral", "painel", SendMessageW(g_set_panel, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
    ini_set(g_global, "geral", "upscale_fsr", SendMessageW(g_set_fsr, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
    ini_set(g_global, "geral", "musica", SendMessageW(g_set_music, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
    ini_set(g_global, "geral", "abertura", SendMessageW(g_set_intro, BM_GETCHECK, 0, 0) == BST_CHECKED ? "0" : "1");
    ini_set(g_global, "geral", "som", SendMessageW(g_set_nosound, BM_GETCHECK, 0, 0) == BST_CHECKED ? "0" : "1");
    snprintf(v, sizeof(v), "%d", combo_value(g_set_cpu));
    ini_set(g_global, "geral", "cpu_antiga", v);
    save_global();
}

static LRESULT CALLBACK settings_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        load_global(); /* keyboard edits saved on their own; nothing else changed */
        DestroyWindow(w);
        return 0;
    }
    if (msg != WM_COMMAND) return DefWindowProcW(w, msg, wp, lp);
    wchar_t path[MAX_PATH];
    char rel[3 * MAX_PATH];
    switch (LOWORD(wp)) {
    case ID_FOLDER_ADD:
        if (browse_folder(w, ui_string(L"Escolha uma pasta com jogos extra\u00eddos ou imagens .exfat"), path)) {
            relative_path(path, rel, sizeof(rel));
            utf8_to_wide(rel, path, MAX_PATH);
            if (SendMessageW(g_set_folders, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)path) == LB_ERR)
                SendMessageW(g_set_folders, LB_ADDSTRING, 0, (LPARAM)path);
        }
        break;
    case ID_FOLDER_REMOVE: {
        LRESULT sel = SendMessageW(g_set_folders, LB_GETCURSEL, 0, 0);
        if (sel != LB_ERR) SendMessageW(g_set_folders, LB_DELETESTRING, (WPARAM)sel, 0);
        break;
    }
    case ID_SYSTEM_BROWSE:
        if (browse_folder(w, ui_string(L"Escolha a pasta com as bibliotecas de sistema do seu console"), path)) {
            relative_path(path, rel, sizeof(rel));
            utf8_to_wide(rel, path, MAX_PATH);
            SetWindowTextW(g_set_system, path);
        }
        break;
    case ID_ACCESSORIES: configure_accessories(w); break;
    case ID_PAD: configure_pad(w); break;
    case ID_UI_LANGUAGES: configure_languages(w); break;
    case ID_UPDATE_CHECK: check_updates(NULL, true); break;
    case IDOK:
        settings_save();
        DestroyWindow(w);
        fill_list();
        break;
    case IDCANCEL:
        load_global();
        DestroyWindow(w);
        break;
    }
    return 0;
}

static void configure_general(void) {
    load_global(); /* the file may have been edited by hand */
    HWND w = create_dialog(L"ZeraPS5Settings", ui_string(L"Configurações"), 520, 746, g_main);
    add_control(w, L"STATIC", ui_string(L"Idioma do console:"), 0, 16, 18, 150, 20, 0);
    g_set_lang = add_combo(w, 170, 14, 220, k_languages, LANGUAGE_COUNT,
                           (int)ini_get_int(g_global, "geral", "idioma", USER_LANGUAGE_DEFAULT), NULL);
    add_control(w, L"STATIC", ui_string(L"Resolução da TV:"), 0, 16, 54, 150, 20, 0);
    g_set_res = add_combo(w, 170, 50, 220, k_resolutions, RESOLUTION_COUNT,
                          (int)ini_get_int(g_global, "geral", "resolucao", USER_RESOLUTION_DEFAULT), NULL);
    add_control(w, L"STATIC", ui_string(L"Placa de vídeo:"), 0, 16, 90, 150, 20, 0);
    g_set_gpu = add_control(w, WC_COMBOBOXW, NULL, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 170, 86, 336, 260, 0);
    SendMessageW(g_set_gpu, CB_ADDSTRING, 0, (LPARAM)ui_string(L"Automática (a dedicada, se houver)"));
    list_gpus();
    const char *want = ini_get(g_global, "video", "placa");
    int sel = 0;
    char *end = NULL;
    long want_index = want && *want ? strtol(want, &end, 10) : -1;
    if (want && *want && *end) want_index = -1;
    g_set_gpu_other[0] = 0;
    for (int i = 0; i < g_gpu_count; i++) {
        wchar_t name[300];
        wchar_t model[256];
        utf8_to_wide(g_gpu_names[i], model, 256);
        swprintf(name,300,L"%ls (%ls)",model,ui_string(g_gpu_discrete[i]?L"dedicada":L"integrada"));
        SendMessageW(g_set_gpu, CB_ADDSTRING, 0, (LPARAM)name);
        if (want && (want_index >= 0 ? want_index == i : !_stricmp(want, g_gpu_names[i]))) sel = i + 1;
    }
    if (want && *want && _stricmp(want, "auto") && !sel) {
        /* chosen on another PC or a GPU not present now: kept unless changed */
        snprintf(g_set_gpu_other, sizeof(g_set_gpu_other), "%s", want);
        wchar_t name[300], model[256];
        utf8_to_wide(want, model, 256);
        swprintf(name,300,ui_string(L"%ls (não encontrada neste PC)"),model);
        SendMessageW(g_set_gpu, CB_ADDSTRING, 0, (LPARAM)name);
        sel = g_gpu_count + 1;
    }
    SendMessageW(g_set_gpu, CB_SETCURSEL, (WPARAM)sel, 0);
    add_control(w, L"STATIC", ui_string(L"Modo de tela:"), 0, 16, 126, 150, 20, 0);
    g_set_screen = add_combo(w, 170, 122, 220, k_screen_modes, SCREEN_MODE_COUNT,
                             (int)ini_get_int(g_global, "video", "tela", 0), NULL);
    add_control(w, L"STATIC", L"Resolução interna:", 0, 16, 154, 205, 20, 0);
    g_set_internal = add_combo(w, 226, 150, 220, k_internal_res, INTERNAL_RES_COUNT,
                               internal_res_value(ini_get_int(g_global, "geral", "resolucao_interna", 0)), NULL);
    add_control(w, L"STATIC", L"Escala Unreal (avançado):", 0, 16, 186, 205, 20, 0);
    g_set_ue = add_ue_scale_combo(w, 226, 182, 220, ini_get_int(g_global, "geral", "escala_unreal", 0), NULL);
    add_control(w, L"STATIC", L"Jogos Unreal, Sonic Origins Plus e TMNT: Splintered Fate.", 0, 16, 212, 490, 20, 0);
    g_set_fsr = add_control(w, L"BUTTON", L"Upscale FSR 1 (tela cheia ou maximizada)",
                            WS_TABSTOP | BS_AUTOCHECKBOX, 16, 236, 290, 22, 0);
    add_control(w, L"STATIC", L"Nitidez:", 0, 316, 240, 64, 20, 0);
    g_set_fsr_sharp = add_combo(w, 386, 234, 120, k_fsr_sharpness, FSR_SHARPNESS_COUNT,
                                (int)ini_get_int(g_global, "geral", "nitidez_fsr", 1), NULL);
    SendMessageW(g_set_fsr, BM_SETCHECK, ini_get_int(g_global, "geral", "upscale_fsr", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    add_control(w, L"STATIC", ui_string(L"Pastas de jogos:"), 0, 16, 266, 300, 20, 0);
    g_set_folders = add_control(w, L"LISTBOX", NULL, WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 16, 288, 360, 58, 0);
    const char *folders[64];
    int n = game_folders(folders, 64);
    for (int i = 0; i < n; i++) {
        wchar_t f[MAX_PATH];
        utf8_to_wide(folders[i], f, MAX_PATH);
        SendMessageW(g_set_folders, LB_ADDSTRING, 0, (LPARAM)f);
    }
    add_button(w, ui_string(L"Adicionar..."), 390, 288, 116, ID_FOLDER_ADD);
    add_button(w, ui_string(L"Remover"), 390, 322, 116, ID_FOLDER_REMOVE);
    add_control(w, L"STATIC", ui_string(L"Bibliotecas de sistema do console:"), 0, 16, 358, 300, 20, 0);
    const char *sys = ini_get(g_global, "pastas", "sistema");
    wchar_t sysw[MAX_PATH];
    utf8_to_wide(sys && *sys ? sys : "system\\modules", sysw, MAX_PATH);
    g_set_system = add_control(w, L"EDIT", sysw, WS_TABSTOP | ES_AUTOHSCROLL, 16, 380, 360, 24, 0);
    SetPropW(g_set_system,L"ZeraPS5.LTR",(HANDLE)1);
    ui_apply_control(g_set_system,0);
    add_button(w, ui_string(L"Procurar..."), 390, 378, 116, ID_SYSTEM_BROWSE);
    add_control(w, L"STATIC", ui_string(L"Atualizações vêm de:"), 0, 16, 418, 360, 20, 0);
    wchar_t updw[2 * MAX_PATH];
    utf8_to_wide(update_source(), updw, 2 * MAX_PATH);
    g_set_update = add_control(w, L"EDIT", updw, ES_READONLY | ES_AUTOHSCROLL, 16, 440, 360, 24, 0);
    SetPropW(g_set_update,L"ZeraPS5.LTR",(HANDLE)1);
    ui_apply_control(g_set_update,0);
    add_button(w, ui_string(L"Verificar agora"), 390, 438, 116, ID_UPDATE_CHECK);
    g_set_autoupd = add_control(w, L"BUTTON", ui_string(L"Verificar atualizações ao iniciar"), WS_TABSTOP | BS_AUTOCHECKBOX, 16, 470, 360, 22, 0);
    SendMessageW(g_set_autoupd, BM_SETCHECK, ini_get_int(g_global, "atualizacao", "ao_iniciar", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    g_set_autolog = add_control(w, L"BUTTON", ui_string(L"Enviar o log para análise quando o jogo fechar"), WS_TABSTOP | BS_AUTOCHECKBOX,
                                16, 494, 400, 22, 0);
    SendMessageW(g_set_autolog, BM_SETCHECK,
                 ini_get_int(g_global, "relatorios", "enviar_ao_fechar", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    g_set_panel = add_control(w, L"BUTTON", ui_string(L"Painel de desempenho no jogo (F10 mostra e esconde)"), WS_TABSTOP | BS_AUTOCHECKBOX,
                              16, 518, 420, 22, 0);
    SendMessageW(g_set_panel, BM_SETCHECK, ini_get_int(g_global, "geral", "painel", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    g_set_music = add_control(w, L"BUTTON", ui_string(L"Tocar a música do jogo selecionado na lista"), WS_TABSTOP | BS_AUTOCHECKBOX,
                              16, 542, 420, 22, 0);
    SendMessageW(g_set_music, BM_SETCHECK, ini_get_int(g_global, "geral", "musica", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    g_set_intro = add_control(w, L"BUTTON", ui_string(L"Pular a intro (vídeo de abertura dos jogos)"), WS_TABSTOP | BS_AUTOCHECKBOX,
                              16, 566, 420, 22, 0);
    SendMessageW(g_set_intro, BM_SETCHECK, ini_get_int(g_global, "geral", "abertura", 1) ? BST_UNCHECKED : BST_CHECKED, 0);
    g_set_nosound = add_control(w, L"BUTTON", ui_string(L"Sem som (PC sem saída de áudio)"), WS_TABSTOP | BS_AUTOCHECKBOX,
                                16, 590, 420, 22, 0);
    SendMessageW(g_set_nosound, BM_SETCHECK, ini_get_int(g_global, "geral", "som", 1) ? BST_UNCHECKED : BST_CHECKED, 0);
    add_control(w, L"STATIC", ui_string(L"Processador antigo:"), 0, 16, 622, 150, 20, 0);
    g_set_cpu = add_combo(w, 170, 618, 336, k_cpu_modes, CPU_MODE_COUNT,
                          (int)ini_get_int(g_global, "geral", "cpu_antiga", 0), NULL);
    add_button(w, ui_string(L"Acessórios..."), 16, 656, 170, ID_ACCESSORIES);
    add_button(w, ui_string(L"Controle (DualSense)..."), 196, 656, 170, ID_PAD);
    add_button(w,L"Idiomas...",376,656,130,ID_UI_LANGUAGES);
    add_button(w, ui_string(L"Salvar"), 316, 704, 90, IDOK);
    add_button(w, ui_string(L"Cancelar"), 416, 704, 90, IDCANCEL);
    run_modal(w, g_main, true);
}

/* ---- Games ---- */

extern int WINAPI GdipCreateBitmapFromStream(IStream *, void **);
/* png is a file, or with png NULL an image of mem_bytes at mem */
static int load_icon_from(HIMAGELIST il, const wchar_t *png, const uint8_t *mem, uint64_t mem_bytes) {
    void *bmp = NULL, *thumb = NULL;
    HBITMAP hb = NULL;
    int index = -1;
    IStream *stream = NULL;
    char utf8[4 * MAX_PATH] = "";
    if (png) wide_to_utf8(png, utf8, sizeof(utf8));
    int opened = -1;
    if (!png) {
        stream = mem && mem_bytes <= (8u << 20) ? SHCreateMemStream(mem, (UINT)mem_bytes) : NULL;
        if (stream) opened = GdipCreateBitmapFromStream(stream, &bmp);
    } else if (exfat_path(utf8)) {
        pal_path_info pi;
        uint8_t *data = NULL; uint64_t bytes = 0;
        if (pal_path_stat(utf8, &pi) == PS5C_OK && pi.size <= (8u << 20) &&
            pal_file_read_all(utf8, &data, &bytes) == PS5C_OK) {
            stream = SHCreateMemStream(data, (UINT)bytes);
            free(data);
            if (stream) opened = GdipCreateBitmapFromStream(stream, &bmp);
        }
    } else opened = GdipCreateBitmapFromFile(png, &bmp);
    if (opened == 0 && GdipGetImageThumbnail(bmp, S(ICON_SIZE), S(ICON_SIZE), &thumb, NULL, NULL) == 0 &&
        GdipCreateHBITMAPFromBitmap(thumb, &hb, 0xFFFFFFFF) == 0) {
        index = ImageList_Add(il, hb, NULL);
        DeleteObject(hb);
    }
    if (thumb) GdipDisposeImage(thumb);
    if (bmp) GdipDisposeImage(bmp);
    if (stream) stream->lpVtbl->Release(stream);
    return index;
}
static int load_icon(HIMAGELIST il, const wchar_t *png) { return load_icon_from(il, png, NULL, 0); }

/* PS4 sce_sys/param.sfo: "\0PSF", key table at 8, data table at 12, entry count at 16, then
 * 16-byte entries {u16 key offset, u16 format, u32 length, u32 max length, u32 data offset};
 * TITLE, TITLE_ID and APP_VER are UTF-8 strings */
static bool parse_sfo(const uint8_t *src, size_t n, game *g) {
    static uint8_t b[64 * 1024 + 1];
    if (n > sizeof(b) - 1) n = sizeof(b) - 1;
    memcpy(b, src, n);
    b[n] = 0; /* key names end inside the buffer */
    if (n < 20 || memcmp(b, "\0PSF", 4)) return false;
    uint32_t keys = b[8] | b[9] << 8 | b[10] << 16 | (uint32_t)b[11] << 24;
    uint32_t data = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
    uint32_t count = b[16] | b[17] << 8 | b[18] << 16 | (uint32_t)b[19] << 24;
    for (uint32_t i = 0; i < count && 20 + 16 * (i + 1) <= n; i++) {
        const uint8_t *e = b + 20 + 16 * i;
        uint32_t key = keys + (e[0] | e[1] << 8), len = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
        uint32_t at = data + (e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24);
        if (key >= n || at >= n || len > n - at || (e[2] | e[3] << 8) != 0x0204) continue; /* UTF-8 strings only */
        char value[512];
        size_t m = len < sizeof(value) - 1 ? len : sizeof(value) - 1;
        memcpy(value, b + at, m);
        value[m] = 0;
        const char *k = (const char *)b + key;
        if (!strcmp(k, "TITLE")) utf8_to_wide(value, g->title, 256);
        else if (!strcmp(k, "TITLE_ID")) utf8_to_wide(value, g->id, 32);
        else if (!strcmp(k, "APP_VER")) utf8_to_wide(value, g->version, 32);
    }
    return true;
}

static bool read_sfo(const wchar_t *path, game *g) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return false;
    static uint8_t b[64 * 1024];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    return parse_sfo(b, n, g);
}

static bool is_update_folder(const wchar_t *name) {
    size_t n = wcslen(name);
    return (n > 6 && !_wcsicmp(name + n - 6, L"-patch")) || (n > 7 && !_wcsicmp(name + n - 7, L"-patch0"));
}

static void add_game(const wchar_t *dir, const wchar_t *folder, HIMAGELIST il) {
    if (g_count == MAX_GAMES) return;
    game *g = &g_games[g_count];
    memset(g, 0, sizeof(*g));
    wcsncpy(g->dir, dir, MAX_PATH - 1);
    char source[4 * MAX_PATH];
    wide_to_utf8(dir, source, sizeof(source));
    g->is_exfat = exfat_path(source);
    wchar_t path[MAX_PATH];
    param_info p;
    swprintf(path, MAX_PATH, L"%ls\\sce_sys\\param.json", dir);
    if (read_param(path, &p)) {
        const char *t = param_title(&p, "pt-BR");
        if (!t) t = param_title(&p, "en-US");
        if (!t) t = param_title(&p, p.default_lang);
        if (!t && p.title_count) t = p.titles[0][1];
        if (t) utf8_to_wide(t, g->title, 256);
        utf8_to_wide(p.id, g->id, 32);
        utf8_to_wide(p.version, g->version, 32);
        g->pro = (p.attribute3 & 0x400000) != 0;
    } else {
        /* PS4: param.sfo, or a CUSA12345-app0 folder (the PS4 half of a PS5 package keeps no param.sfo) */
        swprintf(path, MAX_PATH, L"%ls\\sce_sys\\param.sfo", dir);
        const wchar_t *leaf = wcsrchr(dir, L'\\');
        leaf = leaf ? leaf + 1 : dir;
        g->ps4 = read_sfo(path, g) || !_wcsnicmp(leaf, L"CUSA", 4);
        if (g->ps4 && !g->id[0] && !_wcsnicmp(leaf, L"CUSA", 4) && wcslen(leaf) >= 9) wcsncpy(g->id, leaf, 9);
    }
    if (!g->title[0]) wcsncpy(g->title, folder, 255);
    if (!g->id[0]) {
        /* no title ID: the folder name, without characters a file name cannot hold */
        wcsncpy(g->id, folder, 31);
        for (wchar_t *c = g->id; *c; c++)
            if (wcschr(L"\\/:*?\"<>| ", *c)) *c = L'_';
    }
    swprintf(path, MAX_PATH, L"%ls\\sce_sys\\icon0.png", dir);
    g->image = load_icon(il, path);
    g_count++;
}

static void add_exfat_game(const wchar_t *image, const wchar_t *name, HIMAGELIST il) {
    char path[4 * MAX_PATH], eboot[4 * MAX_PATH];
    wide_to_utf8(image, path, sizeof(path));
    if (!exfat_image_path(path)) return;
    ps5c_err err = exfat_game_path(path, eboot, sizeof(eboot));
    if (err != PS5C_OK) return;
    char *slash = strrchr(eboot, '/');
    if (!slash) return;
    *slash = 0;
    wchar_t dir[MAX_PATH]; utf8_to_wide(eboot, dir, MAX_PATH);
    if (dir[0]) add_game(dir, name, il);
}

/* A .pkg: title, version and icon from the package's metadata entries. */
static void add_pkg_game(const wchar_t *file, const wchar_t *name, HIMAGELIST il) {
    if (g_count == MAX_GAMES) return;
    char path[4 * MAX_PATH];
    wide_to_utf8(file, path, sizeof(path));
    if (!pkg_path(path)) return;
    pkg_info info;
    if (pkg_inspect(path, &info) != PS5C_OK) return;
    game *g = &g_games[g_count];
    memset(g, 0, sizeof(*g));
    wcsncpy(g->dir, file, MAX_PATH - 1);
    g->is_pkg = true;
    g->pkg_retail = info.retail;
    g->ps4 = info.kind == PKG_PS4;
    uint8_t *data;
    uint64_t size;
    const pkg_entry *e = pkg_find(&info, "param.json");
    if (e && pkg_read_entry(path, e, &data, &size) == PS5C_OK) {
        static param_info p;
        if (parse_param((const char *)data, size, &p)) {
            const char *t = param_title(&p, "pt-BR");
            if (!t) t = param_title(&p, "en-US");
            if (!t) t = param_title(&p, p.default_lang);
            if (!t && p.title_count) t = p.titles[0][1];
            if (t) utf8_to_wide(t, g->title, 256);
            utf8_to_wide(p.id, g->id, 32);
            utf8_to_wide(p.version, g->version, 32);
            g->pro = (p.attribute3 & 0x400000) != 0;
        }
        free(data);
    } else if ((e = pkg_find(&info, "param.sfo")) && pkg_read_entry(path, e, &data, &size) == PS5C_OK) {
        parse_sfo(data, (size_t)size, g);
        free(data);
    }
    if (!g->id[0] && strlen(info.content_id) >= 16) {
        char id[10]; /* EP4350-PPSA01615_00-...: the title ID is characters 7..15 */
        snprintf(id, sizeof id, "%.9s", info.content_id + 7);
        utf8_to_wide(id, g->id, 32);
    }
    if (!g->title[0]) wcsncpy(g->title, name, 255);
    if (!g->id[0]) {
        wcsncpy(g->id, name, 31);
        for (wchar_t *c = g->id; *c; c++)
            if (wcschr(L"\\/:*?\"<>| ", *c)) *c = L'_';
    }
    g->image = -1;
    if ((e = pkg_find(&info, "icon0.png")) && pkg_read_entry(path, e, &data, &size) == PS5C_OK) {
        g->image = load_icon_from(il, NULL, data, size);
        free(data);
    }
    pkg_info_free(&info);
    g_count++;
}

/* <root>\<folder>\eboot.bin, or one level deeper (dumps nested in a folder) */
static void scan_games(const wchar_t *root, HIMAGELIST il) {
    WIN32_FIND_DATAW fd, sub;
    wchar_t all[MAX_PATH];
    swprintf(all, MAX_PATH, L"%ls\\*", root);
    HANDLE h = FindFirstFileW(all, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t image[MAX_PATH]; swprintf(image, MAX_PATH, L"%ls\\%ls", root, fd.cFileName);
            add_exfat_game(image, fd.cFileName, il);
            add_pkg_game(image, fd.cFileName, il);
            continue;
        }
        wchar_t dir[MAX_PATH], eboot[MAX_PATH];
        swprintf(dir, MAX_PATH, L"%ls\\%ls", root, fd.cFileName);
        swprintf(eboot, MAX_PATH, L"%ls\\eboot.bin", dir);
        if (file_exists(eboot)) {
            if (!is_update_folder(fd.cFileName)) add_game(dir, fd.cFileName, il);
            continue;
        }
        wchar_t pattern[MAX_PATH];
        swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
        HANDLE hs = FindFirstFileW(pattern, &sub);
        if (hs == INVALID_HANDLE_VALUE) continue;
        do {
            if (sub.cFileName[0] == L'.') continue;
            wchar_t inner[MAX_PATH];
            swprintf(inner, MAX_PATH, L"%ls\\%ls", dir, sub.cFileName);
            if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                /* a package kept in its own folder */
                add_pkg_game(inner, sub.cFileName, il);
                continue;
            }
            swprintf(eboot, MAX_PATH, L"%ls\\eboot.bin", inner);
            /* every game of the folder (a PS5 package can hold its PS4 version beside it); updates
             * (-patch0) are laid over their game when it runs */
            if (file_exists(eboot) && !is_update_folder(sub.cFileName)) add_game(inner, fd.cFileName, il);
        } while (FindNextFileW(hs, &sub));
        FindClose(hs);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int compare_games(const void *a, const void *b) {
    return _wcsicmp(((const game *)a)->title, ((const game *)b)->title);
}

static int count_modules(void) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    system_folder(pattern);
    wcscat(pattern, L"\\*.*prx");
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do n++;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void set_playtime_cell(int row,uint64_t milliseconds) {
    uint64_t seconds=milliseconds/1000;
    wchar_t text[40];
    swprintf(text,PS5C_ARRAY_COUNT(text),L"%llu:%02u:%02u",(unsigned long long)(seconds/3600),
             (unsigned)((seconds/60)%60),(unsigned)(seconds%60));
    ListView_SetItemText(g_list,row,PLAYTIME_COLUMN,text);
}

static uint64_t current_playtime(uint64_t elapsed) {
    uint64_t pending=elapsed>g_play_saved?elapsed-g_play_saved:0;
    return pending>UINT64_MAX-g_play_total?UINT64_MAX:g_play_total+pending;
}

static void fill_list(void) {
    ListView_DeleteAllItems(g_list);
    HIMAGELIST il = ListView_GetImageList(g_list, LVSIL_SMALL);
    ImageList_RemoveAll(il);
    g_count = 0;
    const char *folders[64];
    int nf = game_folders(folders, 64);
    for (int i = 0; i < nf; i++) {
        wchar_t root[MAX_PATH];
        full_path(folders[i], root);
        scan_games(root, il);
    }
    /* icons were added in scan order: keep each game's index while sorting */
    qsort(g_games, (size_t)g_count, sizeof(game), compare_games);
    for (int i = 0; i < g_count; i++) {
        LVITEMW it = {0};
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = i;
        it.pszText = g_games[i].title;
        it.lParam = i;
        it.iImage = g_games[i].image;
        ListView_InsertItem(g_list, &it);
        ListView_SetItemText(g_list, i, 1, (wchar_t *)(g_games[i].is_pkg ? (g_games[i].pkg_retail ? ui_string(L"PKG (loja)") : L"PKG")
                                                        : g_games[i].is_exfat ? L"exFAT" : ui_string(L"Pasta")));
        ListView_SetItemText(g_list, i, 2, g_games[i].id);
        ListView_SetItemText(g_list, i, 3, g_games[i].version);
        ListView_SetItemText(g_list, i, 4, g_games[i].ps4 ? L"PS4" : g_games[i].pro ? L"PS5 Pro" : L"PS5");
        set_playtime_cell(i,*g_play_id&&!_wcsicmp(g_games[i].id,g_play_id)?
                          current_playtime(GetTickCount64()-g_play_started):
                          report_session_playtime(g_base,g_games[i].id));
        /* inside the launcher's folder: shown relative to it */
        size_t bl = wcslen(g_base);
        wchar_t location[MAX_PATH];
        wcsncpy(location, g_games[i].dir, MAX_PATH - 1);
        location[MAX_PATH - 1] = 0;
        if (g_games[i].is_exfat) {
            wchar_t *view = wcsstr(location, L"/@game");
            if (!view) view = wcsstr(location, L"\\@game");
            if (view) *view = 0;
        }
        wchar_t *shown = location;
        if (!_wcsnicmp(shown, g_base, bl) && shown[bl] == L'\\') shown += bl + 1;
        ListView_SetItemText(g_list, i, PATH_COLUMN, shown);
    }
    int modules = count_modules();
    wchar_t text[512];
    if (!nf) {
        swprintf(text, 512, ui_string(L"Nenhuma pasta de jogos: adicione uma em Configurações."));
    } else if (modules) {
        swprintf(text, 512, ui_string(L"%d jogo(s) em %d pasta(s)    /    bibliotecas de sistema: %d"), g_count, nf, modules);
    } else {
        swprintf(text,512,L"%ls",ui_string(L"Adicione em system/modules as bibliotecas extraídas do seu próprio console."));
    }
    SetWindowTextW(g_banner, text);
}

static int selected_game(void) {
    int item = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (item < 0) return -1;
    LVITEMW it = {0};
    it.mask = LVIF_PARAM;
    it.iItem = item;
    ListView_GetItem(g_list, &it);
    return (int)it.lParam;
}

/* ---- Running a game ---- */

static bool game_running(void) {
    return g_proc.hProcess && WaitForSingleObject(g_proc.hProcess, 0) == WAIT_TIMEOUT;
}

static uint64_t filetime_ticks(FILETIME time) {
    return ((uint64_t)time.dwHighDateTime<<32)|time.dwLowDateTime;
}

static void update_playtime(bool checkpoint,bool ended) {
    if(!*g_play_id)return;
    uint64_t elapsed=GetTickCount64()-g_play_started;
    if(ended) {
        FILETIME created,exited,kernel,user;
        if(GetProcessTimes(g_proc.hProcess,&created,&exited,&kernel,&user)) {
            uint64_t finish=filetime_ticks(exited),start=filetime_ticks(g_play_started_wall);
            if(finish>=start) {
                uint64_t duration=(finish-start)/10000;
                if(duration<elapsed)elapsed=duration;
            }
        }
    }
    if(elapsed>g_play_saved&&(checkpoint||elapsed-g_play_saved>=60000)&&
       report_session_add_playtime(&g_proc_session,elapsed-g_play_saved,&g_play_total))
        g_play_saved=elapsed;
    uint64_t total=current_playtime(elapsed);
    for(int i=0;i<g_count;i++)
        if(!_wcsicmp(g_games[i].id,g_play_id))set_playtime_cell(i,total);
}

static void log_path(const game *g, const wchar_t *suffix, wchar_t *out) {
    swprintf(out, MAX_PATH, L"%ls\\user\\logs\\%ls%ls.log", g_base, g->id, suffix);
}

/* ---- The selected game's music (sce_sys/snd0.at9), looping while it stays selected ---- */

#define WM_MUSIC_READY (WM_APP + 3)
#define MUSIC_MAX_SECONDS 90
static volatile LONG g_music_gen;  /* bumped at every selection: older decodes are dropped */
static uint8_t *g_music_wav;       /* the WAV PlaySound plays (kept until stopped) */

typedef struct music_job {
    LONG gen;
    char path[4 * MAX_PATH];
} music_job;

static void music_stop(void) {
    InterlockedIncrement(&g_music_gen);
    PlaySoundW(NULL, NULL, 0);
    free(g_music_wav);
    g_music_wav = NULL;
}

static DWORD WINAPI music_thread(LPVOID arg) {
    music_job *job = (music_job *)arg;
    uint8_t *data = NULL, *wav = NULL;
    uint64_t bytes = 0;
    pal_media *m = NULL;
    if (pal_file_read_all(job->path, &data, &bytes) == PS5C_OK && bytes <= (32u << 20)) m = pal_media_open_memory(data, bytes);
    free(data);
    pal_media_info info = {0};
    if (m) pal_media_get_info(m, &info);
    if (m && info.channels && info.sample_rate && job->gen == g_music_gen) {
        uint64_t max_frames = (uint64_t)info.sample_rate * MUSIC_MAX_SECONDS, frames = 0;
        uint32_t ch = info.channels;
        wav = (uint8_t *)malloc(44 + max_frames * 2 * ch);
        int n;
        uint64_t t;
        while (wav && frames < max_frames && job->gen == g_music_gen &&
               (n = pal_media_next_audio(m, (int16_t *)(wav + 44 + frames * 2 * ch),
                                         (uint32_t)(max_frames - frames < 4096 ? max_frames - frames : 4096), &t)) > 0)
            frames += (uint64_t)n;
        if (wav && frames) {
            uint32_t datab = (uint32_t)(frames * 2 * ch), riff = 36 + datab, fmt = 16, rate = info.sample_rate, br = rate * ch * 2;
            uint16_t tag = 1, chans = (uint16_t)ch, align = (uint16_t)(ch * 2), bits = 16;
            memcpy(wav, "RIFF", 4), memcpy(wav + 4, &riff, 4), memcpy(wav + 8, "WAVEfmt ", 8), memcpy(wav + 16, &fmt, 4);
            memcpy(wav + 20, &tag, 2), memcpy(wav + 22, &chans, 2), memcpy(wav + 24, &rate, 4), memcpy(wav + 28, &br, 4);
            memcpy(wav + 32, &align, 2), memcpy(wav + 34, &bits, 2), memcpy(wav + 36, "data", 4), memcpy(wav + 40, &datab, 4);
        } else {
            free(wav);
            wav = NULL;
        }
    }
    if (m) pal_media_close(m);
    if (wav && job->gen == g_music_gen) {
        if (!PostMessageW(g_main, WM_MUSIC_READY, (WPARAM)job->gen, (LPARAM)wav)) free(wav);
    } else {
        free(wav);
    }
    free(job);
    return 0;
}

/* A selection: its snd0.at9 starts after the decode ([geral] musica=0 turns it off) */
static void music_select(int gi) {
    music_stop();
    if (gi < 0 || !ini_get_int(g_global, "geral", "musica", 1) || game_running()) return;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\sce_sys\\snd0.at9", g_games[gi].dir);
    if (!file_exists(path)) return;
    music_job *job = (music_job *)calloc(1, sizeof(*job));
    if (!job) return;
    job->gen = g_music_gen;
    wide_to_utf8(path, job->path, sizeof(job->path));
    HANDLE th = CreateThread(NULL, 0, music_thread, job, 0, NULL);
    if (th) CloseHandle(th);
    else free(job);
}

static void music_ready(WPARAM gen, LPARAM wav) {
    if ((LONG)gen != g_music_gen || game_running()) {
        free((void *)wav);
        return;
    }
    PlaySoundW(NULL, NULL, 0);
    free(g_music_wav);
    g_music_wav = (uint8_t *)wav;
    PlaySoundW((LPCWSTR)g_music_wav, NULL, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
}

#include "log_translations.inc"
static const ui_log_message *ui_log_find(const char *format) {
    size_t low=0,high=sizeof(ui_log_messages)/sizeof(ui_log_messages[0]);
    while(low<high) {
        size_t mid=low+(high-low)/2;
        int cmp=strcmp(format,ui_log_messages[mid].source);
        if(cmp<0)high=mid;else if(cmp>0)low=mid+1;else return &ui_log_messages[mid];
    }
    return NULL;
}
static const char *ui_log_translation(const char *format,unsigned locale) {
    const ui_log_message *message=ui_log_find(format);
    return message && locale<UI_LANGUAGE_COUNT?message->text[locale]:NULL;
}
static void report_local_names(char *user, int user_cap, char *pc, int pc_cap);
static bool collect_log(const wchar_t *raw_path, FILE *out, bool sanitize, report_log_quality *quality) {
    *quality = (report_log_quality){0};
    wchar_t path[MAX_PATH];swprintf(path,MAX_PATH,L"%ls.records",raw_path);
    FILE *in=_wfopen(path,L"rb");
    if (!in) { quality->missing = errno == ENOENT; quality->io_error = !quality->missing; }
    swprintf(path,MAX_PATH,L"%ls.records.status",raw_path);
    FILE *status = _wfopen(path, L"rb");
    if (status) {
        quality->status_known = log_record_file_status_read(status, &quality->channel);
        quality->status_invalid = !quality->status_known; fclose(status);
        quality->limited = quality->channel.limited != 0;
        quality->partial = quality->status_known && !quality->channel.finalized;
    } else if (errno != ENOENT) quality->status_invalid = true;
    char text[LOG_RECORD_MAX],wide_text[2048], user[1024] = "", pc[1024] = "";
    if (sanitize) report_local_names(user, sizeof(user), pc, sizeof(pc));
    wchar_t heading[512];
    swprintf(heading,512,ui_string(L"Registro traduzido para %ls. Os códigos, caminhos e valores técnicos foram preservados."),ui_languages[g_ui_locale].name);
    wide_to_utf8(heading,wide_text,sizeof(wide_text));fprintf(out,"%s\n\n",wide_text);
    log_record *r=malloc(sizeof(*r));if(!r){if(in)fclose(in);return false;}
    const wchar_t *levels[]={L"Rastreamento",L"Depuração",L"Informação",L"Aviso",L"Erro",L"Falha fatal"};
    int result=0;
    if(in) {char id[33];if(log_session_header_read(in,id)<0){quality->corrupt=true;fclose(in);in=NULL;}}
    while(in && (result=log_record_read(in,r))==1) {
        quality->read++;
        long output_size=ftell(out);
        if(output_size<0){quality->io_error=true;quality->omitted++;break;}
        if(output_size>(sanitize?4:64)*1024*1024-LOG_RECORD_MAX){quality->limited=true;quality->omitted++;break;}
        const char *translation=ui_log_translation(r->format,g_ui_locale);
        const ui_log_message *message=ui_log_find(r->format);
        for(unsigned arg=0;message && arg<r->count;arg++) {
            if(!(message->value_mask&(UINT64_C(1)<<arg)))continue;
            const char *value=ui_log_translation(r->fields[arg],g_ui_locale);
            if(value && !strchr(value,'%'))r->fields[arg]=value;
        }
        if(translation && log_record_render(r,translation,text,sizeof(text))) {
            quality->rendered++;
            /* Sanitize the entire record, including embedded newlines, before
             * writing anything. Preserve the existing conservative line bound. */
            bool content=false,line_content=false;uint64_t lines=0;
            for(const unsigned char *p=(const unsigned char *)text;*p;p++) {
                if(*p=='\n') {if(line_content)lines++;line_content=false;}
                else if(isalnum(*p) || *p>=0x80) {content=true;line_content=true;}
            }
            if(line_content)lines++;
            if(sanitize && (strlen(text)>7900 || report_private_line(text,user,pc))) {
                quality->filtered++;
                wide_to_utf8(ui_string(L"[linha privada omitida]"),wide_text,sizeof(wide_text));
                fprintf(out,"%s\n",wide_text); continue;
            }
            wide_to_utf8(ui_string(levels[r->level]),wide_text,sizeof(wide_text));
            fprintf(out,"[%6u.%06u] [%s] [%s] %s\n",(unsigned)(r->us/1000000),(unsigned)(r->us%1000000),wide_text,log_cat_name(r->category),text);
            if(content){quality->useful++;quality->useful_lines+=lines;}
        } else {
            quality->omitted++;
            wide_to_utf8(ui_string(L"Registro omitido: mensagem não disponível nesta versão."),wide_text,sizeof(wide_text));
            fprintf(out,"%s\n",wide_text);
        }
    }
    if(result<0) {
        quality->corrupt=result!=-2;
        if(result==-2)quality->limited=true;
        wide_to_utf8(ui_string(L"Registro incompleto ou danificado."),wide_text,sizeof(wide_text));fprintf(out,"%s\n",wide_text);
    }
    quality->empty=in && !quality->read && !quality->corrupt && !quality->limited;
    quality->untranslated=quality->read && !quality->rendered;
    quality->all_filtered=quality->rendered && quality->filtered==quality->rendered;
    if(!quality->read) {
        wide_to_utf8(ui_string(L"Nenhum registro traduzível foi gravado nesta execução. Execute novamente o jogo para gerar o registro."),wide_text,sizeof(wide_text));fprintf(out,"%s\n",wide_text);
    }
    free(r);
    if(in) {if(ferror(in))quality->io_error=true;fclose(in);}
    return !ferror(out);
}
static bool translated_log(const wchar_t *raw_path,const wchar_t *destination) {
    FILE *out=_wfopen(destination,L"wb");if(!out)return false;
    report_log_quality quality;
    bool ok=collect_log(raw_path,out,false,&quality);
    if(fclose(out))ok=false;
    return ok;
}
static LRESULT CALLBACK text_view_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_CLOSE || (msg==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL))) {DestroyWindow(w);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
static void view_text_file(const wchar_t *path,const wchar_t *title) {
    FILE *f=_wfopen(path,L"rb");if(!f)return;
    /* Bound memory for a long session; latest text fits comfortably in the UI. */
    fseek(f,0,SEEK_END);long size=ftell(f);if(size<0){fclose(f);return;}
    long start=size>1024*1024?size-1024*1024:0;fseek(f,start,SEEK_SET);
    char *bytes=calloc((size_t)(size-start)+1,1);
    wchar_t *text=calloc(2*(size_t)(size-start)+2,sizeof(wchar_t));
    if(!bytes || !text){free(bytes);free(text);fclose(f);return;}
    size_t n=fread(bytes,1,(size_t)(size-start),f);bytes[n]=0;fclose(f);
    char *begin=bytes;
    if(start) {char *newline=strchr(begin,'\n');if(newline)begin=newline+1;}
    utf8_to_wide(begin,text,(int)(size-start)+2);
    size_t length=wcslen(text), expanded=length;
    for(size_t i=0;i<length;i++)if(text[i]==L'\n' && (!i || text[i-1]!=L'\r'))expanded++;
    text[expanded]=0;
    for(size_t i=length;i>0;) {
        wchar_t ch=text[--i];text[--expanded]=ch;
        if(ch==L'\n' && (!i || text[i-1]!=L'\r'))text[--expanded]=L'\r';
    }
    HWND w=create_dialog(L"ZeraPS5Text",title,800,580,g_main);
    HWND edit=add_control(w,L"EDIT",L"",WS_TABSTOP|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,12,12,776,516,0);
    SendMessageW(edit,EM_SETLIMITTEXT,1024*1024,0);SetWindowTextW(edit,text);
    add_button(w,ui_string(L"Fechar"),688,542,100,IDOK);
    free(bytes);free(text);run_modal(w,g_main,true);
}
static void view_log(const game *g) {
    if(!report_collection_idle()) {
        MessageBoxW(g_main,ui_string(L"Aguarde o encerramento da execução antes de coletar o relatório."),APP_NAME,MB_ICONINFORMATION);return;
    }
    report_session_snapshot snapshot;
    if(!report_session_snapshot_open(g_base,g->id,&snapshot))return;
    if(snapshot.current_identity_bad||!snapshot.raw[0]){report_session_snapshot_close(&snapshot);return;}
    wchar_t path[MAX_PATH];swprintf(path,MAX_PATH,L"%ls.view.txt",snapshot.raw);
    bool ready=translated_log(snapshot.raw,path);
    if(ready)view_text_file(path,ui_string(L"Registro da execução"));
    DeleteFileW(path);report_session_snapshot_close(&snapshot);
}

/* Build a private environment block instead of changing process-global state. */
static int environment_compare(const void *a,const void *b) {
    return _wcsicmp(*(const wchar_t *const *)a,*(const wchar_t *const *)b);
}
static wchar_t *record_environment_session(const wchar_t *path,const char *id) {
    const wchar_t key[]=L"PS5C_LAUNCHER_RECORDS=",session_key[]=L"PS5C_LAUNCHER_SESSION=";
    wchar_t *env=GetEnvironmentStringsW();
    if(!env)return NULL;
    size_t len=1,count=2;
    for(const wchar_t *p=env;*p;p+=wcslen(p)+1){len+=wcslen(p)+1;
    count++;
    }
    wchar_t record_entry[2*MAX_PATH],session_entry[80];
    swprintf(record_entry,2*MAX_PATH,L"%ls%ls",key,path);
    if(id)swprintf(session_entry,80,L"%ls%hs",session_key,id);
    wchar_t *out=calloc(len+wcslen(record_entry)+82,sizeof(wchar_t));
    const wchar_t **entries=calloc(count,sizeof(*entries));
    if(out&&entries) {
        size_t used=0;
        entries[used++]=record_entry;
        if(id)entries[used++]=session_entry;
        for(const wchar_t *p=env;*p;p+=wcslen(p)+1)
            if(_wcsnicmp(p,key,wcslen(key))&&_wcsnicmp(p,session_key,wcslen(session_key)))entries[used++]=p;
        qsort(entries,used,sizeof(*entries),environment_compare);
        wchar_t *at=out;
        for(size_t n=0;n<used;n++){size_t size=wcslen(entries[n])+1;
        memcpy(at,entries[n],size*sizeof(*at));
        at+=size;
        }
    }else{free(out);
    out=NULL;
    }
    free(entries);
    FreeEnvironmentStringsW(env);
    return out;
}
static bool session_collection_blocked(const game *g) {
    if(g_session_failure_overflow)return true;
    for(unsigned n=0;n<MAX_GAMES;n++)if(!wcscmp(g_session_unavailable[n],g->id))return true;
    return false;
}
static void session_unavailable(const game *g) {
    if(session_collection_blocked(g))return;
    for(unsigned n=0;n<MAX_GAMES;n++)if(!g_session_unavailable[n][0]) {
        wcsncpy(g_session_unavailable[n],g->id,31);g_session_unavailable[n][31]=0;return;
    }
    g_session_failure_overflow=true;
}
static void session_available(const game *g) {
    for(unsigned n=0;n<MAX_GAMES;n++)if(!wcscmp(g_session_unavailable[n],g->id))g_session_unavailable[n][0]=0;
}
static bool session_finish_observed(bool known,DWORD code) {
    if(!g_proc_session.lock||g_proc_session.lock==INVALID_HANDLE_VALUE)return true;
    bool ok=report_session_ended(&g_proc_session,&g_proc_meta,known,code);
    report_session_close(&g_proc_session);
    return ok;
}

static void run_game(int gi) {
    const game *g = &g_games[gi];
    if (g->is_pkg && (g->pkg_retail || g->ps4)) {
        MessageBoxW(g_main, g->pkg_retail
            ? ui_string(L"PKG de loja: os arquivos do jogo são criptografados com as chaves do console. Instale o pacote num PS5 e faça o dump do jogo.")
            : ui_string(L"Rodar direto de PKG de PS4 ainda não está pronto: extraia o pacote numa pasta."),
            APP_NAME, MB_ICONINFORMATION);
        return;
    }
    music_stop();
    if (!report_collection_idle()) {
        MessageBoxW(g_main, ui_string(L"Um jogo já está rodando. Feche-o antes de abrir outro."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if(g_proc_game>=0)poll_game();
    if (g_proc.hProcess) {
        CloseHandle(g_proc.hProcess);
        CloseHandle(g_proc.hThread);
        memset(&g_proc, 0, sizeof(g_proc));
    }
    HANDLE log=INVALID_HANDLE_VALUE;
    if(!report_session_open(g_base,g->id,&g_proc_session)) {
        session_unavailable(g);
        MessageBoxW(g_main,ui_string(L"Não foi possível preparar o registro da execução."),APP_NAME,MB_ICONERROR);
        return;
    }
    char version[81];
    snprintf(version,sizeof(version),"%s+%s%s",PS5C_VERSION_STRING,PS5C_GIT_COMMIT,PS5C_GIT_DIRTY?"-dirty":"");
    if(!report_session_begin(&g_proc_session,version,PS5C_BUILD_NUMBER,&g_proc_meta,&log)) {
        session_unavailable(g);
        report_session_close(&g_proc_session);
        MessageBoxW(g_main,ui_string(L"Não foi possível preparar o registro da execução."),APP_NAME,MB_ICONERROR);
        return;
    }
    wchar_t records[MAX_PATH];
    if(!report_session_path(&g_proc_session,g_proc_meta.id,L"run.log.records",records)) {
        report_session_stage(&g_proc_session,&g_proc_meta,SESSION_UNAVAILABLE);
        session_unavailable(g);CloseHandle(log);report_session_close(&g_proc_session);
        MessageBoxW(g_main,ui_string(L"Não foi possível preparar o registro da execução."),APP_NAME,MB_ICONERROR);return;
    }
    /* ps5c reads the settings files itself (user\config) */
    wchar_t exe[MAX_PATH], cmd[4 * MAX_PATH], modules[MAX_PATH];
    swprintf(exe, MAX_PATH, L"%ls\\ps5c.exe", g_base);
    system_folder(modules);
    /* a fake PS5 package runs in place: its files are read from inside it */
    swprintf(cmd, 4 * MAX_PATH, g->is_pkg ? L"\"%ls\" run \"%ls/@game/eboot.bin\" --modules \"%ls\""
                                          : L"\"%ls\" run \"%ls\\eboot.bin\" --modules \"%ls\"",
             exe, g->dir, modules);
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t *environment=record_environment_session(records,g_proc_meta.id);
    if(!environment) {
        report_session_stage(&g_proc_session,&g_proc_meta,SESSION_UNAVAILABLE);
        session_unavailable(g);
        report_session_close(&g_proc_session);
        CloseHandle(log);
        MessageBoxW(g_main,ui_string(L"Não foi possível preparar o registro da execução."),APP_NAME,MB_ICONERROR);
        return;
    }
    BOOL ok = CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED,environment,g_base,&si,&g_proc);
    DWORD creation_error=ok?0:GetLastError();
    free(environment);
    CloseHandle(log);
    if(!report_session_created(&g_proc_session,&g_proc_meta,ok,creation_error)) {
        if(ok){TerminateProcess(g_proc.hProcess,1);
        WaitForSingleObject(g_proc.hProcess,5000);
        CloseHandle(g_proc.hProcess);
        CloseHandle(g_proc.hThread);
        }
        ok=FALSE;
        session_unavailable(g);
    }
    if (!ok) {
        report_session_close(&g_proc_session);
        memset(&g_proc, 0, sizeof(g_proc));
        MessageBoxW(g_main, ui_string(L"Não foi possível iniciar ps5c.exe (ele deve estar na mesma pasta do launcher)."), APP_NAME,
                    MB_ICONERROR);
        return;
    }
    if(ResumeThread(g_proc.hThread)==(DWORD)-1) {
        DWORD error=GetLastError();
        TerminateProcess(g_proc.hProcess,1);
        WaitForSingleObject(g_proc.hProcess,5000);
        g_proc_meta.creation_error=error;
        report_session_stage(&g_proc_session,&g_proc_meta,SESSION_FAILED);
        report_session_close(&g_proc_session);
        CloseHandle(g_proc.hProcess);
        CloseHandle(g_proc.hThread);
        memset(&g_proc,0,sizeof(g_proc));
        session_unavailable(g);
        return;
    }
    g_play_started=GetTickCount64();
    GetSystemTimeAsFileTime(&g_play_started_wall);
    g_play_saved=0;
    g_play_total=report_session_playtime(g_base,g->id);
    wcscpy(g_play_id,g->id);
    if(!report_session_stage(&g_proc_session,&g_proc_meta,SESSION_RUNNING))session_unavailable(g);
    else session_available(g);
    g_proc_game = gi;
    wchar_t text[400];
    swprintf(text, 400, ui_string(L"Rodando: %ls"), g->title);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)text);
}

/* The system libraries the run could not find ("missing system library: X"
 * lines of its log), as a list for the user; false when there are none. */
static bool missing_libs(const game *g, wchar_t *out, int cap) {
    wchar_t logf[MAX_PATH];
    report_session_snapshot snapshot;
    if(!report_session_snapshot_open(g_base,g->id,&snapshot))return false;
    if(snapshot.current_identity_bad || !snapshot.raw[0]){report_session_snapshot_close(&snapshot);return false;}
    wcsncpy(logf,snapshot.raw,MAX_PATH-1);logf[MAX_PATH-1]=0;
    report_session_snapshot_close(&snapshot);
    HANDLE f = CreateFileW(logf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    static char text[1 << 16];
    DWORD got = 0;
    ReadFile(f, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(f);
    text[got] = 0;
    out[0] = 0;
    int len = 0;
    for (const char *p = text; (p = strstr(p, "missing system library: ")) != NULL;) {
        p += 24;
        const char *end = strchr(p, ' ');
        if (!end) break;
        wchar_t name[128];
        char n8[128];
        snprintf(n8, sizeof(n8), "%.*s", (int)(end - p), p);
        utf8_to_wide(n8, name, 128);
        len += swprintf(out + len, (size_t)(cap - len), L"    %ls\n", name);
        if (len >= cap - 140) break;
    }
    return out[0] != 0;
}

/* ---- Automatic report when a game closes ([relatorios] enviar_ao_fechar) ---- */

#define WM_AUTO_SENT (WM_APP + 2)

typedef struct auto_upload {
    wchar_t zip[MAX_PATH];
    wchar_t id[32];
} auto_upload;

typedef enum report_mode { REPORT_DRAFT, REPORT_MANUAL, REPORT_AUTOMATIC } report_mode;
static bool build_report(const game *g, const wchar_t *what, report_mode mode, wchar_t *zip, report_log_quality *built_quality);
static bool report_upload(const wchar_t *zip, const wchar_t *game_id, bool automatic, wchar_t *reply, int cap);
static const char *update_source(void);
static bool is_url(const wchar_t *s);

static DWORD WINAPI auto_upload_thread(void *param) {
    auto_upload *u = (auto_upload *)param;
    wchar_t reply[200] = L"";
    bool ok = report_upload(u->zip, u->id, true, reply, 200);
    PostMessageW(g_main, WM_AUTO_SENT, ok, 0);
    free(u);
    return 0;
}

/* The log of the run that just ended goes to the analysis queue, in the background */
static void auto_report(int gi, DWORD code) {
    if (!ini_get_int(g_global, "relatorios", "enviar_ao_fechar", 0)) return;
    wchar_t src[2 * MAX_PATH], logf[MAX_PATH], what[160];
    utf8_to_wide(report_source(), src, 2 * MAX_PATH);
    log_path(&g_games[gi], L"", logf);
    if (!is_url(src)) return;
    swprintf(what, 160, ui_string(L"(enviado automaticamente ao fechar o jogo; código de saída 0x%lX)"), (unsigned long)code);
    auto_upload *u = (auto_upload *)calloc(1, sizeof(auto_upload));
    if (!u) return;
    wcsncpy(u->id, g_games[gi].id, 31);
    HANDLE t = build_report(&g_games[gi], what, REPORT_AUTOMATIC, u->zip, NULL) ? CreateThread(NULL, 0, auto_upload_thread, u, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    else free(u);
}

/* Timer: tells when the game ends and with what exit code */
static void poll_game(void) {
    if(!g_proc.hProcess||g_proc_game<0)return;
    DWORD wait=WaitForSingleObject(g_proc.hProcess,0);
    if(wait==WAIT_TIMEOUT) {
        update_playtime(false,false);
        return;
    }
    if(wait!=WAIT_OBJECT_0)return;
    update_playtime(true,true);
    g_play_id[0]=0;
    DWORD code = 0;
    bool code_known=GetExitCodeProcess(g_proc.hProcess,&code)!=0;
    if(!session_finish_observed(code_known,code))session_unavailable(&g_games[g_proc_game]);
    wchar_t libs[1024];
    if (code_known && code == 3 && missing_libs(&g_games[g_proc_game], libs, 1024)) {
        wchar_t sys[MAX_PATH], msg[2048];
        system_folder(sys);
        swprintf(msg, 2048,
                 ui_string(L"%ls precisa destas bibliotecas de sistema do console, que não estão na pasta do sistema:\n\n%ls\n"
                 L"Copie-as do seu console (ou do seu outro PC) para:\n%ls\n\nVeja system\\LEIA-ME.txt."),
                 g_games[g_proc_game].title, libs, sys);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)ui_string(L"Faltam bibliotecas de sistema do console."));
        g_proc_game = -1;
        MessageBoxW(g_main, msg, APP_NAME, MB_ICONWARNING);
        return;
    }
    wchar_t text[400];
    if(!code_known)
        swprintf(text,400,L"%ls",ui_string(L"A execução terminou; código de saída desconhecido."));
    else if (code == 0)
        swprintf(text, 400, ui_string(L"%ls fechou normalmente."), g_games[g_proc_game].title);
    else
        swprintf(text, 400, ui_string(L"%ls terminou com erro (código 0x%lX). Use \"Reportar problema\"."), g_games[g_proc_game].title,
                 (unsigned long)code);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)text);
    if(code_known)auto_report(g_proc_game, code);
    g_proc_game = -1;
}

/* ---- Problem report: user\reports\<id>-<date>.zip with the log and system info ----
 * With an HTTP update address the report can go straight to that server
 * (POST /relatorios), where it waits in the analysis queue. */

static const char *update_source(void);
static bool is_url(const wchar_t *s);
static bool report_upload(const wchar_t *zip, const wchar_t *game_id, bool automatic, wchar_t *reply, int cap);

enum { ID_REPORT_SAVE = 600 };
static wchar_t g_rep_text[4000];
static int g_rep_choice; /* -1 cancelled, 0 save only, 1 send */
static HWND g_rep_edit;
static bool report_description_useful(const wchar_t *what);
/* -2 keeps the dialog and text open for correction; drafts never imply send. */
static int report_dialog_choice(unsigned command, const wchar_t *text) {
    if(command==ID_REPORT_SAVE)return 0;
    if(command==IDOK)return report_description_useful(text)?1:-2;
    return -1;
}

static LRESULT CALLBACK report_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == ID_REPORT_SAVE)) {
        GetWindowTextW(g_rep_edit, g_rep_text, 4000);
        int selection=report_dialog_choice(LOWORD(wp),g_rep_text);
        if (selection==-2) {
            MessageBoxW(w, ui_string(L"Descreva o sintoma e os passos com pelo menos 12 caracteres visíveis após o filtro de privacidade."), APP_NAME, MB_ICONWARNING);
            SetFocus(g_rep_edit); return 0;
        }
        g_rep_choice = selection;
        DestroyWindow(w);
        return 0;
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
        g_rep_choice = -1;
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* What happened, in the tester's words; false when cancelled */
static bool ask_report(const game *g, bool can_send) {
    wchar_t title[320];
    swprintf(title, 320, ui_string(L"Reportar problema: %ls"), g->title);
    g_rep_text[0] = 0;
    g_rep_choice = -1;
    HWND w = create_dialog(L"ZeraPS5Report", title, 480, 250, g_main);
    add_control(w, L"STATIC", ui_string(L"O que aconteceu? (o que você fez, onde parou, o que apareceu na tela)"), 0, 16, 12, 450, 20, 0);
    g_rep_edit = add_control(w, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 16, 36,
                             448, 150, 0);
    if (can_send) add_button(w, ui_string(L"Enviar para análise"), 16, 204, 160, IDOK);
    add_button(w, ui_string(L"Só salvar o arquivo"), can_send ? 186 : 16, 204, 150, ID_REPORT_SAVE);
    add_button(w, ui_string(L"Cancelar"), 374, 204, 90, IDCANCEL);
    SetFocus(g_rep_edit);
    run_modal(w, g_main, false);
    return g_rep_choice >= 0;
}

static void reg_string(HKEY root, const wchar_t *key, const wchar_t *name, wchar_t *out, DWORD cap) {
    DWORD size = cap * sizeof(wchar_t);
    out[0] = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS) out[0] = 0;
}

/* Local names are used only to filter report text, never as upload headers. */
static void report_local_names(char *user, int user_cap, char *pc, int pc_cap) {
    wchar_t name[256] = L"";
    GetEnvironmentVariableW(L"USERNAME", name, 256);
    wide_to_utf8(name, user, user_cap);
    DWORD length = 256;
    if (!GetComputerNameW(name, &length)) name[0] = 0;
    wide_to_utf8(name, pc, pc_cap);
}

/* Validate the exact surviving description, never the unfiltered input. */
static bool report_description_space(wchar_t ch) {
    WORD type=0;GetStringTypeW(CT_CTYPE1,&ch,1,&type);
    return (type&(C1_SPACE|C1_CNTRL)) || ch==0x00ad || ch==0x061c || (ch>=0x200b && ch<=0x200f) ||
           ch==0xfeff || ch==0x2060 ||
           (ch>=0x202a && ch<=0x202e) || (ch>=0x2066 && ch<=0x2069);
}
static bool report_description_useful(const wchar_t *what) {
    if(!what)return false;
    char user[1024], pc[1024], utf8[16000];
    wchar_t visible[4000]; size_t used=0;
    report_local_names(user,sizeof(user),pc,sizeof(pc));
    while(*what) {
        size_t length=wcscspn(what,L"\r\n"), take=length<3999?length:3999;
        wchar_t line[4000];wmemcpy(line,what,take);line[take]=0;
        wide_to_utf8(line,utf8,sizeof(utf8));
        if(length<4000 && !report_private_line(utf8,user,pc))
            for(size_t n=0;n<take && used<3999;n++)if(!report_description_space(line[n]))
                visible[used++]=towlower(line[n]);
        what+=length;if(*what==L'\r')what++;if(*what==L'\n')what++;
    }
    visible[used]=0;
    for(unsigned locale=0;locale<UI_LANGUAGE_COUNT;locale++) {
        const wchar_t *placeholder=ui_translate(locale,L"(escreva aqui)");
        size_t matched=0;bool equal=true;
        for(;*placeholder;placeholder++)if(!report_description_space(*placeholder)) {
            if(matched>=used || CompareStringOrdinal(&visible[matched],1,placeholder,1,TRUE)!=CSTR_EQUAL){equal=false;break;}
            matched++;
        }
        if(equal && matched==used)return false;
    }
    size_t letters=0;
    for(size_t n=0;n<used;n++) {
        WORD type=0;GetStringTypeW(CT_CTYPE1,&visible[n],1,&type);
        if(type&(C1_ALPHA|C1_DIGIT))letters++;
    }
    return used>=12 && letters>=4;
}
static void report_write_description(FILE *file, const wchar_t *what) {
    char user[1024], pc[1024], utf8[16000];
    wchar_t line[4000];
    report_local_names(user, sizeof(user), pc, sizeof(pc));
    while (*what) {
        size_t length = wcscspn(what, L"\r\n");
        size_t take = length < 3999 ? length : 3999;
        wmemcpy(line, what, take); line[take] = 0;
        wide_to_utf8(line, utf8, sizeof(utf8));
        fwprintf(file, L"%ls\n", length>=4000 || report_private_line(utf8, user, pc) ? ui_string(L"[linha privada omitida]") : line);
        what += length;
        if (*what == L'\r') what++;
        if (*what == L'\n') what++;
    }
}
typedef struct report_package_io {
    FILE *(*open)(const wchar_t *,const wchar_t *);
    int (*close)(FILE *);
    bool (*archive)(const wchar_t *,report_archive_entry *,unsigned);
    bool (*verify)(const wchar_t *,const report_archive_entry *,unsigned);
} report_package_io;
static report_package_io g_report_package_io={_wfopen,fclose,report_archive_write,report_archive_verify};
static bool report_collection_idle(void) {
    return !g_proc.hProcess || WaitForSingleObject(g_proc.hProcess,0)==WAIT_OBJECT_0;
}
static bool report_copy_log(const wchar_t *from, const wchar_t *to, report_log_quality *quality) {
    FILE *out=g_report_package_io.open(to,L"wb");
    if(!out)return false;
    bool ok=collect_log(from,out,true,quality);
    if(g_report_package_io.close(out))ok=false;
    return ok;
}
static bool report_copy_session_log(const wchar_t *from,const wchar_t *to,report_log_quality *q,bool bad,bool absent) {
    if(!bad&&!absent)return report_copy_log(from,to,q);
    *q=(report_log_quality){.missing=absent,.corrupt=bad,.identity_mismatch=bad};
    FILE *out=g_report_package_io.open(to,L"wb");
    if(!out)return false;
    char notice[1024];
    wide_to_utf8(ui_string(bad?L"Registro incompleto ou danificado.":L"Nenhum registro traduzível foi gravado nesta execução. Execute novamente o jogo para gerar o registro."),notice,sizeof(notice));
    fprintf(out,"%s\n",notice);
    bool ok=!ferror(out);
    if(g_report_package_io.close(out))ok=false;
    return ok;
}
static bool report_send_allowed(report_mode mode, bool description, const report_log_quality *quality) {
    if(mode==REPORT_DRAFT)return false;
    if(mode==REPORT_AUTOMATIC)return report_log_complete(quality)&&!quality->session_unverified&&!quality->identity_mismatch;
    return mode==REPORT_MANUAL && description;
}
static bool report_quality_write(FILE *out, report_mode mode, bool description, const report_log_quality *q,
                                 const report_log_quality *previous) {
    const char *classification=mode==REPORT_DRAFT?"draft_incomplete":
        (!q->useful?"diagnostics_unavailable":(report_log_complete(q)&&!q->session_unverified?"complete":"diagnostics_partial"));
    fprintf(out,"schema=2\nclassification=%s\nmode=%s\nsendable=%u\ndescription_useful=%u\n",
            classification, mode==REPORT_AUTOMATIC?"automatic":(mode==REPORT_MANUAL?"manual":"draft"),
            report_send_allowed(mode,description,q),description);
    fprintf(out,"current_state=%s\nrecords_read=%llu\nrecords_rendered=%llu\nrecords_omitted=%llu\nrecords_filtered=%llu\nuseful_records=%llu\nuseful_lines=%llu\n",
            report_log_state(q),(unsigned long long)q->read,(unsigned long long)q->rendered,(unsigned long long)q->omitted,
            (unsigned long long)q->filtered,(unsigned long long)q->useful,(unsigned long long)q->useful_lines);
    fprintf(out,"missing=%u\nempty=%u\ndamaged=%u\nsize_limited=%u\nnot_rendered=%u\nfully_filtered=%u\nio_error=%u\nstatus_known=%u\nstatus_invalid=%u\npartial=%u\n",
            q->missing,q->empty,q->corrupt,q->limited,q->untranslated,q->all_filtered,q->io_error,q->status_known,q->status_invalid,q->partial);
    fprintf(out,"open_error=%llu\nwrite_error=%llu\nflush_error=%llu\nsetup_error=%llu\nserialization_rejected=%llu\ncontention_dropped=%llu\nfinalized=%llu\n",
            (unsigned long long)q->channel.open_error,(unsigned long long)q->channel.write_error,(unsigned long long)q->channel.flush_error,
            (unsigned long long)q->channel.setup_error,(unsigned long long)q->channel.rejected,(unsigned long long)q->channel.busy,
            (unsigned long long)q->channel.finalized);
    fprintf(out,"previous_state=%s\nprevious_records_read=%llu\nprevious_records_rendered=%llu\nprevious_records_omitted=%llu\nprevious_records_filtered=%llu\nprevious_useful_records=%llu\nprevious_useful_lines=%llu\n",
            report_log_state(previous),(unsigned long long)previous->read,(unsigned long long)previous->rendered,
            (unsigned long long)previous->omitted,(unsigned long long)previous->filtered,
            (unsigned long long)previous->useful,(unsigned long long)previous->useful_lines);
    fprintf(out,"previous_missing=%u\nprevious_empty=%u\nprevious_damaged=%u\nprevious_size_limited=%u\nprevious_not_rendered=%u\nprevious_fully_filtered=%u\nprevious_io_error=%u\nprevious_status_known=%u\nprevious_status_invalid=%u\nprevious_partial=%u\n",
            previous->missing,previous->empty,previous->corrupt,previous->limited,previous->untranslated,
            previous->all_filtered,previous->io_error,previous->status_known,previous->status_invalid,previous->partial);
    fprintf(out,"session_unverified=%u\nidentity_mismatch=%u\nend=1\n",q->session_unverified,q->identity_mismatch);
    return !ferror(out);
}

static bool report_session_info_write(FILE *out,const report_session_snapshot *snap) {
    fprintf(out,"schema=1\nidentity=%s\ncurrent_identity_mismatch=%u\nprevious_identity_mismatch=%u\n",
            snap->legacy?"legacy_unverified":"managed",snap->current_identity_bad,snap->previous_identity_bad);
    for(unsigned n=0;n<2;n++) {
        const char *prefix=n?"previous":"current";
        const log_session_meta *m=n?&snap->previous:&snap->current,*core=n?&snap->previous_core:&snap->core;
        bool known=n?snap->previous_known:snap->current_known,core_known=n?snap->previous_core_known:snap->core_known;
        fprintf(out,"%s_session_id=%s\n%s_launcher_known=%u\n%s_launcher_version=%s\n%s_launcher_build=",
                prefix,known?m->id:"unknown",prefix,known,prefix,known?m->version:"unknown",prefix);
        if(known)fprintf(out,"%u\n",m->build);
        else fputs("unknown\n",out);
        fprintf(out,"%s_launcher_stage=",prefix);
        if(known)fprintf(out,"%u\n",m->stage);
        else fputs("unknown\n",out);
        fprintf(out,"%s_process_creation=%s\n%s_creation_error=",prefix,!known||!m->creation_known?"unknown":(m->creation_ok?"ok":"failed"),prefix);
        if(known&&m->creation_known&&!m->creation_ok)fprintf(out,"%u\n",m->creation_error);
        else fputs("unknown\n",out);
        fprintf(out,"%s_process_exit=",prefix);
        if(known&&m->exit_known)fprintf(out,"%lu\n",(unsigned long)m->exit_code);
        else fputs("unknown\n",out);
        fprintf(out,"%s_core_known=%u\n%s_core_version=%s\n%s_core_build=",prefix,core_known,prefix,core_known?core->version:"unknown",prefix);
        if(core_known)fprintf(out,"%u\n",core->build);
        else fputs("unknown\n",out);
        fprintf(out,"%s_core_stage=",prefix);
        if(core_known)fprintf(out,"%u\n",core->stage);
        else fputs("unknown\n",out);
        fprintf(out,"%s_guest_exit=",prefix);
        if(core_known&&core->guest_known)fprintf(out,"%lld\n",(long long)core->guest_code);
        else fputs("unknown\n",out);
    }
    fputs("end=1\n",out);
    return !ferror(out);
}
static bool report_file_matches(FILE *expected,const wchar_t *path) {
    if(fflush(expected)||fseek(expected,0,SEEK_SET))return false;
    FILE *actual=_wfopen(path,L"rb");
    if(!actual)return false;
    unsigned char a[4096],b[4096];
    size_t n,m;
    bool ok=true;
    do {
        n=fread(a,1,sizeof(a),expected);
        m=fread(b,1,sizeof(b),actual);
        if(n!=m||memcmp(a,b,n)){ok=false;
        break;
        }
    }while(n);
    if(ferror(actual)||ferror(expected))ok=false;
    fclose(actual);
    return ok;
}
static bool report_file_private(const wchar_t *path) {
    FILE *f=_wfopen(path,L"rb");
    if(!f)return true;
    char user[1024],pc[1024],line[20000];
    report_local_names(user,sizeof(user),pc,sizeof(pc));
    bool bad=false;
    while(fgets(line,sizeof(line),f)) {
        if((!strchr(line,'\n')&&!feof(f))||report_private_line(line,user,pc)){bad=true;
        break;
        }
    }
    if(ferror(f))bad=true;
    fclose(f);
    return bad;
}

/* The report zip (user\\reports\\<id>-<date>[-auto].zip): the log, the previous
 * run's log and info.txt with the system and `what` happened */
static bool build_report(const game *g, const wchar_t *what, report_mode mode, wchar_t *zip, report_log_quality *built_quality) {
    if(mode!=REPORT_DRAFT&&mode!=REPORT_MANUAL&&mode!=REPORT_AUTOMATIC)return false;
    if(!report_collection_idle()||session_collection_blocked(g))return false;
    bool automatic=mode==REPORT_AUTOMATIC,description=report_description_useful(what);
    if(mode==REPORT_MANUAL&&!description)return false;
    if(!what)what=L"";
    wchar_t game_key[48];if(!report_session_game_key(g->id,game_key))return false;
    report_session_snapshot snapshot;
    if(!report_session_snapshot_open(g_base,g->id,&snapshot))return false;
    bool ok=false,reserved=false,tmp_owned=false;
    zip[0]=0;
    wchar_t tmp[MAX_PATH]=L"",partial[MAX_PATH]=L"",reports[MAX_PATH],dst[MAX_PATH]=L"",prev_dst[MAX_PATH]=L"";
    wchar_t quality_path[MAX_PATH]=L"",session_path[MAX_PATH]=L"",info_path[MAX_PATH]=L"";
    report_log_quality quality={0},previous={.missing=true};
    if(swprintf(reports,MAX_PATH,L"%ls\\user\\reports",g_base)<0)goto cleanup;
    if(!CreateDirectoryW(reports,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)goto cleanup;
    if(!GetTempFileNameW(reports,L"zqr",0,tmp))goto cleanup;
    tmp_owned=true;
    if(!DeleteFileW(tmp)||!CreateDirectoryW(tmp,NULL))goto cleanup;
    if(swprintf(dst,MAX_PATH,L"%ls\\%ls.log",tmp,game_key)<0 ||
       swprintf(prev_dst,MAX_PATH,L"%ls\\%ls.anterior.log",tmp,game_key)<0 ||
       swprintf(quality_path,MAX_PATH,L"%ls\\report-quality.txt",tmp)<0 ||
       swprintf(session_path,MAX_PATH,L"%ls\\session-info.txt",tmp)<0 ||
       swprintf(info_path,MAX_PATH,L"%ls\\info.txt",tmp)<0) {
        dst[0]=prev_dst[0]=quality_path[0]=session_path[0]=info_path[0]=0;goto cleanup;
    }
    if(!report_copy_session_log(snapshot.raw,dst,&quality,snapshot.current_identity_bad,!snapshot.legacy&&!snapshot.current_known))goto cleanup;
    quality.session_unverified=snapshot.legacy||!snapshot.current_known;
    if(mode!=REPORT_DRAFT&&!report_send_allowed(mode,description,&quality))goto cleanup;
    bool has_previous=snapshot.previous_known;
    if(snapshot.legacy) {
        wchar_t records[MAX_PATH];
        swprintf(records,MAX_PATH,L"%ls.records",snapshot.previous_raw);
        has_previous=file_exists(snapshot.previous_raw)||file_exists(records);
    }
    if(has_previous&&!report_copy_session_log(snapshot.previous_raw,prev_dst,&previous,snapshot.previous_identity_bad,false))goto cleanup;
    FILE *meta=g_report_package_io.open(quality_path,L"wb");
    if(!meta)goto cleanup;
    bool meta_ok=report_quality_write(meta,mode,description,&quality,&previous);
    if(g_report_package_io.close(meta))meta_ok=false;
    if(!meta_ok)goto cleanup;
    meta=g_report_package_io.open(session_path,L"wb");
    if(!meta)goto cleanup;
    meta_ok=report_session_info_write(meta,&snapshot);
    if(g_report_package_io.close(meta))meta_ok=false;
    if(!meta_ok)goto cleanup;
    /* Hardware models only. Paths and account names are omitted. */
    wchar_t cpu[128], os[128], build[32], gpus[512] = L"";
    reg_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", cpu, 128);
    reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", os, 128);
    reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", build, 32);
    /* every GPU of the PC: the one that drives the main display is not
     * necessarily the one the emulator renders on (that one is the setting
     * below, and the log's "Vulkan device:" line) */
    DISPLAY_DEVICEW dd = {0};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if (!dd.DeviceString[0] || wcsstr(gpus, dd.DeviceString)) continue;
        size_t used = wcslen(gpus);
        swprintf(gpus + used, 512 - used, L"%ls%ls", used ? L"; " : L"", dd.DeviceString);
    }
    wchar_t placa[256];
    utf8_to_wide(ini_get(g_global, "video", "placa") ? ini_get(g_global, "video", "placa") : "auto", placa, 256);
    MEMORYSTATUSEX mem = {0};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    FILE *f = g_report_package_io.open(info_path, L"w, ccs=UTF-8");
    if (!f)goto cleanup;
    {
        fwprintf(f, L"ZeraPS5 %hs+%hs%ls (%hs)\n", PS5C_VERSION_STRING, PS5C_GIT_COMMIT, PS5C_GIT_DIRTY ? L"-dirty" : L"",
                 PS5C_BUILD_DATE);
        fwprintf(f, ui_string(L"Jogo: %ls\nID: %ls\nVersão: %ls\n"), g->title, g->id, g->version);
        wchar_t gpath[MAX_PATH];
        game_config_path(g, gpath);
        ini *gdoc = ini_read_file(gpath);
        fwprintf(f, ui_string(L"Idioma: %d\nResolução: %d\n"),
                 (int)ini_get_int(gdoc, "geral", "idioma", ini_get_int(g_global, "geral", "idioma", USER_LANGUAGE_DEFAULT)),
                 (int)ini_get_int(gdoc, "geral", "resolucao",
                                  ini_get_int(g_global, "geral", "resolucao", USER_RESOLUTION_DEFAULT)));
        ini_free(gdoc);
        fwprintf(f, ui_string(L"CPU: %ls\nGPU configurada: %ls\nGPUs do PC: %ls\nRAM: %llu MB\nWindows: %ls (build %ls)\n"), cpu,
                 placa, gpus[0] ? gpus : L"?",
                 (unsigned long long)(mem.ullTotalPhys >> 20), os, build);
        fwprintf(f, ui_string(L"\nO que aconteceu:\n%ls\n"), L"");
        report_write_description(f, what);
        bool info_ok=!ferror(f);
        if(g_report_package_io.close(f))info_ok=false;
        if(!info_ok)goto cleanup;
    }
    /* Read back mandatory metadata and privacy-check every member before finalizing. */
    FILE *expected=tmpfile();
    if(!expected)goto cleanup;
    meta_ok=report_quality_write(expected,mode,description,&quality,&previous)&&report_file_matches(expected,quality_path);
    if(fclose(expected))meta_ok=false;
    if(!meta_ok)goto cleanup;
    expected=tmpfile();
    if(!expected)goto cleanup;
    meta_ok=report_session_info_write(expected,&snapshot)&&report_file_matches(expected,session_path);
    if(fclose(expected))meta_ok=false;
    if(!meta_ok)goto cleanup;
    report_archive_entry entries[REPORT_ARCHIVE_FILES]={0};
    unsigned count=0;
    const wchar_t *paths[]={info_path,dst,quality_path,session_path,prev_dst};
    for(unsigned n=0;n<(has_previous?5u:4u);n++) {
        if(report_file_private(paths[n]))goto cleanup;
        const wchar_t *name=wcsrchr(paths[n],L'\\');
        if(!name)goto cleanup;
        wide_to_utf8(name+1,entries[count].name,sizeof(entries[count].name));
        wcsncpy(entries[count].path,paths[n],MAX_PATH-1);
        count++;
    }
    if(!report_archive_prepare(entries,count))goto cleanup;
    uint64_t archive_bytes=22;
    for(unsigned n=0;n<count;n++) {
        if(!entries[n].size)goto cleanup;
        archive_bytes+=entries[n].size+76+2*strlen(entries[n].name);
    }
    if(archive_bytes>10u*1024u*1024u)goto cleanup;
    SYSTEMTIME st;
    GetLocalTime(&st);
    for(unsigned attempt=0;attempt<1000;attempt++) {
        if(swprintf(zip,MAX_PATH,L"%ls\\%ls-%04u%02u%02u-%02u%02u%02u-%03u%ls.zip",reports,game_key,
                    st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,attempt,automatic?L"-auto":L"")<0||
           swprintf(partial,MAX_PATH,L"%ls.partial",zip)<0)goto cleanup;
        if(file_exists(zip))continue;
        HANDLE h=CreateFileW(partial,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);
        reserved=true;
        break;
        }
        if(GetLastError()!=ERROR_FILE_EXISTS)goto cleanup;
    }
    report_archive_entry frozen[REPORT_ARCHIVE_FILES];
    memcpy(frozen,entries,sizeof(entries));
    if(!reserved||!g_report_package_io.archive(partial,entries,count))goto cleanup;
    for(unsigned n=0;n<count;n++) {
        if(strcmp(entries[n].name,frozen[n].name)||wcscmp(entries[n].path,frozen[n].path)||entries[n].size!=frozen[n].size||entries[n].crc!=frozen[n].crc)goto cleanup;
        frozen[n].offset=entries[n].offset;
    }
    if(!g_report_package_io.verify(partial,frozen,count))goto cleanup;
    /* No replace: a prior valid package can never be overwritten. */
    if(!MoveFileExW(partial,zip,MOVEFILE_WRITE_THROUGH))goto cleanup;
    reserved=false;
    ok=true;
cleanup:
    if(reserved)DeleteFileW(partial);
    if(*quality_path)DeleteFileW(quality_path);
    if(*session_path)DeleteFileW(session_path);
    if(*info_path)DeleteFileW(info_path);
    if(*dst)DeleteFileW(dst);
    if(*prev_dst)DeleteFileW(prev_dst);
    if(tmp_owned) {
        DWORD attr=GetFileAttributesW(tmp);
        if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY))RemoveDirectoryW(tmp);
        else DeleteFileW(tmp);
    }
    report_session_snapshot_close(&snapshot);
    if(ok&&built_quality)*built_quality=quality;
    if(!ok)zip[0]=0;
    return ok;
}

static void report(int gi) {
    const game *g = &g_games[gi];
    if(!report_collection_idle()) {
        MessageBoxW(g_main,ui_string(L"Aguarde o encerramento da execução antes de coletar o relatório."),APP_NAME,MB_ICONINFORMATION);return;
    }
    if(g_proc_game>=0)poll_game();
    wchar_t srcw[2 * MAX_PATH];
    utf8_to_wide(report_source(), srcw, 2 * MAX_PATH);
    bool can_send = is_url(srcw);
    if (!ask_report(g, can_send)) return;
    report_mode mode=g_rep_choice==1?REPORT_MANUAL:REPORT_DRAFT;
    wchar_t zip[MAX_PATH];report_log_quality quality;
    bool ok = build_report(g,g_rep_text,mode,zip,&quality);
    if (!ok) {
        MessageBoxW(g_main, ui_string(L"Não foi possível criar o arquivo do relatório."), APP_NAME, MB_ICONERROR);
        return;
    }
    if(mode==REPORT_MANUAL && (!report_log_complete(&quality)||quality.session_unverified||quality.identity_mismatch) && MessageBoxW(g_main,
        ui_string(L"O diagnóstico está ausente ou incompleto. O relatório será identificado assim. Enviar com a descrição informada?"),
        APP_NAME,MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES)return;
    if (g_rep_choice == 1) {
        wchar_t reply[200] = L"";
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        bool sent = report_upload(zip, g->id, false, reply, 200);
        SetCursor(old);
        if (sent) {
            wchar_t msg[400];
            swprintf(msg, 400, ui_string(L"Relatório enviado para análise.\n\nCódigo: %ls\n(uma cópia ficou em user\\reports)"), reply);
            MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION);
            return;
        }
        MessageBoxW(g_main, ui_string(L"Não foi possível enviar ao servidor (ele está ligado?). O arquivo foi salvo: envie-o de outro jeito."),
                    APP_NAME, MB_ICONWARNING);
    }
    wchar_t args[MAX_PATH + 16];
    swprintf(args, MAX_PATH + 16, L"/select,\"%ls\"", zip);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    MessageBoxW(g_main,
                mode==REPORT_DRAFT?ui_string(L"Rascunho incompleto salvo localmente. Nada foi enviado."):
                ui_string(L"Relatório criado (a pasta foi aberta com o arquivo selecionado).\n\n"
                L"Envie o .zip junto com uma descrição curta: o que você fez e o que aconteceu."),
                APP_NAME, MB_ICONINFORMATION);
}

/* ---- Updates ----
 * The builds come from the address built in (PS5C_UPDATE_URL: a LAN server
 * today, the internet later) or from [atualizacao] fonte when the settings
 * name another (an HTTP(S) address or a folder such as \\PC\ZeraPS5): it holds
 * latest.json, the manifest tools\publish.ps1 writes, and the zips.
 * A newer build (a higher build number) is offered at start or on request;
 * installing downloads the zip, checks its SHA-256, extracts it and replaces
 * the emulator's own files only (renaming the ones in use), then restarts.
 * user\, games and system libraries are never touched: the package has none. */

#define WM_UPDATE_CHECKED (WM_APP + 1)

typedef struct update_info {
    int build;
    int schema;
    char channel[32], platform[32];
    char full[128], zip[260], sha[80];
    char notes[2048];
    char notes_localized[15][2048];
} update_info;

static update_info g_update;                /* the newer build found */
static wchar_t g_update_src[2 * MAX_PATH]; /* where it was found */
static volatile LONG g_update_busy;

static void update_visit(void *user, const char *path, const char *value) {
    update_info *u = (update_info *)user;
    if (!strcmp(path, "build")) u->build = atoi(value);
    else if (!strcmp(path, "schema")) u->schema = atoi(value);
    else if (!strcmp(path, "channel")) snprintf(u->channel,sizeof(u->channel),"%s",value);
    else if (!strcmp(path, "platform")) snprintf(u->platform,sizeof(u->platform),"%s",value);
    else if (!strcmp(path, "full_version")) snprintf(u->full, sizeof(u->full), "%s", value);
    else if (!strcmp(path, "zip")) snprintf(u->zip, sizeof(u->zip), "%s", value);
    else if (!strcmp(path, "zip_sha256")) snprintf(u->sha, sizeof(u->sha), "%s", value);
    else if (!strcmp(path, "notes")) snprintf(u->notes, sizeof(u->notes), "%s", value);
    else if (!strncmp(path,"notes_localized.",16)) {
        const char *tag=path+16;
        for(unsigned n=0;n<UI_LANGUAGE_COUNT;n++)if(!strcmp(tag,ui_languages[n].tag))
            snprintf(u->notes_localized[n],sizeof(u->notes_localized[n]),"%s",value);
    }
}

static bool is_url(const wchar_t *s) { return !_wcsnicmp(s, L"http://", 7) || !_wcsnicmp(s, L"https://", 8); }

/* GET url into the file `dst` (WinHTTP: no cache, so a new latest.json is always seen) */
static bool http_download(const wchar_t *url, const wchar_t *dst) {
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen(L"ZeraPS5-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 5000, 5000, 10000, 30000);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, NULL);
    DWORD status = 0, len = sizeof(status);
    ok = ok && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                   &status, &len, WINHTTP_NO_HEADER_INDEX) &&
         status == 200;
    wchar_t length_text[32]; DWORD length_size = sizeof(length_text);
    bool declared_length = ok && WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                                    length_text, &length_size, WINHTTP_NO_HEADER_INDEX);
    uint64_t expected_length = declared_length ? _wcstoui64(length_text, NULL, 10) : 0;
    HANDLE f = ok ? CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    ok = f != INVALID_HANDLE_VALUE;
    uint8_t buf[1 << 16];
    DWORD got = 0, put; uint64_t total = 0;
    while (ok) {
        if (!WinHttpReadData(r, buf, sizeof(buf), &got)) { ok=false; break; }
        if (!got) break;
        total += got;
        if (total > (512u << 20)) { ok=false; break; }
        ok = WriteFile(f, buf, got, &put, NULL) && put == got;
    }
    if (ok && declared_length && total != expected_length) ok = false;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) DeleteFileW(dst);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

/* POST the file `path` to url; the answer's first line into reply */
static bool http_post_file(const wchar_t *url, const wchar_t *path, const wchar_t *headers, wchar_t *reply, int cap) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    uint8_t *data = size && size <= (10u << 20) ? (uint8_t *)malloc(size) : NULL;
    bool ok = data && ReadFile(f, data, size, &got, NULL) && got == size;
    CloseHandle(f);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], upath[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath;
    uc.dwUrlPathLength = 2048;
    ok = ok && WinHttpCrackUrl(url, 0, 0, &uc);
    HINTERNET s = ok ? WinHttpOpen(L"ZeraPS5-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0)
                     : NULL;
    if (s) WinHttpSetTimeouts(s, 5000, 5000, 60000, 60000);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    ok = r && WinHttpSendRequest(r, headers, (DWORD)-1L, data, size, size, 0) && WinHttpReceiveResponse(r, NULL);
    DWORD status = 0, len = sizeof(status);
    ok = ok && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                   &status, &len, WINHTTP_NO_HEADER_INDEX) &&
         status == 200;
    char text[512] = "";
    DWORD n = 0;
    if (ok && WinHttpReadData(r, text, sizeof(text) - 1, &n)) text[n] = 0;
    text[strcspn(text, "\r\n")] = 0;
    if (reply) utf8_to_wide(text, reply, cap);
    free(data);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static bool report_upload(const wchar_t *zip, const wchar_t *game_id, bool automatic, wchar_t *reply, int cap) {
    wchar_t src[2 * MAX_PATH], url[3 * MAX_PATH], headers[600];
    utf8_to_wide(report_source(), src, 2 * MAX_PATH);
    if (!is_url(src)) return false;
    swprintf(url, 3 * MAX_PATH, L"%ls%lsrelatorios", src, src[wcslen(src) - 1] == L'/' ? L"" : L"/");
    swprintf(headers, 600,
             L"Content-Type: application/zip\r\nX-ZeraPS5-Game: %ls\r\nX-ZeraPS5-Build: %d\r\nX-ZeraPS5-Auto: %d\r\n",
             game_id, PS5C_BUILD_NUMBER, automatic ? 1 : 0);
    return http_post_file(url, zip, headers, reply, cap);
}

/* The file `name` of the update source into `dst` */
static bool fetch_update_file(const wchar_t *src, const wchar_t *name, const wchar_t *dst) {
    wchar_t from[3 * MAX_PATH];
    if (is_url(src)) {
        swprintf(from, 3 * MAX_PATH, L"%ls%ls%ls", src, src[wcslen(src) - 1] == L'/' ? L"" : L"/", name);
        return http_download(from, dst);
    }
    swprintf(from, 3 * MAX_PATH, L"%ls\\%ls", src, name);
    return CopyFileW(from, dst, FALSE) != FALSE;
}

static bool read_update_manifest(const wchar_t *src, update_info *out) {
    memset(out, 0, sizeof(*out));
    wchar_t path[MAX_PATH];
    CreateDirectoryW(L"user", NULL);
    CreateDirectoryW(L"user\\updates", NULL);
    swprintf(path, MAX_PATH, L"%ls\\user\\updates\\latest.json", g_base);
    wchar_t name[256] = L"latest.json";
    if (!fetch_update_file(src, name, path)) return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    char *data = size && size < (8u << 20) ? (char *)malloc(size) : NULL;
    bool ok = data && ReadFile(f, data, size, &got, NULL) && got == size;
    CloseHandle(f);
    if (ok) {
        jp j = {data, data + size, 0, update_visit, out};
        if (size >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) j.p += 3;
        char path_buf[256] = "";
        ok = jp_value(&j, path_buf, sizeof(path_buf));
    }
    free(data);
    /* the zip is a plain file name next to the manifest */
    return ok && out->schema == 1 && !strcmp(out->channel,"stable") && !strcmp(out->platform,"windows-x86_64") &&
           out->build > 0 && out->zip[0] && strlen(out->sha) == 64 && strspn(out->sha, "0123456789abcdefABCDEF") == 64 &&
           strspn(out->zip, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == strlen(out->zip) &&
           !strncmp(out->zip, "ZeraPS5-", 8) && strlen(out->zip) > 12 && !strcmp(out->zip + strlen(out->zip) - 4, ".zip");
}

typedef struct update_check {
    wchar_t src[2 * MAX_PATH];
    bool manual;
} update_check;

/* Worker thread: a server or share may take seconds to answer */
static DWORD WINAPI update_check_thread(void *param) {
    update_check *c = (update_check *)param;
    update_info info;
    WPARAM result = 0; /* 0 unreadable, 1 up to date, 2 newer found */
    if (read_update_manifest(c->src, &info)) {
        result = info.build > PS5C_BUILD_NUMBER ? 2 : 1;
        if (result == 2) {
            g_update = info;
            wcscpy(g_update_src, c->src);
        }
    }
    PostMessageW(g_main, WM_UPDATE_CHECKED, result, c->manual);
    free(c);
    InterlockedExchange(&g_update_busy, 0);
    return 0;
}

static const char *report_source(void) {
    const char *v = ini_get(g_global, "relatorios", "servidor");
    return v ? v : "https://zeraps5-reports.zeraps5.workers.dev";
}

/* Where updates come from: the built-in address (PS5C_UPDATE_URL at build
 * time), unless [atualizacao] fonte names another (edited by hand) */
static const char *update_source(void) {
    const char *v = ini_get(g_global, "atualizacao", "fonte");
    return v && *v ? v : PS5C_UPDATE_URL;
}

/* Starts a check of `source` (NULL: update_source()) */
static void check_updates(const char *source, bool manual) {
    if (!source) source = update_source();
    if (!*source) return;
    if (InterlockedExchange(&g_update_busy, 1)) return;
    update_check *c = (update_check *)calloc(1, sizeof(update_check));
    if (!c) {
        InterlockedExchange(&g_update_busy, 0);
        return;
    }
    wchar_t w[2 * MAX_PATH];
    utf8_to_wide(source, w, 2 * MAX_PATH);
    if (is_url(w)) wcscpy(c->src, w);
    else full_path(source, c->src);
    c->manual = manual;
    HANDLE t = CreateThread(NULL, 0, update_check_thread, c, 0, NULL);
    if (t) {
        if(g_update_button)EnableWindow(g_update_button,FALSE);
        CloseHandle(t);
    } else {
        free(c);
        InterlockedExchange(&g_update_busy, 0);
    }
}

static bool sha256_file(const wchar_t *path, char *hex /* 65 */) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return false;
    bool ok = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0;
    HANDLE f = ok ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    ok = f != INVALID_HANDLE_VALUE;
    uint8_t buf[1 << 16];
    DWORD got;
    while (ok) {
        if (!ReadFile(f, buf, sizeof(buf), &got, NULL)) { ok=false; break; }
        if (!got) break;
        ok = BCryptHashData(hash, buf, got, 0) >= 0;
    }
    uint8_t digest[32];
    ok = ok && BCryptFinishHash(hash, digest, 32, 0) >= 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; ok && i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    return ok;
}

/* 0: nothing to recover, 1: worker started (exit this launcher), -1: failure. */
static int recover_update(void) {
    FILE *f=_wfopen(L"user\\updates\\transaction\\journal.json",L"rb");
    if(!f) return 0;
    char state[2048]; size_t n=fread(state,1,sizeof(state)-1,f); fclose(f); state[n]=0;
    if(!strstr(state,"\"applying\"")) return 0;
    wchar_t cmd[4*MAX_PATH];
    swprintf(cmd,4*MAX_PATH,L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%ls\\user\\updates\\Recuperar-ZeraPS5.ps1\" -InstallRoot \"%ls\" -WaitPid %lu -Recover -Restart",g_base,g_base,(unsigned long)GetCurrentProcessId());
    STARTUPINFOW si={0}; si.cb=sizeof(si); PROCESS_INFORMATION pi={0};
    if(!CreateProcessW(NULL,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,g_base,&si,&pi)) return -1;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return 1;
}

static void install_update(void) {
    if (game_running()) {
        MessageBoxW(g_main,ui_string(L"Feche o jogo antes de atualizar."),APP_NAME,MB_ICONINFORMATION); return;
    }
    CreateDirectoryW(L"user",NULL); CreateDirectoryW(L"user\\updates",NULL);
    wchar_t zipw[260],zip[MAX_PATH],script[MAX_PATH],cmd[6*MAX_PATH];
    utf8_to_wide(g_update.zip,zipw,260);
    swprintf(zip,MAX_PATH,L"%ls\\user\\updates\\%ls",g_base,zipw);
    swprintf(script,MAX_PATH,L"%ls\\user\\updates\\Recuperar-ZeraPS5.ps1",g_base);
    HCURSOR prev=SetCursor(LoadCursor(NULL,IDC_WAIT)); char sha[65]="";
    const wchar_t *error=NULL;
    SendMessageW(g_status,SB_SETTEXTW,0,(LPARAM)ui_string(L"Baixando e verificando a atualização..."));
    if(!fetch_update_file(g_update_src,zipw,zip)) error=ui_string(L"Não foi possível baixar a atualização.");
    else if(!sha256_file(zip,sha) || _stricmp(sha,g_update.sha)) error=ui_string(L"A atualização não passou na verificação SHA-256.");
    else if(!CopyFileW(L"tools\\apply-update.ps1",script,FALSE)) error=ui_string(L"Não foi possível preparar a recuperação da atualização.");
    SetCursor(prev);
    if(!error) {
        swprintf(cmd,6*MAX_PATH,L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%ls\" -InstallRoot \"%ls\" -Package \"%ls\" -ExpectedSha256 %hs -ExpectedBuild %d -WaitPid %lu -Restart",script,g_base,zip,sha,g_update.build,(unsigned long)GetCurrentProcessId());
        STARTUPINFOW si={0}; si.cb=sizeof(si); PROCESS_INFORMATION pi={0};
        if(CreateProcessW(NULL,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,g_base,&si,&pi)) {
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread); DestroyWindow(g_main); return;
        }
        error=ui_string(L"Não foi possível iniciar a instalação. A versão atual foi preservada.");
    }
    DeleteFileW(zip);
    SendMessageW(g_status,SB_SETTEXTW,0,(LPARAM)ui_string(L"Atualização não instalada."));
    MessageBoxW(g_main,error,APP_NAME,MB_ICONERROR);
}

/* The result of a check (main thread) */
static void update_checked(WPARAM result, LPARAM manual) {
    if(g_update_button)EnableWindow(g_update_button,TRUE);
    if (result == 2) {
        wchar_t full[128], notes[2048], msg[3000], status[200];
        utf8_to_wide(g_update.full, full, 128);
        if(g_update.notes_localized[g_ui_locale][0])
            utf8_to_wide(g_update.notes_localized[g_ui_locale],notes,2048);
        else swprintf(notes,2048,L"%ls",ui_string(L"Versão experimental. Consulte LEIA-ME e limitações conhecidas."));
        swprintf(status, 200, ui_string(L"Atualização disponível: build %d"), g_update.build);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)status);
        swprintf(msg, 3000, ui_string(L"Nova versão disponível: %ls.\nVocê está na build %d.\n\n%ls\n\nAtualizar agora?"), full,
                 PS5C_BUILD_NUMBER, notes);
        if (MessageBoxW(g_main, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) install_update();
    } else if (manual && result == 1) {
        MessageBoxW(g_main, ui_string(L"Você já está na versão mais nova."), APP_NAME, MB_ICONINFORMATION);
    } else if (manual) {
        MessageBoxW(g_main,
                    ui_string(L"Nenhuma release acessível no momento.\nO repositório pode estar privado, vazio ou sem conexão. A versão instalada foi mantida."),
                    APP_NAME, MB_ICONWARNING);
    }
}

/* ---- Main window ---- */

/* Show the effective bindings, including a selected game's file overrides.
 * Missing entries inherit defaults; an explicitly empty binding stays empty. */
static const char *keyboard_binding(const ini *overrides, const char *name) {
    const char *value = ini_get(overrides, "teclado", name);
    if (!value) value = ini_get(g_global, "teclado", name);
    if (!value) {
        for (uint32_t i = 0; i < user_control_count; i++)
            if (!strcmp(user_controls[i].key, name)) return user_controls[i].keys;
        return "";
    }
    return value;
}

static void keyboard_key_text(const ini *overrides, const char *name, wchar_t *out, int cap) {
    const char *value = keyboard_binding(overrides, name);
    if (*value) utf8_to_wide(value, out, cap);
    else swprintf(out, cap, L"%ls", ui_string(L"(nenhuma)"));
}

static void keyboard_group_text(const ini *overrides, const char *const names[4], wchar_t *out, int cap) {
    wchar_t values[4][64];
    for (int i = 0; i < 4; i++) keyboard_key_text(overrides, names[i], values[i], 64);
    swprintf(out, cap, L"%ls / %ls / %ls / %ls", values[0], values[1], values[2], values[3]);
}

static void keyboard_summary(const ini *overrides, wchar_t *out, int cap) {
    const char *const movement[] = {"esq_cima", "esq_esquerda", "esq_baixo", "esq_direita"};
    const char *const camera[] = {"dir_cima", "dir_esquerda", "dir_baixo", "dir_direita"};
    const char *const arrows[] = {"cima", "esquerda", "baixo", "direita"};
    wchar_t move[280], look[280], dpad[280], cross[64], circle[64], options[64], touch[64];
    keyboard_group_text(overrides, movement, move, 280);
    keyboard_group_text(overrides, camera, look, 280);
    keyboard_group_text(overrides, arrows, dpad, 280);
    keyboard_key_text(overrides, "cruz", cross, 64);
    keyboard_key_text(overrides, "circulo", circle, 64);
    keyboard_key_text(overrides, "options", options, 64);
    keyboard_key_text(overrides, "touchpad", touch, 64);
    swprintf(out, cap, ui_string(L"Teclado — mover: %ls | câmera: %ls | direcional: %ls\r\nX: %ls | O: %ls | Options: %ls | Touchpad: %ls"),
             move, look, dpad, cross, circle, options, touch);
}

static void keyboard_hint_refresh(void) {
    if (!g_keyboard_hint) return;
    ini *overrides = NULL;
    int gi = selected_game();
    if (gi >= 0) {
        wchar_t path[MAX_PATH];
        game_config_path(&g_games[gi], path);
        overrides = ini_read_file(path);
    }
    wchar_t text[2048];
    keyboard_summary(overrides, text, 2048);
    SetWindowTextW(g_keyboard_hint, text);
    ini_free(overrides);
}

static void layout(void) {
    RECT r;
    GetClientRect(g_main, &r);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int status_h = sr.bottom - sr.top, pad = S(8), bh = S(38), banner_h = S(36);
    int columns=r.right<S(1100)?3:6,rows=6/columns;
    int bottom = r.bottom - status_h - pad - rows*bh-(rows-1)*pad;
    int update_width=S(210), keys_width=S(210), hint_h=S(64);
    MoveWindow(g_banner, pad, pad, r.right - 4 * pad-update_width-keys_width, banner_h, TRUE);
    MoveWindow(g_keyboard_button,r.right-2*pad-update_width-keys_width,pad,keys_width,S(30),TRUE);
    MoveWindow(g_update_button,r.right-pad-update_width,pad,update_width,S(30),TRUE);
    MoveWindow(g_list, pad, pad + banner_h + S(4), r.right - 2 * pad, bottom - pad - banner_h - S(4) - pad - hint_h, TRUE);
    MoveWindow(g_keyboard_hint, pad, bottom-hint_h, r.right-2*pad, hint_h-pad, TRUE);
    int button_width=(r.right-(columns+1)*pad)/columns;
    for(int i=0;i<6;i++)MoveWindow(g_buttons[i],pad+(i%columns)*(button_width+pad),bottom+(i/columns)*(bh+pad),button_width,bh,TRUE);
    /* status bar: messages on the left, the version always on the right */
    int parts[2] = {r.right - S(300) > S(100) ? r.right - S(300) : S(100), -1};
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);
}

static void ui_apply_main(void) {
    if(!g_main)return;
    ui_apply_control(g_main,0);
    EnumChildWindows(g_main,ui_apply_control,0);
    for(int i=0;i<6;i++)SetWindowTextW(g_buttons[i],ui_string(k_button_text[i]));
    SetWindowTextW(g_update_button,ui_string(L"Verificar atualizações"));
    SetWindowTextW(g_keyboard_button,ui_string(L"Controles do teclado..."));
    for(int n=0;n<(int)PS5C_ARRAY_COUNT(k_game_columns);n++) {
        LVCOLUMNW column={0};column.mask=LVCF_TEXT;column.pszText=(wchar_t *)ui_string(k_game_columns[n]);
        ListView_SetColumn(g_list,n,&column);
    }
    HMENU bar=CreateMenu(),languages=CreatePopupMenu();
    AppendMenuW(languages,MF_STRING,ID_UI_LANGUAGES,ui_string(L"Idiomas..."));
    AppendMenuW(languages,MF_SEPARATOR,0,NULL);
    for(unsigned n=0;n<UI_LANGUAGE_COUNT;n++)
        AppendMenuW(languages,MF_STRING|(n==g_ui_locale?MF_CHECKED:0),ID_UI_LANG_FIRST+n,ui_languages[n].name);
    HMENU old=GetMenu(g_main);
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)languages,ui_string(L"Idiomas"));
    AppendMenuW(bar,MF_STRING,ID_UI_LANGUAGES+1,ui_string(L"Ajuda técnica"));
    AppendMenuW(bar,MF_STRING,ID_PATCHES,ui_string(L"Patches"));
    SetMenu(g_main,bar);if(old)DestroyMenu(old);DrawMenuBar(g_main);
    wchar_t version[128];
    swprintf(version,128,ui_string(L"Versão %hs build %d (%hs%ls)"),PS5C_VERSION_STRING,PS5C_BUILD_NUMBER,PS5C_GIT_COMMIT,PS5C_GIT_DIRTY?L"-dirty":L"");
    SendMessageW(g_status,SB_SETTEXTW,1,(LPARAM)version);
    layout();fill_list();keyboard_hint_refresh();
    if(!game_running())SendMessageW(g_status,SB_SETTEXTW,0,(LPARAM)ui_string(g_system_result.copy_failed?
        L"Bibliotecas encontradas; a cópia local falhou. Usando a pasta original.":L"Pronto"));
}

/* user\mods\<title id>: one subfolder a mod, laid over the game's files like an update
 * (a name starting with '_' switches it off); created and opened in Explorer */
static void open_mods(const game *g) {
    for (const wchar_t *q = g->id; *q; q++)
        if (!iswalnum(*q) && *q != L'_' && *q != L'-') {
            MessageBoxW(g_main, ui_string(L"Este jogo não tem um title ID válido para mods."), APP_NAME, MB_ICONINFORMATION);
            return;
        }
    wchar_t dir[MAX_PATH];
    CreateDirectoryW(L"user", NULL);
    CreateDirectoryW(L"user\\mods", NULL);
    swprintf(dir, MAX_PATH, L"%ls\\user\\mods\\%ls", g_base, g->id);
    CreateDirectoryW(dir, NULL);
    ShellExecuteW(NULL, L"open", L"explorer.exe", dir, NULL, SW_SHOWNORMAL);
    SendMessageW(g_status, SB_SETTEXTW, 0,
                 (LPARAM)ui_string(L"Mods: uma subpasta por mod, com os arquivos no mesmo caminho do jogo. Nome começando com _ desativa."));
}

/* A desktop shortcut to ZeraPS5.exe --jogar "<game folder>", with the launcher's icon */
static void create_shortcut(const game *g) {
    static const GUID clsid_shell_link = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    static const GUID iid_shell_link = {0x000214F9, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    static const GUID iid_persist_file = {0x0000010B, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    wchar_t exe[MAX_PATH], desktop[MAX_PATH], args[MAX_PATH + 16], name[200], link[MAX_PATH + 220];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop))) desktop[0] = 0;
    size_t n = 0;
    for (const wchar_t *q = g->title[0] ? g->title : g->id; *q && n + 1 < 120; q++)
        name[n++] = wcschr(L"\\/:*?\"<>|", *q) || *q < 32 ? L' ' : *q;
    name[n] = 0;
    swprintf(args, MAX_PATH + 16, L"--jogar \"%ls\"", g->dir);
    swprintf(link, MAX_PATH + 220, L"%ls\\%ls.lnk", desktop, name);
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    bool ok = desktop[0] && SUCCEEDED(CoCreateInstance(&clsid_shell_link, NULL, CLSCTX_INPROC_SERVER, &iid_shell_link, (void **)&sl));
    if (ok) {
        sl->lpVtbl->SetPath(sl, exe);
        sl->lpVtbl->SetArguments(sl, args);
        sl->lpVtbl->SetWorkingDirectory(sl, g_base);
        sl->lpVtbl->SetIconLocation(sl, exe, 0);
        sl->lpVtbl->SetDescription(sl, g->title);
        ok = SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &iid_persist_file, (void **)&pf)) &&
             SUCCEEDED(pf->lpVtbl->Save(pf, link, TRUE));
        if (pf) pf->lpVtbl->Release(pf);
        sl->lpVtbl->Release(sl);
    }
    SendMessageW(g_status, SB_SETTEXTW, 0,
                 (LPARAM)(ok ? ui_string(L"Atalho criado na área de trabalho.") : ui_string(L"Não foi possível criar o atalho.")));
}

static void list_menu(void) {
    int gi = selected_game();
    if (gi < 0) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_PLAY, ui_string(L"Jogar"));
    AppendMenuW(m, MF_STRING, ID_CONFIG, ui_string(L"Configurar jogo"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_MODS, ui_string(L"Pasta de mods"));
    AppendMenuW(m, MF_STRING, ID_PATCHES, ui_string(L"Patches de compatibilidade..."));
    AppendMenuW(m, MF_STRING, ID_SHORTCUT, ui_string(L"Criar atalho na área de trabalho"));
    POINT pt;
    GetCursorPos(&pt);
    int id = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (id == ID_MODS) open_mods(&g_games[gi]);
    else if (id == ID_SHORTCUT) create_shortcut(&g_games[gi]);
    else if (id) on_command(id);
}

/* --jogar: select the shortcut's game and start it */
static void autoplay(void) {
    if (!g_autoplay[0]) return;
    int count = ListView_GetItemCount(g_list);
    for (int item = 0; item < count; item++) {
        LVITEMW it = {0};
        it.mask = LVIF_PARAM;
        it.iItem = item;
        ListView_GetItem(g_list, &it);
        if ((int)it.lParam < 0 || (int)it.lParam >= g_count || _wcsicmp(g_games[it.lParam].dir, g_autoplay)) continue;
        ListView_SetItemState(g_list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, item, FALSE);
        g_autoplay[0] = 0;
        PostMessageW(g_main, WM_COMMAND, ID_PLAY, 0);
        return;
    }
    wchar_t msg[MAX_PATH + 100];
    swprintf(msg, MAX_PATH + 100, ui_string(L"O jogo do atalho não está na lista:\n%ls"), g_autoplay);
    g_autoplay[0] = 0;
    MessageBoxW(g_main, msg, APP_NAME, MB_ICONWARNING);
}

static void on_command(int id) {
    if (id == ID_MAIN_KEYS) {
        configure_keys(g_main);
        keyboard_hint_refresh();
        return;
    }
    if(id==ID_UI_LANGUAGES+1) {
        wchar_t path[MAX_PATH];swprintf(path,MAX_PATH,L"%ls\\docs\\HELP-%hs.txt",g_base,ui_languages[g_ui_locale].tag);
        if(file_exists(path))view_text_file(path,ui_string(L"Ajuda técnica"));
        else MessageBoxW(g_main,L"Não foi possível preparar a ajuda.",APP_NAME,MB_ICONERROR);
        return;
    }
    int gi = selected_game();
    if(id==ID_CHECK_UPDATES) {
        check_updates(NULL,true);
        return;
    }
    if (id == ID_SETTINGS) {
        configure_general();
        keyboard_hint_refresh();
        return;
    }
    if (id == ID_REFRESH) {
        load_global(); /* the settings file may have been edited by hand */
        ui_apply_main();
        return;
    }
    if (gi < 0) {
        MessageBoxW(g_main, ui_string(L"Selecione um jogo na lista."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (id == ID_PLAY) run_game(gi);
    else if (id == ID_PATCHES) configure_patches(gi, g_main);
    else if (id == ID_CONFIG) configure_game(gi);
    else if (id == ID_REPORT) report(gi);
    else if (id == ID_LOG) view_log(&g_games[gi]);
}

static LRESULT CALLBACK main_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = w;
        g_banner = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, w, NULL, NULL, NULL);
        g_update_button=CreateWindowExW(0,L"BUTTON",ui_string(L"Verificar atualizações"),
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,w,(HMENU)(INT_PTR)ID_CHECK_UPDATES,NULL,NULL);
        g_keyboard_button=CreateWindowExW(0,L"BUTTON",ui_string(L"Controles do teclado..."),
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,w,(HMENU)(INT_PTR)ID_MAIN_KEYS,NULL,NULL);
        g_keyboard_hint=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,
            0,0,0,0,w,NULL,NULL,NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0,
                                 0, 0, w, (HMENU)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetImageList(g_list, ImageList_Create(S(ICON_SIZE), S(ICON_SIZE), ILC_COLOR32, 16, 16), LVSIL_SMALL);
        for (int i = 0; i < (int)PS5C_ARRAY_COUNT(k_game_columns); i++) {
            LVCOLUMNW c = {0};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (wchar_t *)ui_string(k_game_columns[i]);
            c.cx = S(k_game_widths[i]);
            ListView_InsertColumn(g_list, i, &c);
        }
        for (int i = 0; i < 6; i++) {
            g_buttons[i] = CreateWindowExW(0, L"BUTTON", ui_string(k_button_text[i]), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE | (i ? 0 : BS_DEFPUSHBUTTON),
                                           0, 0, 0, 0, w, (HMENU)(INT_PTR)(ID_PLAY + i), NULL, NULL);
        }
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, w, NULL, NULL, NULL);
        HWND all[] = {g_banner,     g_list,       g_status,     g_update_button, g_keyboard_button, g_keyboard_hint, g_buttons[0], g_buttons[1],
                      g_buttons[2], g_buttons[3], g_buttons[4], g_buttons[5]};
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) SendMessageW(all[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        wchar_t ver[128];
        swprintf(ver, 128, ui_string(L"Versão %hs build %d (%hs%ls)"), PS5C_VERSION_STRING, PS5C_BUILD_NUMBER, PS5C_GIT_COMMIT,
                 PS5C_GIT_DIRTY ? L"-dirty" : L"");
        SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)ver);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)ui_string(L"Pronto"));
        ui_apply_main();
        fill_list();
        autoplay();
        SetTimer(w, ID_TIMER, 1000, NULL);
        if (ini_get_int(g_global, "atualizacao", "ao_iniciar", 1))
            check_updates(NULL, false); /* in the background: a newer build is offered */
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(720);
        mm->ptMinTrackSize.y = S(400);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==ID_UI_LANGUAGES){configure_languages(g_main);return 0;}
        if(LOWORD(wp)==ID_UI_LANGUAGES+1){on_command(ID_UI_LANGUAGES+1);return 0;}
        if(LOWORD(wp)>=ID_UI_LANG_FIRST && LOWORD(wp)<ID_UI_LANG_FIRST+UI_LANGUAGE_COUNT) {
            if(ui_save_preference(LOWORD(wp)-ID_UI_LANG_FIRST))ui_apply_main();
            else MessageBoxW(g_main,L"Falha ao salvar configurações.",APP_NAME,MB_ICONERROR);
            return 0;
        }
        if (LOWORD(wp) >= ID_PLAY && LOWORD(wp) <= ID_SETTINGS) on_command(LOWORD(wp));
        if (LOWORD(wp) == ID_CHECK_UPDATES) on_command(ID_CHECK_UPDATES);
        if (LOWORD(wp) == ID_MAIN_KEYS) on_command(ID_MAIN_KEYS);
        if (LOWORD(wp) == ID_PATCHES) on_command(ID_PATCHES);
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == ID_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if (lv->uChanged & LVIF_STATE) keyboard_hint_refresh();
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
                music_select(selected_game());
        }
        if (n->idFrom == ID_LIST && n->code == NM_RCLICK) list_menu();
        if (n->idFrom == ID_LIST && (n->code == NM_DBLCLK || n->code == LVN_KEYDOWN)) {
            if (n->code == NM_DBLCLK || ((NMLVKEYDOWN *)lp)->wVKey == VK_RETURN) on_command(ID_PLAY);
            else if (((NMLVKEYDOWN *)lp)->wVKey == VK_F5) on_command(ID_REFRESH);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) SetFocus(g_list);
        return 0;
    case WM_TIMER: poll_game(); return 0;
    case WM_UPDATE_CHECKED: update_checked(wp, lp); return 0;
    case WM_MUSIC_READY: music_ready(wp, lp); return 0;
    case WM_AUTO_SENT:
        SendMessageW(g_status, SB_SETTEXTW, 0,
                     (LPARAM)(wp ? ui_string(L"Log da partida enviado para análise.") : ui_string(L"Não foi possível enviar o log (o servidor está ligado?).")));
        return 0;
    case WM_DESTROY:
        update_playtime(true,!game_running());
        music_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    if (pal_init() != PS5C_OK) return 1;
    (void)prev;
    (void)cmdline;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--jogar")) swprintf(g_autoplay, MAX_PATH, L"%ls", argv[i + 1]);
    if (argv) LocalFree(argv);
    /* everything is relative to the launcher's folder */
    GetModuleFileNameW(NULL, g_base, MAX_PATH);
    wchar_t *slash = wcsrchr(g_base, L'\\');
    if (slash) *slash = 0;
    SetCurrentDirectoryW(g_base);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* the folder picker */
    load_global();
    int recovery = recover_update();
    if(recovery) {
        if(recovery<0) MessageBoxW(NULL,ui_string(L"A recuperação não iniciou. Consulte user\\updates\\result.txt."),APP_NAME,MB_ICONERROR);
        return recovery<0?1:0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ULONG_PTR gdip;
    gdip_input gi = {1, NULL, FALSE, FALSE};
    GdiplusStartup(&gdip, &gi, NULL);

    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    NONCLIENTMETRICSW ncm = {0};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ZeraPS5Main";
    RegisterClassExW(&wc);
    static const struct {
        WNDPROC proc;
        const wchar_t *name;
    } k_dialogs[] = {{game_config_proc, L"ZeraPS5GameConfig"},
                     {patches_proc, L"ZeraPS5Patches"},
                     {settings_proc, L"ZeraPS5Settings"},
                     {keys_proc, L"ZeraPS5Keys"},
                     {text_view_proc, L"ZeraPS5Text"},
                     {pad_proc, L"ZeraPS5Pad"},
                     {accessories_proc, L"ZeraPS5Accessories"},
                     {capture_proc, L"ZeraPS5Capture"},
                     {report_proc, L"ZeraPS5Report"},
                     {languages_proc,L"ZeraPS5Languages"}};
    for (size_t i = 0; i < sizeof(k_dialogs) / sizeof(k_dialogs[0]); i++) {
        wc.lpfnWndProc = k_dialogs[i].proc;
        wc.lpszClassName = k_dialogs[i].name;
        RegisterClassExW(&wc);
    }

    wchar_t title[128];
    swprintf(title, 128, L"%ls %hs build %d - PlayStation 5", APP_NAME, PS5C_VERSION_STRING, PS5C_BUILD_NUMBER);
    HWND w = CreateWindowExW(ui_layout_style(), L"ZeraPS5Main", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, S(960), S(620), NULL, NULL, inst, NULL);
    ShowWindow(w, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    pal_shutdown();
    return 0;
}
