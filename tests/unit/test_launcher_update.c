/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the actual launcher download/parser/hash functions without a UI. */
#define wWinMain launcher_unused_entry
#include "../../frontends/launcher/launcher.c"
#undef wWinMain
static void test_record_sink(void *user,const void *data,size_t size) {
    if(fwrite(data,1,size,user)!=size)abort();
}
static void test_discard_log(void *user,log_level level,const char *line,size_t size) {
    (void)user;(void)level;(void)line;(void)size;
}
int wmain(int argc, wchar_t **argv) {
    /* First use needs no controller selection; user overrides must be shown
     * accurately, including deliberately disabled keys and per-game values. */
    g_global = ini_parse(user_config_default_text, strlen(user_config_default_text));
    if (!g_global) return 1;
    for (uint32_t n = 0; n < user_control_count; n++) {
        const char *value = ini_get(g_global, "teclado", user_controls[n].key);
        if (!value || strcmp(value, user_controls[n].keys) ||
            !pal_input_bind_keys(user_controls[n].control, value)) return 1;
    }
    if (strcmp(keyboard_binding(NULL, "cruz"), "SPACE,Z")) return 1;
    ini_set(g_global, "teclado", "cruz", "U");
    if (strcmp(keyboard_binding(NULL, "cruz"), "U")) return 1;
    const char *keyboard_override = "[teclado]\ncruz=\noptions=F12\n";
    ini *keyboard_game = ini_parse(keyboard_override, strlen(keyboard_override));
    if (!keyboard_game || *keyboard_binding(keyboard_game, "cruz") ||
        strcmp(keyboard_binding(keyboard_game, "options"), "F12") ||
        strcmp(keyboard_binding(keyboard_game, "esq_cima"), "W")) return 1;
    wchar_t keyboard_text[2048];
    for (unsigned n = 0; n < UI_LANGUAGE_COUNT; n++) {
        g_ui_locale = n;
        keyboard_summary(keyboard_game, keyboard_text, 2048);
        if (!wcsstr(keyboard_text, L"W / A / S / D") ||
            !wcsstr(keyboard_text, L"I / J / K / L") ||
            !wcsstr(keyboard_text, ui_string(L"(nenhuma)")) ||
            !wcsstr(keyboard_text, L"F12") || wcsstr(keyboard_text, L"%ls")) return 1;
    }
    ini_free(keyboard_game);
    ini_free(g_global); g_global = NULL; g_ui_locale = 0;
    if(UI_LANGUAGE_COUNT!=15 || !ui_languages[ui_language_index("ar")].rtl || !ui_languages[ui_language_index("fa")].rtl)return 1;
    if(ui_languages[ui_language_index("ru")].rtl || ui_languages[ui_language_index("uk")].rtl)return 1;
    if(wcscmp(ui_translate(ui_language_index("en"),L"Jogar"),L"Play"))return 1;
    if(wcscmp(ui_translate(ui_language_index("fa"),L"Jogar"),L"بازی"))return 1;
    for(unsigned l=0;l<UI_LANGUAGE_COUNT;l++) {
        if(ui_language_index(ui_languages[l].tag)!=l)return 1;
        for(unsigned n=0;n<sizeof(ui_messages)/sizeof(ui_messages[0]);n++)
            if(!ui_messages[n][l] || !*ui_messages[n][l])return 1;
    }
    /* Exercise the renderer against every translated printf template, not just
     * a textual placeholder check. Values must never be interpreted as code. */
    if(sizeof(ui_log_messages)/sizeof(ui_log_messages[0])<550)return 1;
    log_record *record=calloc(1,sizeof(*record));if(!record)return 1;
    char rendered[LOG_RECORD_MAX];
    for(unsigned n=0;n<sizeof(ui_log_messages)/sizeof(ui_log_messages[0]);n++) {
        record->format=ui_log_messages[n].source;record->count=0;
        for(const char *p=record->format;*p;) {
            if(*p!='%'){p++;continue;}
            size_t spec=log_format_spec(p);if(!spec){free(record);return 1;}
            if(p[1]!='%') {
                if(record->count==LOG_RECORD_FIELDS){free(record);return 1;}
                record->fields[record->count++]="0xABCDEF / %s ";
            }
            p+=spec;
        }
        for(unsigned l=0;l<UI_LANGUAGE_COUNT;l++)
            if(!ui_log_messages[n].text[l] || !*ui_log_messages[n].text[l] ||
               !log_record_render(record,ui_log_messages[n].text[l],rendered,sizeof(rendered))) {free(record);return 1;}
    }
    free(record);
    if(argc==3 && !wcscmp(argv[1],L"language-tests")) {
        wcsncpy(g_base,argv[2],MAX_PATH-1);SetCurrentDirectoryW(g_base);
        const char seed[]="[geral]\nidioma=17\n[pastas]\njogos1=games-test\n";
        g_global=ini_parse(seed,strlen(seed));
        for(unsigned l=0;l<UI_LANGUAGE_COUNT;l++) {
            if(!ui_save_preference(l))return 1;
            load_global();
            if(g_ui_locale!=l || ini_get_int(g_global,"geral","idioma",-1)!=17)return 1;
            if(strcmp(ini_get(g_global,"pastas","jogos1"),"games-test"))return 1;
            for(uint32_t i=0;i<user_control_count;i++) {
                wchar_t label[128];ui_control_label(i,label,128);
                if(!*label)return 1;
                if(l==ui_language_index("en") && (wcsstr(label,L"Analógico") || wcsstr(label,L"Direcional")))return 1;
            }
        }
        wchar_t raw[MAX_PATH],records[MAX_PATH],view[MAX_PATH];
        swprintf(raw,MAX_PATH,L"%ls\\sample.log",g_base);
        swprintf(records,MAX_PATH,L"%ls.records",raw);
        FILE *sample=_wfopen(records,L"wb");if(!sample)return 1;
        log_set_sink(test_discard_log,NULL);log_set_record_sink(test_record_sink,sample);
        const char *message="guest address range already in use at startup; 'load'/'run' may fail";
        log_emit(LOG_CAT_MEM,LOG_LVL_WARN,"guest address range already in use at startup; 'load'/'run' may fail");
        log_emit(LOG_CAT_LOADER,LOG_LVL_ERROR,"%s: %s","busy",ps5c_err_str(PS5C_ERR_NOT_FOUND));
        log_set_record_sink(NULL,NULL);fclose(sample);
        for(unsigned l=0;l<UI_LANGUAGE_COUNT;l++) {
            g_ui_locale=l;swprintf(view,MAX_PATH,L"%ls\\sample-%hs.txt",g_base,ui_languages[l].tag);
            if(!translated_log(raw,view))return 1;
            sample=_wfopen(view,L"rb");if(!sample)return 1;
            char data[8192];size_t n=fread(data,1,sizeof(data)-1,sample);data[n]=0;fclose(sample);
            if(l!=ui_language_index("en") && strstr(data,message))return 1;
            if(!strstr(data,"busy"))return 1; /* a module name, not a status */
            if(l!=ui_language_index("en") && strstr(data,"not found"))return 1;
            if(strstr(data,"TEMPORARY-CATALOG"))return 1;
        }
        /* The selected viewer language can change after a session was recorded. */
        wchar_t original_env[1024];DWORD old_len=GetEnvironmentVariableW(L"PS5C_LAUNCHER_RECORDS",original_env,1024);
        wchar_t *environment=record_environment_session(records,NULL);if(!environment)return 1;
        bool found=false;const wchar_t *previous=NULL;
        for(const wchar_t *p=environment;*p;p+=wcslen(p)+1) {
            if(previous && _wcsicmp(previous,p)>0)return 1;
            if(!wcsncmp(p,L"PS5C_LAUNCHER_RECORDS=",22))found=true;
            previous=p;
        }
        free(environment);if(!found)return 1;
        wchar_t after_env[1024];DWORD new_len=GetEnvironmentVariableW(L"PS5C_LAUNCHER_RECORDS",after_env,1024);
        if(old_len!=new_len || (old_len && wcscmp(original_env,after_env)))return 1;
        if(ui_save_preference((unsigned)UI_LANGUAGE_COUNT))return 1;
        ini_free(g_global);g_global=NULL;
        puts("15 interface languages persist; console language and game folders preserved; Arabic/Persian RTL verified");return 0;
    }
    if(argc==4 && !wcscmp(argv[1],L"query")) {
        wcsncpy(g_base,argv[3],MAX_PATH-1); SetCurrentDirectoryW(g_base);
        update_info result;
        if(!read_update_manifest(argv[2],&result)) return 2;
        printf("%d|%s|%s\n",result.build,result.zip,result.sha); return 0;
    }
    if(argc==5 && !wcscmp(argv[1],L"query-language")) {
        wcsncpy(g_base,argv[3],MAX_PATH-1);SetCurrentDirectoryW(g_base);
        char tag[32];wide_to_utf8(argv[4],tag,sizeof(tag));g_ui_locale=ui_language_index(tag);
        update_info result;if(!read_update_manifest(argv[2],&result))return 2;
        puts(result.notes_localized[g_ui_locale]);return 0;
    }
    if(argc==4 && !wcscmp(argv[1],L"download")) return http_download(argv[2],argv[3])?0:2;
    if(argc==3 && !wcscmp(argv[1],L"sha")) {
        char digest[65]; if(!sha256_file(argv[2],digest))return 2;
        puts(digest);return 0;
    }
    if(!report_private_line("path C:\\Users\\Private Person\\save", "", ""))return 1;
    if(!report_private_line("loaded G:/private/game/eboot.bin", "", ""))return 1;
    if(!report_private_line("host MY-COMPUTER", "", "my-computer"))return 1;
    if(!report_private_line("Authorization: Bearer xyz", "", ""))return 1;
    if(report_private_line("Vulkan device: RTX; 30 fps", "test-user", "test-computer"))return 1;
    puts("launcher report privacy: ok");return 0;
}
