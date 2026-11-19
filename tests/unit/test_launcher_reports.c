/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercises the actual launcher report builder and WinHTTP uploader.
 * All fixtures are local, synthetic, and generated under the supplied directory. */
#define wWinMain launcher_unused_entry
#include "../../frontends/launcher/launcher.c"
#undef wWinMain

static void record_sink(void *user, const void *data, size_t size) {
    if (fwrite(data, 1, size, user) != size) abort();
}
static void discard_sink(void *user, log_level level, const char *line, size_t size) {
    (void)user; (void)level; (void)line; (void)size;
}
static bool fixture_log(const wchar_t *suffix) {
    wchar_t path[MAX_PATH], records[MAX_PATH];
    swprintf(path, MAX_PATH, L"user\\logs\\ZPS5LOGTEST%ls.log", suffix);
    FILE *raw = _wfopen(path, L"wb");
    if (!raw) return false;
    fputs("Synthetic log fixture, not a game session.\n", raw); fclose(raw);
    swprintf(records, MAX_PATH, L"%ls.records", path);
    FILE *file = _wfopen(records, L"wb"); if (!file) return false;
    log_set_sink(discard_sink, NULL); log_set_record_sink(record_sink, file);
    log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "SYNTHETIC_SAFE", "60 fps");
    log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "private", "fictitious@example.invalid");
    log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "private", "C:/Users/Fictional Person/save");
    log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "private", "Authorization: Bearer FICTIONAL-SECRET");
    log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "private", "token=FICTIONAL-TOKEN");
    char user[1024], computer[1024]; report_local_names(user, sizeof(user), computer, sizeof(computer));
    if (*user) log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "private", user);
    if (*computer) log_emit(LOG_CAT_GPU, LOG_LVL_INFO, "%s: %s", "private", computer);
    log_set_record_sink(NULL, NULL); fclose(file); return true;
}

int wmain(int argc, wchar_t **argv);
int wmain(int argc, wchar_t **argv) {
    if (argc < 3) return 2;
    wcsncpy(g_base, argv[2], MAX_PATH - 1);
    if (!SetCurrentDirectoryW(g_base)) return 3;
    g_ui_locale = ui_language_index("en");
    const char *config = "[interface]\nidioma=pt-BR\n[pastas]\njogos1=games\n[relatorios]\nservidor=https://zeraps5-reports.zeraps5.workers.dev\nenviar_ao_fechar=0\n[atualizacao]\nao_iniciar=0\n";
    g_global = ini_parse(config, strlen(config));
    if (!wcscmp(argv[1], L"prepare")) {
        CreateDirectoryW(L"user", NULL); CreateDirectoryW(L"user\\config", NULL);
        CreateDirectoryW(L"user\\logs", NULL); CreateDirectoryW(L"games", NULL);
        CreateDirectoryW(L"games\\ZPS5LOGTEST", NULL);
        FILE *fake = _wfopen(L"games\\ZPS5LOGTEST\\eboot.bin", L"wb");
        if (!fake) return 4;
        fputs("SYNTHETIC FIXTURE. NOT AN EXECUTABLE GAME.\n", fake); fclose(fake);
        if (!ini_write_file(g_global, L"user\\config\\zeraps5.ini")) return 5;
        if (!fixture_log(L"") || !fixture_log(L".anterior")) return 6;
        game sample = {0};
        wcscpy(sample.title, L"ZeraPS5 synthetic log test"); wcscpy(sample.id, L"ZPS5LOGTEST"); wcscpy(sample.version, L"TEST");
        wchar_t zip[MAX_PATH];
        if (!build_report(&sample, L"Synthetic report test. No game was run.\nfictitious@example.invalid\nC:/Users/Fictional Person/save\ntoken=FICTIONAL-TOKEN\nSafe closing detail.", REPORT_MANUAL, zip, NULL)) return 7;
        char path[4 * MAX_PATH]; wide_to_utf8(zip, path, sizeof(path)); puts(path);
        return 0;
    }
    if (argc == 5 && !wcscmp(argv[1], L"upload")) {
        char server[2048]; wide_to_utf8(argv[3], server, sizeof(server));
        ini_set(g_global, "relatorios", "servidor", server);
        wchar_t receipt[200] = L"";
        bool sent = report_upload(argv[4], L"ZPS5LOGTEST", false, receipt, 200);
        if (sent) { char text[800]; wide_to_utf8(receipt, text, sizeof(text)); puts(text); }
        return sent ? 0 : 8;
    }
    return 2;
}
