/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ZERAPS5_LOCALIZATION_H
#define ZERAPS5_LOCALIZATION_H
#include <stdbool.h>
#include <string.h>
#include <wchar.h>
typedef struct ui_language { const char *tag; const wchar_t *name; bool rtl; } ui_language;
static const ui_language ui_languages[] = {
    {"pt-BR",L"Português (Brasil)",false},{"pt-PT",L"Português (Portugal)",false},
    {"en",L"English",false},{"es",L"Español",false},{"fr",L"Français",false},
    {"de",L"Deutsch",false},{"it",L"Italiano",false},{"ja",L"日本語",false},
    {"ko",L"한국어",false},{"zh-CN",L"简体中文",false},{"zh-TW",L"繁體中文",false},
    {"ar",L"العربية",true},{"fa",L"فارسی",true},{"ru",L"Русский",false},{"uk",L"Українська",false}
};
#define UI_LANGUAGE_COUNT (sizeof(ui_languages)/sizeof(ui_languages[0]))
#include "translations.inc"
static unsigned ui_language_index(const char *tag) {
    for(unsigned n=0;tag && n<UI_LANGUAGE_COUNT;n++) if(!strcmp(tag,ui_languages[n].tag)) return n;
    return 0;
}
static const wchar_t *ui_translate(unsigned locale,const wchar_t *text) {
    if(!text) return NULL;
    if(locale>=UI_LANGUAGE_COUNT)locale=0;
    for(unsigned n=0;n<sizeof(ui_messages)/sizeof(ui_messages[0]);n++)
        if(!wcscmp(text,ui_messages[n][0]))return ui_messages[n][locale];
    return text; /* filenames, game names, keys and technical values stay intact */
}
#endif
