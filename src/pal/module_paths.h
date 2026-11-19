/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_MODULE_PATHS_H
#define PS5C_MODULE_PATHS_H
#include "base/base.h"

#define MODULE_PATH_CAP 1024
#define MODULE_SEARCH_ROOTS 32
enum { MODULE_AGC=1, MODULE_AGC_DRIVER=2, MODULE_AMPR=4, MODULE_PSML=8 };
typedef enum module_path_origin { MODULE_PATH_NONE, MODULE_PATH_CONFIGURED, MODULE_PATH_LOCAL,
                                  MODULE_PATH_REMEMBERED, MODULE_PATH_DISCOVERED, MODULE_PATH_COLLECTION } module_path_origin;
typedef struct module_path_options {
    char install[MODULE_PATH_CAP];
    char configured[MODULE_PATH_CAP];
    char hint_file[MODULE_PATH_CAP];
    char collection_dir[MODULE_PATH_CAP];
    char roots[MODULE_SEARCH_ROOTS][MODULE_PATH_CAP];
    uint32_t root_count;
    uint32_t directory_limit;
} module_path_options;
typedef struct module_path_result {
    char directory[MODULE_PATH_CAP];
    uint32_t modules;
    uint32_t directories_checked;
    module_path_origin origin;
    bool copy_failed;
} module_path_result;

/* Recognizes ELF64/x86-64 headers, including inside SELF. This is a structural
 * check, not a guarantee of payload decryption or firmware/SDK compatibility. */
uint32_t module_path_probe(const char *directory);
/* Explicit existing custom folders win, including intentionally partial sets.
 * Automatic candidates need a structurally recognizable libSceAgc. Offline
 * fallback validates the latest complete local collection. Sets are never
 * assembled by copying files from several firmware directories. */
bool module_path_find(const module_path_options *options,module_path_result *result);
/* Host defaults: own folder/nearby installs, per-user hint and bounded local
 * PS5 install roots. No network drives, arbitrary recursion or game scanning. */
void module_path_host_options(const char *install,const char *configured,module_path_options *options);
bool module_path_remember(const module_path_options *options,const module_path_result *result);
/* Snapshot one discovered directory into an immutable, content-addressed local
 * collection. Known modules only; never overwrites files or merges sources.
 * On failure the caller may continue using the original directory. */
bool module_path_collect(const module_path_options *options,const module_path_result *source,module_path_result *result);
/* remember=true retains the source hint and a local collection snapshot;
 * false only locates/validates files and does not write preferences or copies. */
bool module_path_resolve(const char *install,const char *configured,bool remember,module_path_result *result);
#endif
