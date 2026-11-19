/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int WINAPI launcher_unused_entry(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show);
#define wWinMain launcher_unused_entry
#include "../../frontends/launcher/launcher.c"
#undef wWinMain

static void write_fixture(const wchar_t *path, const char *text) {
    FILE *f = _wfopen(path, L"wb");
    assert(f && fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
}

/* Exercise the real save handler with an invisible list control and a
 * disposable profile, including an unchecked and an unsupported patch. */
static void save_preference(bool checked, bool supported) {
    HWND owner = CreateWindowExW(0, L"STATIC", L"patch test", 0, 0, 0, 1, 1,
                                  NULL, NULL, GetModuleHandleW(NULL), NULL);
    assert(owner);
    g_patch_list = CreateWindowExW(0, WC_LISTVIEWW, L"", LVS_REPORT, 0, 0, 1, 1,
                                     owner, NULL, GetModuleHandleW(NULL), NULL);
    assert(g_patch_list);
    ListView_SetExtendedListViewStyle(g_patch_list, LVS_EX_CHECKBOXES);
    LVITEMW row = {0}; row.mask = LVIF_TEXT; row.pszText = L"test";
    assert(ListView_InsertItem(g_patch_list, &row) == 0);
    ListView_SetCheckState(g_patch_list, 0, checked);
    g_patch_game = 0;
    g_patch_count = 1;
    g_patch_rows[0] = compat_patch_at(0);
    g_patch_ready[0] = supported;
    patches_proc(owner, WM_COMMAND, IDOK, 0);
    assert(!IsWindow(owner));
}

int wmain(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv) {
    assert(argc == 2 && SetCurrentDirectoryW(argv[1]));
    CreateDirectoryW(L"game", NULL);
    CreateDirectoryW(L"game\\sce_sys", NULL);
    CreateDirectoryW(L"user", NULL);
    CreateDirectoryW(L"user\\config", NULL);
    CreateDirectoryW(L"user\\patch", NULL);
    CreateDirectoryW(L"user\\patch\\PPSA01880", NULL);
    CreateDirectoryW(L"user\\patch\\PPSA01880\\sce_sys", NULL);
    const wchar_t *update_param = L"user\\patch\\PPSA01880\\sce_sys\\param.json";
    DeleteFileW(update_param);
    write_fixture(L"game\\sce_sys\\param.json", "{\"titleId\":\"PPSA01880\",\"contentVersion\":\"01.006.000\"}");
    write_fixture(L"game\\eboot.bin", "base");
    write_fixture(L"user\\patch\\PPSA01880\\eboot.bin", "update");
    game g = {0};
    wcscpy(g.dir, L"game");
    char file[1024];
    param_info param;
    assert(patch_content(&g, file, sizeof(file), &param));
    assert(!strcmp(param.version, "01.006.000") && !strcmp(file, "game/eboot.bin"));

    write_fixture(update_param, "{\"titleId\":\"PPSA01880\",\"contentVersion\":\"01.009.000\"}");
    assert(patch_content(&g, file, sizeof(file), &param));
    assert(!strcmp(param.version, "01.009.000"));
    assert(!strcmp(file, "user/patch/PPSA01880/eboot.bin"));
    assert(compat_patch_check(compat_patch_at(0), param.id, param.version, NULL, 0) == COMPAT_PATCH_WRONG_VERSION);

    /* Valid titleId lets the overlay resolver find this update, but an invalid
     * complete param.json must not fall back to the old, supported version. */
    write_fixture(update_param, "{\"titleId\":\"PPSA01880\",\"contentVersion\":");
    assert(!patch_content(&g, file, sizeof(file), &param));

    /* Data-only updates still select their content version even when the base
     * eboot runs; this is deliberately stricter than a binary hash alone. */
    write_fixture(update_param, "{\"titleId\":\"PPSA01880\",\"contentVersion\":\"01.009.000\"}");
    assert(DeleteFileW(L"user\\patch\\PPSA01880\\eboot.bin"));
    assert(patch_content(&g, file, sizeof(file), &param));
    assert(!strcmp(param.version, "01.009.000") && !strcmp(file, "game/eboot.bin"));
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
    assert(InitCommonControlsEx(&controls));
    assert(GetCurrentDirectoryW(MAX_PATH, g_base));
    wcscpy(g_games[0].id, L"PPSA01880");
    write_fixture(L"user\\config\\PPSA01880.ini",
                  "[geral]\nresolucao=1080\n[patches]\nquake1_direct_ip_host=1\n");
    save_preference(false, true);
    ini *doc = ini_read_file(L"user\\config\\PPSA01880.ini");
    assert(ini_get_int(doc, "patches", "quake1_direct_ip_host", -1) == 0);
    assert(ini_get_int(doc, "geral", "resolucao", 0) == 1080);
    ini_free(doc);
    save_preference(true, true);
    save_preference(false, false); /* an unknown update retains the user's preference */
    doc = ini_read_file(L"user\\config\\PPSA01880.ini");
    assert(ini_get_int(doc, "patches", "quake1_direct_ip_host", -1) == 1);
    assert(ini_get_int(doc, "geral", "resolucao", 0) == 1080);
    ini_free(doc);
    puts("launcher patches: base, updates, metadata and preference persistence passed");
    return 0;
}
