/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/log.h"
#include "core/game_patch.h"
#include "core/game_fixes.h"
#include "base/log_record.h"
#include "base/log_record_file.h"
#ifdef _WIN32
#include "pal_win32/report_session.h"
#endif
#include <wchar.h>
#include "pal/module_paths.h"
#include <stdatomic.h>
#include "core/runtime.h"
#include "core/settings.h"
#include "hle/video_out.h"
#include "gpu/rdna2.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/shader_corpus.h"
#include "loader/linker.h"
#include "loader/module_set.h"
#include "loader/nid.h"
#include "loader/nid_db.h"
#include "mem/gmem.h"
#include "pal/pal.h"
#ifdef _WIN32
#include "pal/ps4_camera.h"
#include "pal/pal_camera.h"
#endif
#include "pal/pal_exfat.h"
#include "pal/pal_pkg.h"
#include "base/kraken.h"
#include "pal/pal_pkg_inner.h"
#include "ps5c/build_info.h"
#include "ps5c/version.h"
#include "recorder/profile.h"

#define MAX_DIRS 16

/* "0.1.0+abc1234" ("-dirty": built with uncommitted changes) */
#define VERSION_FULL PS5C_VERSION_STRING "+" PS5C_GIT_COMMIT
#define VERSION_DIRTY (PS5C_GIT_DIRTY ? "-dirty" : "")

#ifdef _WIN32
static log_session_meta core_session;
static wchar_t core_meta_path[MAX_PATH];
static bool core_session_known;
static void core_session_stage(unsigned stage) {
    if(!core_session_known)return;
    core_session.stage=stage;
    if(!report_session_meta_save(core_meta_path,&core_session))core_session_known=false;
}
static void core_session_guest(int64_t code) {
    if(!core_session_known)return;
    core_session.guest_known=1;core_session.guest_code=code;
    if(!report_session_meta_save(core_meta_path,&core_session))core_session_known=false;
}
#endif

static void print_usage(void) {
    printf("PS5C %s - PlayStation 5 emulator core\n\n"
           "usage:\n"
           "  ps5c info <file>                  show headers, libraries and imports of an ELF/SELF\n"
           "  ps5c analyze <eboot> [options]    write the static game profile (JSON)\n"
           "  ps5c load <eboot> [options]       map and link all modules into guest memory\n"
           "  ps5c run <eboot|image.exfat> [options]        load, link and execute the game natively\n"
           "  ps5c pkg <file.pkg> [dir]         show a PS5/PS4 package; with dir, write its metadata to dir/sce_sys\n"
           "  ps5c disasm <shader.bin>          disassemble an RDNA2 shader binary\n"
           "  ps5c shaders <dir> [report.md]    check a shader corpus (PS5C_SHADER_CORPUS=1), or user/;\n"
           "                                    PS5C_SHADERS_SPIRV=<dir> also writes each module there,\n"
           "                                    PS5C_SHADERS_FLOAT_CONTROLS=1 as on GPUs with float controls\n"
           "  ps5c --host-info                 show host platform information\n"
           "  ps5c --list-gpus                 list the host GPUs (number, dedicated/integrated, name)\n"
           "  ps5c camera [firmware.bin]       PS4 Camera state; with a file, send it the firmware\n"
           "  ps5c --list-cameras              list the host cameras for the launcher\n"
           "  ps5c --find-system-modules       locate local system libraries without changing preferences\n"
           "  ps5c --collect-system-modules    retain discovered libraries in the local collection\n"
           "  ps5c --version\n\n"
           "options:\n"
           "  -o <file>            output file for analyze (default: game_profile.json)\n"
           "  --modules <dir>      directory with .sprx/.prx modules (repeatable); the\n"
           "                       'sce_module' folder next to the eboot is added automatically\n"
           "  --nid-db <file>      NID name database (default: data/nid_names.txt)\n"
           "  --hle <library>      force a library to HLE\n"
           "  --lle <library>      force a library to LLE\n"
           "  --hybrid <library>   HLE where implemented, LLE for the rest\n"
           "  --stop-on-missing    stop at the first call to an unimplemented import (run)\n"
           "  --trace-hle          log every HLE call (run)\n"
           "  -v                   verbose logging\n",
           PS5C_VERSION_STRING);
}

typedef struct options {
    const char *command;
    const char *input;
    const char *input2; /* second positional argument */
    const char *output;
    const char *nid_db_path;
    const char *dirs[MAX_DIRS];
    uint32_t dir_count;
    const char *force_hle[64];
    uint32_t hle_count;
    const char *force_lle[64];
    uint32_t lle_count;
    const char *force_hybrid[64];
    uint32_t hybrid_count;
    bool stop_on_missing;
    bool trace_hle;
} options;

static bool parse_args(int argc, char **argv, options *o) {
    memset(o, 0, sizeof(*o));
    o->output = "game_profile.json";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_next = i + 1 < argc;
        if (strcmp(a, "-o") == 0 && has_next) {
            o->output = argv[++i];
        } else if (strcmp(a, "--modules") == 0 && has_next && o->dir_count < MAX_DIRS) {
            o->dirs[o->dir_count++] = argv[++i];
        } else if (strcmp(a, "--nid-db") == 0 && has_next) {
            o->nid_db_path = argv[++i];
        } else if (strcmp(a, "--hle") == 0 && has_next && o->hle_count < 64) {
            o->force_hle[o->hle_count++] = argv[++i];
        } else if (strcmp(a, "--lle") == 0 && has_next && o->lle_count < 64) {
            o->force_lle[o->lle_count++] = argv[++i];
        } else if (strcmp(a, "--hybrid") == 0 && has_next && o->hybrid_count < 64) {
            o->force_hybrid[o->hybrid_count++] = argv[++i];
        } else if (strcmp(a, "--stop-on-missing") == 0) {
            o->stop_on_missing = true;
        } else if (strcmp(a, "--trace-hle") == 0) {
            o->trace_hle = true;
        } else if (strcmp(a, "-v") == 0) {
            log_set_level_all(LOG_LVL_DEBUG);
        } else if (a[0] == '-' && strcmp(a, "-") != 0 && o->command) {
            fprintf(stderr, "unknown option '%s'\n", a);
            return false;
        } else if (!o->command) {
            o->command = a;
        } else if (!o->input) {
            o->input = a;
        } else if (!o->input2) {
            o->input2 = a;
        } else {
            fprintf(stderr, "unexpected argument '%s'\n", a);
            return false;
        }
    }
    return true;
}

/* Directory part of a path, including the trailing separator ("" if none). */
static void dir_of(const char *path, char *out, size_t n) {
    size_t cut = 0;
    for (size_t i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\') cut = i + 1;
    }
    if (cut >= n) cut = n - 1;
    memcpy(out, path, cut);
    out[cut] = '\0';
}

static bool load_nid_db(nid_db *db, const char *explicit_path, const char *argv0) {
    char exe_dir[512], candidate[1024];
    dir_of(argv0, exe_dir, sizeof(exe_dir));
    const char *paths[4];
    int n = 0;
    if (explicit_path) {
        paths[n++] = explicit_path;
    } else {
        paths[n++] = "data/nid_names.txt";
        snprintf(candidate, sizeof(candidate), "%sdata/nid_names.txt", exe_dir);
        paths[n++] = candidate;
    }
    for (int i = 0; i < n; i++) {
        uint8_t *text;
        uint64_t size;
        if (pal_file_read_all(paths[i], &text, &size) != PS5C_OK) {
            continue;
        }
        ps5c_err err = nid_db_load_text(db, (const char *)text, (size_t)size);
        free(text);
        if (err == PS5C_OK) {
            LOG_I(LOADER, "NID database: %u names from %s", db->count, paths[i]);
            return true;
        }
    }
    if (explicit_path) {
        LOG_E(LOADER, "cannot load NID database '%s'", explicit_path);
        return false;
    }
    LOG_W(LOADER, "no NID database found; imports will be shown as raw NIDs");
    return true;
}

static void print_info(const loaded_module *m, const nid_db *db) {
    const sce_image *img = &m->img;
    printf("\n== %s ==\n", m->path);
    printf("container       : %s\n", sce_container_name(img->container));
    printf("dynamic layout  : %s\n", !img->has_dynamic ? "none"
                                     : img->tables_in_memory ? "PS5 (tables in loadable segment)"
                                                             : "PS4 (PT_SCE_DYNLIBDATA)");
    printf("type            : %s (0x%04X)\n", sce_elf_type_name(img->elf_type), img->elf_type);
    printf("entry           : 0x%llx\n", (unsigned long long)img->entry);
    if (img->has_module_info) {
        printf("module          : %s v%u.%u\n", img->module_info.name, img->module_info.ver_major,
               img->module_info.ver_minor);
    }
    if (img->original_filename) printf("original name   : %s\n", img->original_filename);
    printf("image range     : 0x%llx - 0x%llx\n", (unsigned long long)img->image_lo,
           (unsigned long long)img->image_hi);

    printf("\nsegments:\n  %-3s %-17s %-4s %-14s %-12s %-12s\n", "#", "type", "flg", "vaddr", "filesz",
           "memsz");
    for (uint32_t i = 0; i < img->segment_count; i++) {
        const elf64_phdr *ph = &img->segments[i].h;
        printf("  %-3u %-17s %c%c%c  0x%-12llx 0x%-10llx 0x%-10llx\n", i, sce_phdr_type_name(ph->p_type),
               ph->p_flags & PF_R ? 'R' : '-', ph->p_flags & PF_W ? 'W' : '-',
               ph->p_flags & PF_X ? 'X' : '-', (unsigned long long)ph->p_vaddr,
               (unsigned long long)ph->p_filesz, (unsigned long long)ph->p_memsz);
    }

    if (img->needed_module_count) {
        printf("\nneeded modules:\n");
        for (uint32_t i = 0; i < img->needed_module_count; i++) {
            printf("  [%3u] %s\n", img->needed_modules[i].id, img->needed_modules[i].name);
        }
    }
    if (img->import_lib_count) {
        printf("\nimported libraries:\n");
        for (uint32_t i = 0; i < img->import_lib_count; i++) {
            printf("  [%3u] %s\n", img->import_libs[i].id, img->import_libs[i].name);
        }
    }
    if (img->export_lib_count) {
        printf("\nexported libraries:\n");
        for (uint32_t i = 0; i < img->export_lib_count; i++) {
            printf("  [%3u] %s\n", img->export_libs[i].id, img->export_libs[i].name);
        }
    }

    uint32_t imports = 0, named = 0;
    printf("\nimports:\n");
    for (uint32_t i = 0; i < img->sym_count; i++) {
        const sce_sym *s = &img->syms[i];
        if (s->shndx != SHN_UNDEF || !s->has_nid) continue;
        const char *lib = sce_image_sym_lib_name(img, s);
        const char *name = db ? nid_db_lookup(db, s->nid) : NULL;
        char nid[NID_STR_LEN + 1];
        nid_to_str(s->nid, nid);
        printf("  %s  %-24s %s\n", nid, lib ? lib : "?", name ? name : "");
        imports++;
        named += name != NULL;
    }
    printf("\n%u imports, %u with known names, %u relocations\n", imports, named, img->reloc_count);
}

static int cmd_info(const options *o, const nid_db *db) {
    module_set ms;
    module_set_init(&ms);
    loaded_module *m;
    int rc = 1;
    if (module_set_add_file(&ms, o->input, false, &m) == PS5C_OK) {
        print_info(m, db);
        rc = 0;
    }
    module_set_free(&ms);
    return rc;
}

/* Game modules (.prx) of a module directory the game may load at run time
 * with sceKernelLoadStartModule: they are loaded with the process. */
typedef struct preload_ctx {
    module_set *ms;
    const char *dir;
    uint32_t added;
} preload_ctx;

static bool preload_entry(void *user, const char *name, bool is_dir) {
    preload_ctx *p = (preload_ctx *)user;
    size_t n = strlen(name);
    if (is_dir || n < 5 || strcmp(name + n - 4, ".prx") != 0) return true;
    char mod[256];
    snprintf(mod, sizeof(mod), "%.*s", (int)(n - 4), name);
    for (size_t i = 0; i < p->ms->modules.len; i++) {
        const char *path = p->ms->modules.data[i]->path;
        if (!strcmp(p->ms->modules.data[i]->name, mod) || (path && strstr(path, name))) return true;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", p->dir, name);
    loaded_module *m = NULL;
    if (module_set_add_file(p->ms, path, false, &m) == PS5C_OK) {
        LOG_I(LOADER, "preloaded game module %s", path);
        if (m) m->deferred_start = true;
        p->added++;
    }
    return true;
}

/* Folders of the game that hold modules (.prx/.sprx), found by walking the
 * dump a few levels deep: fakelib/, Media/Modules/, PS5/prx/, the root... */
#define MAX_AUTO_DIRS 32
typedef struct scan_ctx {
    const char *dir;
    int depth;
    bool has_modules;
    char (*found)[600];
    uint32_t *count;
} scan_ctx;

static bool scan_entry(void *user, const char *name, bool is_dir) {
    scan_ctx *c = (scan_ctx *)user;
    size_t n = strlen(name);
    if (!is_dir) {
        if ((n > 4 && !strcmp(name + n - 4, ".prx")) || (n > 5 && !strcmp(name + n - 5, ".sprx"))) c->has_modules = true;
        return true;
    }
    if (c->depth >= 3 || !strcmp(name, "sce_sys") || !strcmp(name, "sce_module") || *c->count >= MAX_AUTO_DIRS) return true;
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/%s", c->dir, name);
    scan_ctx child = {sub, c->depth + 1, false, c->found, c->count};
    pal_dir_list(sub, scan_entry, &child);
    if (child.has_modules && *c->count < MAX_AUTO_DIRS) snprintf(c->found[(*c->count)++], 600, "%s", sub);
    return true;
}

static bool is_fakelib(const char *dir) {
    size_t n = strlen(dir);
    return n >= 7 && !strcmp(dir + n - 7, "fakelib") && (n == 7 || dir[n - 8] == '/' || dir[n - 8] == '\\');
}

/* Loads the main executable plus every needed module that can be found. */
static bool setup_modules(const options *o, module_set *ms) {
    module_set_init(ms);
    /* An update laid over the game: its eboot.bin and sce_module/ run (core/game_patch.h) */
    char game_dir[512], patch_dir[512], main_file[600];
    dir_of(o->input, game_dir, sizeof(game_dir));
    bool patched = !exfat_image_path(o->input) && game_patch_find(game_dir[0] ? game_dir : ".", patch_dir, sizeof(patch_dir));
    snprintf(main_file, sizeof(main_file), "%s", o->input);
    /* or the decrypted executables of a game whose own files stay encrypted beside them */
    char base_dir[512];
    if (!patched && !exfat_image_path(o->input) && game_patch_base_of(game_dir[0] ? game_dir : ".", base_dir, sizeof(base_dir))) {
        snprintf(patch_dir, sizeof(patch_dir), "%s", game_dir[0] ? game_dir : ".");
        size_t pl = strlen(patch_dir);
        while (pl > 1 && (patch_dir[pl - 1] == '/' || patch_dir[pl - 1] == '\\')) patch_dir[--pl] = 0;
        snprintf(game_dir, sizeof(game_dir), "%s/", base_dir);
        patched = true;
    }
    if (patched) {
        char base[512];
        snprintf(base, sizeof(base), "%s", game_dir[0] ? game_dir : ".");
        size_t bl = strlen(base);
        while (bl > 1 && (base[bl - 1] == '/' || base[bl - 1] == '\\')) base[--bl] = 0;
        game_patch_set(base, patch_dir);
        char eboot[600];
        pal_path_info pi;
        snprintf(eboot, sizeof(eboot), "%s/eboot.bin", patch_dir);
        if (pal_path_stat(eboot, &pi) == PS5C_OK && !pi.is_dir) snprintf(main_file, sizeof(main_file), "%s", eboot);
    }
    if (module_set_add_file(ms, main_file, true, NULL) != PS5C_OK) {
        module_set_free(ms);
        return false;
    }
    for (uint32_t i = 0; i < o->hle_count; i++) module_set_set_policy(ms, o->force_hle[i], LIB_POLICY_HLE);
    for (uint32_t i = 0; i < o->lle_count; i++) module_set_set_policy(ms, o->force_lle[i], LIB_POLICY_LLE);
    for (uint32_t i = 0; i < o->hybrid_count; i++) module_set_set_policy(ms, o->force_hybrid[i], LIB_POLICY_HYBRID);

    /* Game-shipped libraries live in sce_module/ next to the eboot (the update's first). */
    char sce_module_dir[600], patch_module_dir[600];
    char patch_param[600];
    pal_path_info ppi;
    snprintf(patch_param, sizeof(patch_param), "%s/sce_sys/param.json", patched ? patch_dir : ".");
    bool patch_has_param = patched && pal_path_stat(patch_param, &ppi) == PS5C_OK;
    settings_load(patch_has_param ? patch_dir : game_dir[0] ? game_dir : "./");
    snprintf(sce_module_dir, sizeof(sce_module_dir), "%ssce_module", game_dir[0] ? game_dir : "./");
    snprintf(patch_module_dir, sizeof(patch_module_dir), "%s/sce_module", patched ? patch_dir : ".");
    /* Search order: sce_module/, --modules folders, the game's own module
     * folders, then system/modules/ (the console's system libraries, which
     * the user dumps from their console; never part of the repository), then
     * fakelib/: a backport's stand-ins for system libraries an older console
     * firmware lacks, used only for what system/modules does not have (Sonic
     * Frontiers' fakelib libSceAmpr fails where the system one works). */
    static char auto_dirs[MAX_AUTO_DIRS][600];
    uint32_t auto_count = 0;
    /* an update's own module folders first: its modules replace the game's (and a game
     * whose files stay encrypted has its decrypted modules only there) */
    if (patched) {
        scan_ctx pc = {patch_dir, 0, false, auto_dirs, &auto_count};
        pal_dir_list(patch_dir, scan_entry, &pc);
        if (pc.has_modules && auto_count < MAX_AUTO_DIRS) snprintf(auto_dirs[auto_count++], 600, "%s", patch_dir);
    }
    char root[600];
    snprintf(root, sizeof(root), "%s", game_dir[0] ? game_dir : "./");
    if (root[strlen(root) - 1] == '/' || root[strlen(root) - 1] == '\\') root[strlen(root) - 1] = '\0';
    if (exfat_image_path(root) && strlen(root)+2 < sizeof(root)) strcat(root, "/.");
    scan_ctx sc = {root, 0, false, auto_dirs, &auto_count};
    pal_dir_list(root, scan_entry, &sc);
    if (sc.has_modules && auto_count < MAX_AUTO_DIRS) snprintf(auto_dirs[auto_count++], 600, "%s", root);

    const char *dirs[MAX_DIRS + MAX_AUTO_DIRS + 3];
    uint32_t dir_count = 0;
    if (patched) dirs[dir_count++] = patch_module_dir;
    dirs[dir_count++] = sce_module_dir;
    for (uint32_t i = 0; i < o->dir_count; i++) dirs[dir_count++] = o->dirs[i];
    for (uint32_t i = 0; i < auto_count; i++) {
        if (is_fakelib(auto_dirs[i])) continue;
        bool dup = false;
        for (uint32_t k = 0; k < dir_count && !dup; k++) dup = !strcmp(dirs[k], auto_dirs[i]);
        if (!dup) dirs[dir_count++] = auto_dirs[i];
    }
    uint32_t game_dirs = dir_count;
    pal_path_info si;
    const char *system_dir = settings_get("pastas", "sistema"); /* [pastas] sistema, system/modules by default */
    if (!system_dir || !*system_dir) system_dir = "system/modules";
    module_path_result system_path;
    bool needs_system=false;
    for(size_t m=0;m<ms->modules.len;m++)for(uint32_t n=0;n<ms->modules.data[m]->img.needed_module_count;n++) {
        const char *name=ms->modules.data[m]->img.needed_modules[n].name;
        if(!strcmp(name,"libSceAgc") || !strcmp(name,"libScePsml"))needs_system=true;
    }
    if(needs_system && !o->dir_count && module_path_resolve(NULL,system_dir,true,&system_path))system_dir=system_path.directory;
    if (pal_path_stat(system_dir, &si) == PS5C_OK && si.is_dir) dirs[dir_count++] = system_dir;
    for (uint32_t i = 0; i < auto_count; i++)
        if (is_fakelib(auto_dirs[i])) dirs[dir_count++] = auto_dirs[i];
    for (uint32_t i = 1; i < dir_count; i++) LOG_I(LOADER, "module folder: %s", dirs[i]);

    uint32_t added = module_set_load_needed(ms, dirs, dir_count);
    /* A needed module from a game folder the console's loader does not search (Unity's
     * Media/Modules) is bound when the game loads it with sceKernelLoadStartModule, and its
     * module_start gets the game's arguments then (TMNT Wrath of the Mutants: the PSN plugin
     * in Il2CppUserAssemblies checks them). Only sce_module/ is loaded with the process. */
    for (uint32_t i = 1; i < game_dirs; i++) {
        size_t dl = strlen(dirs[i]);
        if ((dl >= 10 && !strcmp(dirs[i] + dl - 10, "sce_module")) || !strcmp(dirs[i], root) ||
            (patched && !strcmp(dirs[i], patch_dir)) || i < 1 + (patched ? 1u : 0u) + o->dir_count)
            continue;
        for (size_t k = 0; k < ms->modules.len; k++) {
            loaded_module *m = ms->modules.data[k];
            const char *p = m->path;
            if (m->is_main || !p || strncmp(p, dirs[i], dl) || (p[dl] != '/' && p[dl] != '\\') || strpbrk(p + dl + 1, "/\\"))
                continue;
            if (!m->deferred_start) LOG_I(LOADER, "%s starts when the game loads it", m->name);
            m->deferred_start = true;
        }
    }
    for (uint32_t i = 1; i < game_dirs; i++) { /* game modules the game may load at run time */
        size_t dl = strlen(dirs[i]);
        if (dl >= 10 && !strcmp(dirs[i] + dl - 10, "sce_module")) continue; /* system libraries: only when needed */
        preload_ctx p = {ms, dirs[i], 0};
        pal_dir_list(dirs[i], preload_entry, &p);
        if (p.added) added += p.added + module_set_load_needed(ms, dirs, dir_count);
    }
    LOG_I(LOADER, "%u additional modules loaded", added);
    return true;
}

static int cmd_load(const options *o) {
    module_set ms;
    if (!setup_modules(o, &ms)) {
        return 1;
    }
    gmem mem;
    gmem_config cfg = gmem_default_config();
    if (gmem_init(&mem, &cfg) != PS5C_OK) {
        module_set_free(&ms);
        return 1;
    }
    linker lk;
    int rc = 1;
    if (linker_init(&lk, &mem, &ms) == PS5C_OK && linker_link_all(&lk) == PS5C_OK) {
        printf("\nguest memory map:\n  %-18s %-18s %-5s %s\n", "start", "end", "prot", "name");
        for (size_t i = 0; i < mem.vmas.len; i++) {
            const gmem_vma *v = &mem.vmas.data[i];
            printf("  0x%016llx 0x%016llx %c%c%c   %s\n", (unsigned long long)v->start,
                   (unsigned long long)(v->start + v->size), v->prot & GPROT_READ ? 'r' : '-',
                   v->prot & GPROT_WRITE ? 'w' : '-', v->prot & GPROT_EXEC ? 'x' : '-', v->name);
        }
        const link_stats *s = &lk.stats;
        printf("\nmodules mapped      : %u (%.1f MiB of guest memory)\n", s->modules_mapped,
               (double)mem.mapped_bytes / (1024.0 * 1024.0));
        printf("relocations applied : %llu (RELATIVE %llu, JUMP_SLOT %llu, GLOB_DAT %llu, 64 %llu, TLS %llu)\n",
               (unsigned long long)s->relocs_applied, (unsigned long long)s->relocs_by_type[R_X86_64_RELATIVE],
               (unsigned long long)s->relocs_by_type[R_X86_64_JUMP_SLOT],
               (unsigned long long)s->relocs_by_type[R_X86_64_GLOB_DAT],
               (unsigned long long)s->relocs_by_type[R_X86_64_64],
               (unsigned long long)(s->relocs_by_type[R_X86_64_DTPMOD64] + s->relocs_by_type[R_X86_64_DTPOFF64] +
                                    s->relocs_by_type[R_X86_64_TPOFF64]));
        printf("relocations skipped : %llu\n", (unsigned long long)s->relocs_unsupported);
        printf("import bindings     : %u local, %u LLE, %u HLE, %u missing, %u weak->0\n", s->binds_local,
               s->binds_lle, s->binds_hle, s->binds_missing, s->weak_zero);
        printf("import stubs        : %u functions, %u data objects\n", lk.code_used, lk.data_used);
        printf("static TLS          : %llu bytes\n", (unsigned long long)s->static_tls_size);
        for (size_t i = 0; i < ms.modules.len; i++) {
            const loaded_module *m = ms.modules.data[i];
            if (m->is_main) {
                printf("entry point         : 0x%llx\n", (unsigned long long)(m->bias + m->img.entry));
            }
        }
        rc = 0;
    }
    linker_free(&lk);
    gmem_free(&mem);
    module_set_free(&ms);
    return rc;
}

/* One line per instruction: dword offset, encoding words, text. Lines the
 * decoder cannot print exactly start with '!'. */
/* ---- ps5c shaders <dir>: offline check of a shader corpus ----
 * Every entry (PS5C_SHADER_CORPUS=1, user/<game>/corpus) is analyzed and
 * translated again; failures are grouped by the reason the analyzer or the
 * translator logs (addresses removed), so the most frequent gaps come first.
 * <dir> is a corpus folder, or a folder whose subfolders have one (user/). */

typedef struct corpus_reason {
    char text[160];
    char example[24];
    uint32_t count;
} corpus_reason;

typedef struct corpus_stats {
    uint32_t entries, unreadable, code_only, analyzed, analysis_failed, sites_differ, translated, translate_failed;
    uint64_t spirv_words;
    corpus_reason *reasons;
    uint32_t reason_count, reason_cap;
    char current[24]; /* entry being checked (its file name) */
    bool failed;      /* the current entry logged an error */
} corpus_stats;

static corpus_stats *g_corpus;

/* "shader 0x1234 +0x40: LDS outside compute shaders (ds_write_b32)" -> "LDS outside compute shaders (ds_write_b32)" */
static void corpus_reason_text(const char *line, size_t len, char *out, size_t size) {
    const char *s = strstr(line, "shader 0x");
    const char *end = line + len;
    if (s) {
        const char *colon = memchr(s, ':', (size_t)(end - s));
        if (colon) s = colon + 1;
    } else {
        s = line;
    }
    size_t n = 0;
    while (s < end && n + 1 < size) {
        if (s[0] == '(' && s + 3 < end && s[1] == '+' && s[2] == '0' && s[3] == 'x') { /* "(+0x40)" */
            while (s < end && *s != ')') s++;
            if (s < end) s++;
            continue;
        }
        if (s[0] == '0' && s + 1 < end && s[1] == 'x') { /* addresses and words */
            s += 2;
            while (s < end && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) s++;
            out[n++] = 'X';
            continue;
        }
        if (*s == '\n' || *s == '\r') break;
        out[n++] = *s++;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    size_t start = 0;
    while (start < n && out[start] == ' ') start++;
    memmove(out, out + start, n - start);
    out[n - start] = 0;
}

static void reason_add(corpus_stats *c, const char *text, const char *example, uint32_t count) {
    for (uint32_t i = 0; i < c->reason_count; i++) {
        if (!strcmp(c->reasons[i].text, text)) {
            c->reasons[i].count += count;
            return;
        }
    }
    if (c->reason_count == c->reason_cap) {
        uint32_t cap = c->reason_cap ? c->reason_cap * 2 : 64;
        corpus_reason *r = (corpus_reason *)realloc(c->reasons, sizeof(corpus_reason) * cap);
        if (!r) return;
        c->reasons = r;
        c->reason_cap = cap;
    }
    corpus_reason *r = &c->reasons[c->reason_count++];
    snprintf(r->text, sizeof(r->text), "%s", text);
    snprintf(r->example, sizeof(r->example), "%s", example);
    r->count = count;
}

static void corpus_sink(void *user, log_level level, const char *line, size_t len) {
    PS5C_UNUSED(user);
    corpus_stats *c = g_corpus;
    if (!c || level < LOG_LVL_ERROR) return;
    c->failed = true;
    char text[160];
    corpus_reason_text(line, len, text, sizeof(text));
    reason_add(c, text, c->current, 1);
}

static void corpus_check(const char *path, const char *name, corpus_stats *c, sh_program *p) {
    sh_corpus_entry e;
    c->entries++;
    if (!sh_corpus_read(path, &e)) {
        c->unreadable++;
        return;
    }
    snprintf(c->current, sizeof(c->current), "%s", name);
    c->failed = false;
    if (!sh_corpus_analyze(&e, p)) {
        c->analysis_failed++;
    } else if (e.kind == SH_CORPUS_CODE) {
        c->code_only++; /* the analysis failed in the game and passes now */
        c->analyzed++;
    } else if (p->site_count != e.site_count) {
        c->sites_differ++; /* written by an analyzer that found other sites */
        c->analyzed++;
    } else {
        c->analyzed++;
        uint32_t *spv = NULL;
        size_t words = 0;
        if (sh_translate(p, &e.info, e.shapes, e.binding_base, &spv, &words)) {
            c->translated++;
            c->spirv_words += words;
            const char *out = getenv("PS5C_SHADERS_SPIRV"); /* for spirv-val --target-env vulkan1.1 */
            if (out) {
                char spv_path[1024];
                snprintf(spv_path, sizeof(spv_path), "%s/%.*s.spv", out, (int)(strlen(name) - 4), name);
                FILE *f = fopen(spv_path, "wb");
                if (!f || fwrite(spv, 4, words, f) != words) fprintf(stderr, "cannot write %s\n", spv_path);
                if (f) fclose(f);
            }
        } else {
            c->translate_failed++;
        }
        free(spv);
    }
    sh_corpus_free(&e);
}

typedef struct corpus_walk {
    const char *dir;
    corpus_stats *stats;
    sh_program *prog;
    uint32_t subdirs;
    char subdir[64][260];
} corpus_walk;

static bool corpus_file(void *user, const char *name, bool is_dir) {
    corpus_walk *w = (corpus_walk *)user;
    size_t len = strlen(name);
    if (is_dir) {
        if (w->subdirs < 64) snprintf(w->subdir[w->subdirs++], sizeof(w->subdir[0]), "%s", name);
        return true;
    }
    if (len < 5 || strcmp(name + len - 4, ".shd")) return true;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", w->dir, name);
    corpus_check(path, name, w->stats, w->prog);
    return true;
}

static int reason_order(const void *a, const void *b) {
    const corpus_reason *x = (const corpus_reason *)a, *y = (const corpus_reason *)b;
    return x->count != y->count ? (x->count < y->count ? 1 : -1) : strcmp(x->text, y->text);
}

static void corpus_print(const char *title, corpus_stats *c, FILE *md) {
    printf("%s: %u entries", title, c->entries);
    if (c->unreadable) printf(" (%u unreadable)", c->unreadable);
    printf("\n  analysis:    %u ok, %u failed\n", c->analyzed, c->analysis_failed);
    printf("  translation: %u ok, %u failed (%llu SPIR-V words)\n", c->translated, c->translate_failed,
           (unsigned long long)c->spirv_words);
    if (c->code_only) printf("  %u shaders whose analysis failed in the game analyze now\n", c->code_only);
    if (c->sites_differ) printf("  %u entries from another analyzer version (sites differ): not translated\n", c->sites_differ);
    qsort(c->reasons, c->reason_count, sizeof(corpus_reason), reason_order);
    if (c->reason_count) printf("  failures by reason:\n");
    for (uint32_t i = 0; i < c->reason_count; i++)
        printf("  %6u  %-70s %s\n", c->reasons[i].count, c->reasons[i].text, c->reasons[i].example);
    if (!md) return;
    fprintf(md, "## %s\n\n| | ok | failed |\n|---|---|---|\n| analysis | %u | %u |\n| translation | %u | %u |\n\n", title,
            c->analyzed, c->analysis_failed, c->translated, c->translate_failed);
    if (c->reason_count) fprintf(md, "| count | reason | example |\n|---|---|---|\n");
    for (uint32_t i = 0; i < c->reason_count; i++)
        fprintf(md, "| %u | %s | %s |\n", c->reasons[i].count, c->reasons[i].text, c->reasons[i].example);
    fprintf(md, "\n");
}

static void corpus_add_stats(corpus_stats *total, const corpus_stats *c) {
    total->entries += c->entries;
    total->unreadable += c->unreadable;
    total->code_only += c->code_only;
    total->analyzed += c->analyzed;
    total->analysis_failed += c->analysis_failed;
    total->sites_differ += c->sites_differ;
    total->translated += c->translated;
    total->translate_failed += c->translate_failed;
    total->spirv_words += c->spirv_words;
    for (uint32_t i = 0; i < c->reason_count; i++) reason_add(total, c->reasons[i].text, c->reasons[i].example, c->reasons[i].count);
}

static int cmd_shaders(const char *dir, const char *report) {
    if (getenv("PS5C_SHADERS_FLOAT_CONTROLS")) sh_set_preserve_inf_nan(true);
    sh_program *p = (sh_program *)malloc(sizeof(sh_program));
    corpus_walk *w = (corpus_walk *)calloc(1, sizeof(corpus_walk));
    corpus_stats total, one;
    memset(&total, 0, sizeof(total));
    memset(&one, 0, sizeof(one));
    FILE *md = report ? fopen(report, "w") : NULL;
    if (!p || !w || (report && !md)) {
        fprintf(stderr, "cannot start (memory or %s)\n", report ? report : "");
        free(p);
        free(w);
        if (md) fclose(md);
        return 1;
    }
    if (md) fprintf(md, "# Shader corpus: %s\n\n", dir);
    log_set_sink(corpus_sink, NULL);
    w->dir = dir;
    w->stats = &one;
    w->prog = p;
    g_corpus = &one;
    if (pal_dir_list(dir, corpus_file, w) != PS5C_OK) {
        fprintf(stderr, "cannot list %s\n", dir);
        log_set_sink(NULL, NULL);
        free(p);
        free(w);
        if (md) fclose(md);
        return 1;
    }
    uint32_t games = 0;
    if (one.entries) {
        corpus_print(dir, &one, md);
        games = 1;
    } else { /* a folder of games: <dir>/<game>/corpus */
        uint32_t n = w->subdirs;
        char (*subs)[260] = (char (*)[260])malloc(sizeof(w->subdir));
        if (subs) memcpy(subs, w->subdir, sizeof(w->subdir));
        for (uint32_t i = 0; subs && i < n; i++) {
            char sub[600];
            snprintf(sub, sizeof(sub), "%s/%s/corpus", dir, subs[i]);
            corpus_stats game;
            memset(&game, 0, sizeof(game));
            w->dir = sub;
            w->stats = &game;
            w->subdirs = 0;
            g_corpus = &game;
            if (pal_dir_list(sub, corpus_file, w) != PS5C_OK || !game.entries) {
                free(game.reasons);
                continue;
            }
            corpus_print(subs[i], &game, md);
            corpus_add_stats(&total, &game);
            free(game.reasons);
            games++;
        }
        free(subs);
        if (games > 1) corpus_print("all games", &total, md);
    }
    log_set_sink(NULL, NULL);
    g_corpus = NULL;
    if (!games) printf("%s: no corpus entries (run a game with PS5C_SHADER_CORPUS=1)\n", dir);
    if (md) {
        fclose(md);
        printf("report: %s\n", report);
    }
    free(one.reasons);
    free(total.reasons);
    free(p);
    free(w);
    return games ? 0 : 1;
}

/* ps5c video <file> <out.pgm>: the luma of a movie's first picture as a PGM image (debugging aid) */
static int cmd_video(const char *in, const char *out) {
    pal_media *m = pal_media_open_file(in);
    if (!m) {
        fprintf(stderr, "%s: cannot decode\n", in);
        return 1;
    }
    pal_media_info info;
    pal_media_get_info(m, &info);
    uint8_t *nv12 = info.width ? (uint8_t *)malloc((size_t)info.width * info.height * 3 / 2) : NULL;
    uint64_t t;
    int n = nv12 ? pal_media_next_video(m, nv12, &t) : -1;
    pal_media_close(m);
    FILE *f = n > 0 ? fopen(out, "wb") : NULL;
    if (f) {
        fprintf(f, "P5\n%u %u\n255\n", info.width, info.height);
        fwrite(nv12, 1, (size_t)info.width * info.height, f);
        fclose(f);
    }
    free(nv12);
    if (!f) fprintf(stderr, "%s: no picture\n", in);
    return f ? 0 : 1;
}

/* ps5c audio <file> <out.wav>: decodes a sound file (snd0.at9, movies) to 16-bit PCM WAV */
static int cmd_audio(const char *in, const char *out) {
    pal_media *m = pal_media_open_file(in);
    if (!m) {
        fprintf(stderr, "%s: cannot decode\n", in);
        return 1;
    }
    pal_media_info info;
    pal_media_get_info(m, &info);
    if (!info.channels || !info.sample_rate) {
        fprintf(stderr, "%s: no audio\n", in);
        pal_media_close(m);
        return 1;
    }
    FILE *f = fopen(out, "wb");
    if (!f) {
        pal_media_close(m);
        return 1;
    }
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, f);
    int16_t pcm[4096 * 2];
    uint64_t frames = 0, t;
    int n;
    while ((n = pal_media_next_audio(m, pcm, 4096, &t)) > 0) {
        fwrite(pcm, 2 * info.channels, (size_t)n, f);
        frames += (uint64_t)n;
    }
    pal_media_close(m);
    uint32_t data = (uint32_t)(frames * 2 * info.channels), rate = info.sample_rate, ch = info.channels;
    uint32_t riff = 36 + data, fmt_size = 16, byte_rate = rate * ch * 2;
    uint16_t pcm_tag = 1, chans = (uint16_t)ch, align = (uint16_t)(ch * 2), bits = 16;
    memcpy(hdr, "RIFF", 4), memcpy(hdr + 4, &riff, 4), memcpy(hdr + 8, "WAVEfmt ", 8), memcpy(hdr + 16, &fmt_size, 4);
    memcpy(hdr + 20, &pcm_tag, 2), memcpy(hdr + 22, &chans, 2), memcpy(hdr + 24, &rate, 4), memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &align, 2), memcpy(hdr + 34, &bits, 2), memcpy(hdr + 36, "data", 4), memcpy(hdr + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 1, 44, f);
    fclose(f);
    printf("%s: %llu frames, %u Hz, %u channels\n", in, (unsigned long long)frames, rate, ch);
    return frames ? 0 : 1;
}

/* The string or number after "key": in a JSON text; first match wins. */
static bool json_field(const char *text, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p || !cap) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    size_t n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && n + 1 < cap; p++) out[n++] = *p;
    } else {
        for (; *p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && n + 1 < cap; p++) out[n++] = *p;
    }
    out[n] = 0;
    return n > 0;
}

static ps5c_err write_file(const char *path, const void *data, uint64_t size) {
    pal_file *f;
    ps5c_err e = pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f);
    if (e) return e;
    uint64_t put = 0;
    e = pal_file_write(f, data, size, &put);
    pal_file_close(f);
    return e == PS5C_OK && put != size ? PS5C_ERR_IO : e;
}

/* The inner file system of a fake package: lists it, and with dir extracts
 * it there (PS5C_PKG_ONLY=<path> extracts just that file). */
static int cmd_pkg_inner(const char *path, const pkg_info *info, const char *dir) {
    pkg_inner *in;
    uint64_t t0 = pal_time_ns();
    ps5c_err e = pkg_inner_open(path, info, &in);
    if (e) {
        printf("inner PFS:   cannot open (%s)\n", ps5c_err_str(e));
        return 1;
    }
    uint64_t files = 0, bytes = 0, kblocks = 0, holes = 0;
    for (uint32_t i = 0; i < in->nblocks; i++)
        kblocks += in->blocks[i].kind == PKG_SPAN_KRAKEN, holes += in->blocks[i].kind == PKG_SPAN_ZERO;
    for (uint32_t i = 1; i < in->count; i++)
        if (!in->nodes[i].dir) files++, bytes += in->nodes[i].size;
    printf("naps layout: %u files, %u ublocks, %u cblock records -> %u spans (%llu kraken, %llu holes)\n",
           in->naps_files, in->naps_ublocks, in->naps_cblocks, in->nblocks, (unsigned long long)kblocks,
           (unsigned long long)holes);
    printf("inner PFS:   %llu bytes, superblock 0x%llx, block 0x%llx, %u nodes, %llu files, %.2f GB, opened in %.1f s\n",
           (unsigned long long)in->mount_size, (unsigned long long)in->superblock, (unsigned long long)in->block_size,
           in->count, (unsigned long long)files, bytes / 1e9, (pal_time_ns() - t0) / 1e9);
    const char *dbg = getenv("PS5C_PKG_DEBUG");
    if (dbg && *dbg == '1') {
        static const char *kinds[] = {"stored", "kraken", "hole", "zlib"};
        for (uint32_t i = 0; i < in->nblocks && i < 6; i++) {
            const pkg_inner_block *b = &in->blocks[i];
            printf("  span %u: image 0x%llx -> disk 0x%llx, %u -> %u bytes, first %u, flags %02x, %s\n", i,
                   (unsigned long long)b->uoff, (unsigned long long)b->disk, b->clen, b->ulen, b->first, b->flags,
                   kinds[b->kind]);
        }
        /* decode a spread of compressed spans */
        uint32_t tried = 0, good = 0;
        uint64_t bad0 = in->bad_blocks;
        for (uint32_t i = 0; i < in->nblocks && tried < 200; i += in->nblocks / 150 + 1) {
            if (in->blocks[i].kind != PKG_SPAN_KRAKEN) continue;
            uint8_t one;
            pkg_inner_flush(in);
            if (pkg_inner_read(in, &one, 1, in->blocks[i].uoff)) continue;
            tried++;
            if (in->last_status == KRAKEN_OK) good++;
            else if (tried - good <= 5) {
                const pkg_inner_block *b = &in->blocks[i];
                printf("  undecoded span %u: disk 0x%llx, %u -> %u, first %u, flags %02x, status %d\n", i,
                       (unsigned long long)b->disk, b->clen, b->ulen, b->first, b->flags, in->last_status);
            }
        }
        printf("kraken test: %u of %u sampled spans decode\n", good, tried);
        in->bad_blocks = bad0;
    }
    if (dbg && *dbg == '2') {
        /* decode every span, writing nothing */
        uint64_t t1 = pal_time_ns(), bad0 = in->bad_blocks;
        for (uint32_t i = 0; i < in->nblocks; i++) {
            pkg_inner_flush(in);
            uint8_t one;
            if (pkg_inner_read(in, &one, 1, in->blocks[i].uoff)) {
                printf("verify: read failed at span %u\n", i);
                break;
            }
        }
        printf("verify:      %u spans, %llu did not decode, %.1f s\n", in->nblocks,
               (unsigned long long)(in->bad_blocks - bad0), (pal_time_ns() - t1) / 1e9);
    }
    if (in->tree_err) printf("inner tree:  cannot load (%s)\n", ps5c_err_str(in->tree_err));
    uint32_t shown = 0;
    for (uint32_t i = 1; i < in->count && shown < 60; i++) {
        const pkg_inner_node *n = &in->nodes[i];
        if (n->parent != in->uroot && in->nodes[n->parent].parent != in->uroot) continue;
        char p[512];
        pkg_inner_path(in, i, p, sizeof p);
        if (!p[0]) continue;
        printf("  %s%-40s %llu\n", n->dir ? "[dir] " : "", p, (unsigned long long)n->size);
        shown++;
    }
    const char *probes[] = {"eboot.bin", "sce_sys/param.json"};
    for (size_t k = 0; k < 2 && !in->tree_err; k++) {
        uint32_t ni = pkg_inner_lookup(in, probes[k]);
        uint8_t head[32];
        if (ni == UINT32_MAX || in->nodes[ni].size < sizeof head) continue;
        if (pkg_inner_read_file(in, &in->nodes[ni], head, sizeof head, 0)) continue;
        printf("  %s head:", probes[k]);
        for (int j = 0; j < 32; j++) printf("%s%02x", j % 4 ? "" : " ", head[j]);
        printf("\n");
    }
    int rc = 0;
    if (dir) {
        const char *only = getenv("PS5C_PKG_ONLY");
        size_t bufsz = 8u << 20;
        uint8_t *buf = malloc(bufsz);
        uint64_t done = 0, written = 0, next_report = 1ull << 30;
        t0 = pal_time_ns();
        pal_mkdir(dir);
        for (uint32_t i = 1; buf && i < in->count && !rc; i++) {
            const pkg_inner_node *n = &in->nodes[i];
            char rel[1024], out[1400];
            pkg_inner_path(in, i, rel, sizeof rel);
            if (!rel[0] || i == in->uroot) continue;
            bool under = false;
            for (uint32_t c = i; c; c = in->nodes[c].parent)
                if (c == in->uroot) under = true;
            if (!under) continue;
            if (only && *only && strcmp(only, rel)) {
                /* keep the directories leading to it */
                size_t l = strlen(rel);
                if (!n->dir || strncmp(only, rel, l) || only[l] != '/') continue;
            }
            snprintf(out, sizeof out, "%s/%s", dir, rel);
            if (n->dir) { pal_mkdir(out); continue; }
            pal_file *f;
            if (pal_file_open(out, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f)) {
                fprintf(stderr, "cannot write %s\n", out);
                rc = 1;
                break;
            }
            for (uint64_t off = 0; off < n->size && !rc;) {
                uint64_t take = n->size - off < bufsz ? n->size - off : bufsz, got = 0;
                if (pkg_inner_read_file(in, n, buf, take, off) || pal_file_write(f, buf, take, &got) || got != take) {
                    fprintf(stderr, "extract failed: %s\n", rel);
                    rc = 1;
                }
                off += take;
                done += take;
                if (done >= next_report) {
                    printf("  ... %.1f GB, %.0f MB/s\n", done / 1e9, done / 1e6 / ((pal_time_ns() - t0) / 1e9));
                    fflush(stdout);
                    next_report += 1ull << 30;
                }
            }
            pal_file_close(f);
            written++;
        }
        free(buf);
        printf("extracted:   %llu files, %.2f GB in %.1f s to %s\n", (unsigned long long)written, done / 1e9,
               (pal_time_ns() - t0) / 1e9, dir);
    }
    if (in->bad_blocks) {
        printf("WARNING:     %llu compressed blocks did not decode (read as zeros)\n", (unsigned long long)in->bad_blocks);
        rc = 1;
    }
    pkg_inner_close(in);
    return rc;
}

/* ps5c pkg <file.pkg> [dir]: what the package is; with dir, its plaintext
 * metadata (param.json, icons, ...) goes to dir/sce_sys/. */
static int cmd_pkg(const char *path, const char *dir) {
    pkg_info info;
    ps5c_err e = pkg_inspect(path, &info);
    if (e) {
        fprintf(stderr, "%s: %s\n", path, e == PS5C_ERR_UNSUPPORTED ? "not a PS5/PS4 package" : ps5c_err_str(e));
        return 1;
    }
    printf("package:     %s (%s)\n", pkg_kind_name(info.kind),
           info.kind != PKG_PS5 ? "?" : info.retail ? "retail" : "fake/debug");
    if (info.kind == PKG_PS5) printf("FIH flags:   %08x\n", info.fih_flags);
    printf("content id:  %s\n", info.content_id[0] ? info.content_id : "?");
    if (info.kind == PKG_PS5)
        printf("outer PFS:   0x%llx, %llu bytes, superblock 0x%llx, %s\n", (unsigned long long)info.pfs_offset,
               (unsigned long long)info.pfs_size, (unsigned long long)info.superblock_offset,
               info.outer_encrypted ? "encrypted" : "plaintext");
    const pkg_entry *pj = pkg_find(&info, "param.json");
    uint8_t *param = NULL;
    uint64_t psize = 0;
    if (pj && !pkg_read_entry(path, pj, &param, &psize)) {
        static const char *keys[] = {"titleId", "titleName", "contentVersion", "masterVersion", "applicationCategoryType",
                                     "requiredSystemSoftwareVersion"};
        char v[256];
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            if (json_field((const char *)param, keys[i], v, sizeof v)) printf("%-12s %s\n", keys[i], v);
    }
    free(param);
    printf("entries:     %u\n", info.nentries);
    for (uint32_t i = 0; i < info.nentries; i++) {
        const pkg_entry *x = &info.entries[i];
        printf("  %06x %08x %10llu %s%s\n", x->id, x->flags1, (unsigned long long)x->size, x->name,
               x->flags1 & PKG_ENTRY_ENCRYPTED ? " (encrypted)" : "");
    }
    int rc = 0;
    if (dir) {
        char out[1024];
        snprintf(out, sizeof out, "%s", dir);
        pal_mkdir(out);
        snprintf(out, sizeof out, "%s/sce_sys", dir);
        pal_mkdir(out);
        unsigned written = 0;
        for (uint32_t i = 0; i < info.nentries; i++) {
            const pkg_entry *x = &info.entries[i];
            uint8_t *data;
            uint64_t size;
            if (!x->name[0] || strchr(x->name, '/') || strchr(x->name, '\\') || x->name[0] == '.') continue;
            if (pkg_read_entry(path, x, &data, &size)) continue;
            snprintf(out, sizeof out, "%s/sce_sys/%s", dir, x->name);
            if (write_file(out, data, size)) { fprintf(stderr, "cannot write %s\n", out); rc = 1; }
            else written++;
            free(data);
        }
        printf("metadata:    %u files in %s/sce_sys\n", written, dir);
    }
    const char *dbg = getenv("PS5C_PKG_DEBUG");
    if (info.kind == PKG_PS5 && dbg && *dbg == '1' && info.block_size) {
        /* blocks around the superblock and the first ones: zero count and first bytes */
        uint64_t bs = info.block_size, sb = (info.superblock_offset - info.pfs_offset) / bs;
        uint64_t picks[] = {0, 1, 2, sb - 2, sb - 1, sb, sb + 1, sb + 2, sb + 3, info.pfs_blocks - 1};
        uint8_t *b = malloc(bs);
        pal_file *pf;
        printf("pfs blocks:  %llu of 0x%x bytes, superblock at block %llu, seed", (unsigned long long)info.pfs_blocks,
               info.block_size, (unsigned long long)sb);
        for (int i = 0; i < 16; i++) printf(" %02x", info.seed[i]);
        printf("\n");
        if (b && !pal_file_open(path, PAL_FILE_READ, &pf)) {
            for (size_t k = 0; k < sizeof picks / sizeof *picks; k++) {
                uint64_t got = 0, n = picks[k];
                if (n >= info.pfs_blocks || pal_file_pread(pf, b, bs, info.pfs_offset + n * bs, &got) || got != bs) continue;
                size_t zeros = 0;
                for (uint64_t i = 0; i < bs; i++) zeros += !b[i];
                printf("  block %6llu zeros %5.1f%%:", (unsigned long long)n, 100.0 * zeros / bs);
                for (int i = 0; i < 48; i++) printf("%s%02x", i % 4 ? "" : " ", b[i]);
                printf("\n");
            }
            pal_file_close(pf);
        }
        free(b);
    }
    if (info.kind == PKG_PS5 && info.outer_encrypted) {
        ps5c_err ke = pkg_find_key(path, &info, getenv("PS5C_PKG_PASSCODE"));
        printf("outer read:  %s\n", ke == PS5C_OK ? (info.outer_plain ? "stored as plaintext" : "fake key opens it")
                                    : ke == PS5C_ERR_NOT_FOUND ? "needs a key we do not have" : ps5c_err_str(ke));
        if (ke == PS5C_ERR_NOT_FOUND && info.retail)
            printf("game files:  retail package, its file system needs the console's keys; install it on a PS5 and dump it\n");
        else
            printf("game files:  %s\n", ke == PS5C_ERR_NOT_FOUND ? "the fake key and passcode do not open it (try PS5C_PKG_PASSCODE)"
                                                              : "inner file system below");
        pkg_outer o;
        ps5c_err oe = ke == PS5C_OK && info.outer_plain ? pkg_outer_open(path, &info, &o) : PS5C_ERR_UNSUPPORTED;
        if (ke == PS5C_OK && info.outer_plain && oe) printf("outer PFS:   cannot open (%s)\n", ps5c_err_str(oe));
        if (!oe) {
            printf("outer files: %u nodes, mode 0x%x, block 0x%llx\n", o.fs.count, o.fs.mode, (unsigned long long)o.fs.block_size);
            for (uint32_t i = 1; i < o.fs.count && i < 200; i++) {
                const pfs_node *n = &o.fs.nodes[i];
                char p[512];
                pfs_path(&o.fs, i, p, sizeof p);
                if (i == o.fs.uroot) snprintf(p, sizeof p, "(uroot)");
                else if (n->parent == 0 && i != o.fs.uroot) snprintf(p, sizeof p, "(super-root)/%s", n->name);
                printf("  %s%-32s size %llu stored %llu flags 0x%x blocks %llu at %lld%s\n", n->dir ? "[dir] " : "", p,
                       (unsigned long long)n->size, (unsigned long long)n->compressed_size, n->flags,
                       (unsigned long long)n->nblocks, (long long)n->start, n->blocks ? " (scattered)" : "");
                if (!n->dir && n->nblocks) {
                    uint8_t head[64];
                    if (!pfs_read(&o.fs, n, head, sizeof head, 0)) {
                        printf("    head:");
                        for (int k = 0; k < 64; k++) printf("%s%02x", k % 4 ? "" : " ", head[k]);
                        printf("\n");
                    }
                }
            }
            pkg_outer_close(&o);
            if (cmd_pkg_inner(path, &info, dir)) rc = 1;
        }
    } else if (info.kind == PKG_PS4) {
        printf("pfs image:   0x%llx+0x%llx, block 0x%x, %s\n", (unsigned long long)info.pfs_offset,
               (unsigned long long)info.pfs_size, info.block_size, info.outer_encrypted ? "encrypted" : "plain");
        ps5c_err ke = pkg_ps4_find_key(path, &info);
        if (ke == PS5C_ERR_NOT_FOUND || ke == PS5C_ERR_INVALID)
            printf("game files:  encrypted; needs the PS4 fake-package RSA keys in %s (fake_n, fake_d, dk3_n, dk3_d = hex)\n",
                   pkg_ps4_keys_path());
        else if (ke)
            printf("game files:  %s\n", ps5c_err_str(ke));
        else if (cmd_pkg_inner(path, &info, dir))
            rc = 1;
    } else {
        printf("game files:  extraction not implemented yet\n");
    }
    pkg_info_free(&info);
    return rc;
}

#ifdef _WIN32
/* ps5c --list-cameras, for the launcher: "camera<TAB>name" lines, then "ps4<TAB>absent|firmware|driver|ready" */
static int cmd_list_cameras(void) {
    static const char *k_state[] = {"absent", "firmware", "driver", "ready"};
    char names[16][128];
    uint32_t n = pal_camera_list(names, 16);
    for (uint32_t i = 0; i < n && i < 16; i++) printf("camera\t%s\n", names[i]);
    ps4_camera_state st = ps4_camera_query();
    printf("ps4\t%s\n", (unsigned)st < 4 ? k_state[st] : "absent");
    return 0;
}

/* ps5c camera [firmware.bin]: the PS4 Camera's state; with the user's firmware file, loads it */
static int cmd_camera(const char *firmware) {
    ps4_camera_state st = ps4_camera_query();
    printf("PS4 Camera: %s\n", ps4_camera_state_name(st));
    if (!firmware) return st == PS4_CAMERA_ABSENT ? 1 : 0;
    if (st == PS4_CAMERA_READY) return 0;
    FILE *f = fopen(firmware, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", firmware); return 1; }
    static uint8_t fw[4u << 20];
    size_t n = fread(fw, 1, sizeof(fw), f);
    fclose(f);
    if (!n || n == sizeof(fw)) { fprintf(stderr, "%s: unexpected size\n", firmware); return 1; }
    int rc = ps4_camera_load_firmware(fw, n);
    if (rc) return 1;
    for (int i = 0; i < 50 && (st = ps4_camera_query()) != PS4_CAMERA_READY; i++) pal_sleep_ns(100000000ull);
    printf("PS4 Camera: %s\n", ps4_camera_state_name(st));
    return st == PS4_CAMERA_READY ? 0 : 1;
}
#endif

static int cmd_disasm(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    uint32_t *code = NULL;
    size_t count = 0, cap = 0;
    uint32_t w;
    while (fread(&w, 4, 1, f) == 1) {
        if (count == cap) {
            cap = cap ? cap * 2 : 1024;
            uint32_t *grown = (uint32_t *)realloc(code, cap * 4);
            if (!grown) {
                free(code);
                fclose(f);
                return 1;
            }
            code = grown;
        }
        code[count++] = w;
    }
    fclose(f);
    for (size_t pos = 0; pos < count;) {
        rdna2_inst in;
        char text[256];
        bool decoded = rdna2_decode(code + pos, (uint32_t)(count - pos), &in);
        bool exact = decoded && rdna2_disasm(&in, text, sizeof(text));
        uint32_t n = decoded ? in.words : 1;
        char hex[128];
        int h = 0;
        for (uint32_t i = 0; i < n && h < (int)sizeof(hex) - 10; i++) h += snprintf(hex + h, sizeof(hex) - (size_t)h, "%08x ", code[pos + i]);
        printf("%s%04zx: %-36s %s\n", exact ? " " : "!", pos * 4, hex, decoded ? text : "<unknown>");
        pos += n;
    }
    free(code);
    return 0;
}

/* Translation check: default site shapes, writes <path>.spv */
static int cmd_spirv(const char *path, const char *stage) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    static uint32_t code[0x10000];
    size_t count = fread(code, 4, PS5C_ARRAY_COUNT(code), f);
    fclose(f);
    sh_program *p = (sh_program *)malloc(sizeof(sh_program));
    if (!p) return 1;
    int rc = 1;
    if (sh_analyze(code, (uint32_t)count, 0, p)) {
        sh_stage_info info;
        memset(&info, 0, sizeof(info));
        info.stage = !strcmp(stage, "ps") ? SH_STAGE_PS : !strcmp(stage, "cs") ? SH_STAGE_CS : SH_STAGE_VS;
        info.vertex_id_vgpr = 5;
        info.instance_id_vgpr = 8;
        for (uint32_t i = 0; i < 32; i++) info.ps_attr_location[i] = (uint8_t)i;
        info.local_size[0] = 64;
        info.cs_tgid_sgpr = 12;
        info.cs_tgid_enable = 1;
        info.cs_tid_components = 1;
        sh_site_shape shapes[SH_MAX_SITES];
        memset(shapes, 0, sizeof(shapes));
        for (uint32_t i = 0; i < SH_MAX_SITES; i++) {
            shapes[i].buf_format = 77;
            shapes[i].buf_stride = 16;
            for (int k = 0; k < 4; k++) shapes[i].dst_sel[k] = (uint8_t)(4 + k);
        }
        uint32_t *spv = NULL;
        size_t words = 0;
        if (sh_translate(p, &info, shapes, 0, &spv, &words)) {
            char out[1024];
            snprintf(out, sizeof(out), "%s.spv", path);
            FILE *o = fopen(out, "wb");
            if (o) {
                fwrite(spv, 4, words, o);
                fclose(o);
                printf("%s: %u instructions, %u sites -> %s (%zu words)\n", path, p->count, p->site_count, out, words);
                rc = 0;
            }
            free(spv);
        }
    }
    free(p);
    return rc;
}

/* Console libraries the emulator runs as they are (LLE): a game that needs
 * one cannot start without its file in the system folder. */
static const char *const k_required_system_libs[] = {"libSceAgc", "libSceAmpr", "libScePsml"};
#define EXIT_MISSING_SYSTEM_LIB 3

/* Logs "missing system library: <name>.sprx" for each one the game needs and
 * no folder had; false if any. The launcher shows these lines to the user. */
static bool check_system_libs(const module_set *ms) {
    bool ok = true;
    for (size_t k = 0; k < PS5C_ARRAY_COUNT(k_required_system_libs); k++) {
        const char *lib = k_required_system_libs[k];
        if (module_set_find_by_name(ms, lib) || module_set_get_policy(ms, lib) == LIB_POLICY_HLE) continue;
        bool needed = false;
        for (size_t i = 0; i < ms->modules.len && !needed; i++) {
            const sce_image *img = &ms->modules.data[i]->img;
            for (uint32_t n = 0; n < img->needed_module_count && !needed; n++) needed = !strcmp(img->needed_modules[n].name, lib);
        }
        if (needed) {
            LOG_E(CORE, "missing system library: %s.sprx (copy the console's system libraries into the system folder)", lib);
            ok = false;
        }
    }
    return ok;
}

static int cmd_run(const options *o, const nid_db *db) {
#ifdef _WIN32
    core_session_stage(4);
#endif
    LOG_I(CORE, "ZeraPS5 %s%s build %d (%s), %s", VERSION_FULL, VERSION_DIRTY, PS5C_BUILD_NUMBER, PS5C_BUILD_DATE,
          pal_os_name());
    char caption[64];
    snprintf(caption, sizeof(caption), "ZeraPS5 %s build %d", PS5C_VERSION_STRING, PS5C_BUILD_NUMBER);
    video_out_set_caption(caption);
    module_set ms;
    if (!setup_modules(o, &ms)) {
        return 1;
    }
    if (!check_system_libs(&ms)) {
        module_set_free(&ms);
        return EXIT_MISSING_SYSTEM_LIB;
    }
    settings_apply_keyboard();
    /* Sony's PSP emulator (the PSP classics) maps the PSP memory and its mirrors
     * onto the same physical pages at fixed addresses from 0x80_0000_0000 */
    const loaded_module *main_module = module_set_main(&ms);
    if (main_module && main_module->name && !strcmp(main_module->name, "PSPHD_Master") &&
        gmem_set_shared_area_base(0x1000000000ull) != PS5C_OK) {
        LOG_W(MEM, "cannot move the shared-memory area: PSP memory mirrors will diverge");
    }
    const bool longshot = game_longshot_allocator_rva(main_module) != 0;
    /* t1x places direct-memory views and CPU/GPU aliases at 0x10_0000_0000.
     * Leaving these below the placeholder area makes independent private
     * copies instead of shared physical pages. */
    if(main_module && main_module->name && !strcmp(main_module->name,"t1x") &&
       gmem_set_shared_area_base(0x1000000000ull)!=PS5C_OK)
        LOG_W(MEM,"cannot move the shared-memory area for t1x's physical aliases");
    /* Unreal reserves its whole heap at 0x1000000000 (768 GiB in Silent Hill Townfall and
     * SpongeBob Titans of the Tide): placed there it stays below 1 TiB, where the GPU's
     * 40-bit render target addresses reach */
    size_t mn = main_module && main_module->name ? strlen(main_module->name) : 0;
    if (mn > 13 && !strcmp(main_module->name + mn - 13, "-PS5-Shipping") && !getenv("PS5C_LOW_SHARED_AREA") &&
        gmem_set_shared_area_base(0x1000000000ull) != PS5C_OK)
        LOG_W(MEM, "cannot move the shared-memory area for Unreal's heap");
    runtime_options ro;
    memset(&ro, 0, sizeof(ro));
    char compatibility_title[64]={0};
    bool quake3=false;
    if(settings_param_string("titleId",compatibility_title,sizeof(compatibility_title)) &&
        ((quake3=!strcmp(compatibility_title,"PPSA99319")) || !strcmp(compatibility_title,"PPSA99902"))) {
        /* Quake3e's bundled RADV uses address32_hi=2 for shader/ring memory.
         * Keep this measured homebrew profile isolated from other titles.
         * UnleashedRecomp (PPSA99902) bundles the same RADV port: its device
         * probe maps direct memory with no address and wants it in 8-12 GiB. */
        ro.direct_memory_window=true;
        if(gmem_set_shared_area_base(0x280000000ull)!=PS5C_OK)
            LOG_W(MEM,"cannot reserve the PS5 shader address32 window");
        else if(quake3) LOG_I(MEM,"Quake III homebrew: PS5 shader address32 memory window enabled");
        else LOG_I(MEM,"Unleashed Recompiled homebrew: PS5 shader address32 memory window enabled");
    }
    if (longshot) {
        /* The bundled PS5 RADV probes a low address32 pool as well as the
         * high guest window. The same placeholder split supports both. */
        ro.direct_memory_window = true;
        if (gmem_set_shared_area_base(0x280000000ull) != PS5C_OK)
            LOG_W(MEM, "cannot reserve Longshot's shared memory and RADV address32 window");
    }
    ro.stop_on_missing = o->stop_on_missing;
    ro.trace_hle = o->trace_hle;
    ro.names = db;
    ps5_runtime rt;
    int rc = 1;
    int64_t exit_code = 0;
    ps5c_err err = runtime_init(&rt, &ms, &ro);
    const char *stage = "init";
    if (err == PS5C_OK) {
        stage = "link";
        err = runtime_link(&rt);
    }
    if (err == PS5C_OK) {
        stage = "run";
        video_out_intro(&rt);
        err = runtime_run_main(&rt, &exit_code);
    }
    if (err == PS5C_OK) {
#ifdef _WIN32
        core_session_guest(exit_code);
#endif
        printf("guest exit code: %lld (0x%llx)\n", (long long)exit_code, (unsigned long long)exit_code);
        rc = 0;
    } else {
        LOG_E(CORE, "runtime %s failed: %s", stage, ps5c_err_str(err));
    }
    if (err == PS5C_OK && runtime_running_threads(&rt) > 0) {
        /* Other guest threads still run on guest memory: end the process now
         * instead of freeing it under them. */
        LOG_I(CORE, "%u guest threads still running at exit", runtime_running_threads(&rt));
        fflush(stdout);
        pal_exit_process(rc);
    }
    runtime_free(&rt);
    module_set_free(&ms);
    return rc;
}

static int cmd_analyze(const options *o, const nid_db *db) {
    module_set ms;
    int rc = 1;
    if (!setup_modules(o, &ms)) {
        return 1;
    }

    json_writer w;
    json_init(&w, true);
    profile_summary sum;
    if (profile_write_static(&w, &ms, db, &sum) == PS5C_OK) {
        size_t len;
        const char *doc = json_result(&w, &len);
        pal_file *f;
        if (pal_file_open(o->output, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK) {
            ps5c_err err = pal_file_write(f, doc, len, NULL);
            pal_file_close(f);
            if (err == PS5C_OK) {
                printf("profile written to %s\n", o->output);
                printf("  modules loaded : %u (missing: %u)\n", sum.modules_loaded, sum.modules_missing);
                printf("  unique imports : %u (%u with known names)\n", sum.imports_unique, sum.imports_named);
                printf("  providers      : %u HLE, %u LLE, %u missing\n", sum.by_provider[PROVIDER_HLE],
                       sum.by_provider[PROVIDER_LLE], sum.by_provider[PROVIDER_MISSING]);
                rc = 0;
            }
        }
        if (rc != 0) {
            LOG_E(RECORDER, "cannot write '%s'", o->output);
        }
    }
    json_free(&w);
    module_set_free(&ms);
    return rc;
}

#ifdef _WIN32
static FILE *launcher_records;
static log_record_file launcher_record_output;
static void flush_launcher_records(void) {
    log_record_file_finish(&launcher_record_output);
}
static void start_launcher_records(void) {
    const wchar_t *records = _wgetenv(L"PS5C_LAUNCHER_RECORDS");
    if (!records || !*records) return;
    size_t length = wcslen(records) + 8;
    wchar_t *path = calloc(length, sizeof(*path));
    FILE *status = NULL;
    if (path) { swprintf(path, length, L"%ls.status", records); status = _wfopen(path, L"wb"); free(path); }
    launcher_records = _wfopen(records, L"wb");
    log_record_file_init(&launcher_record_output, launcher_records, status, 64u * 1024u * 1024u);
    const wchar_t *session=_wgetenv(L"PS5C_LAUNCHER_SESSION");
    if(session && wcslen(session)==32 && wcslen(records)<MAX_PATH) {
        core_session=(log_session_meta){.role=2,.build=PS5C_BUILD_NUMBER,.stage=1};
        bool ascii=true;for(unsigned n=0;n<32;n++){if(session[n]>127)ascii=false;core_session.id[n]=(char)session[n];}
        snprintf(core_session.version,sizeof(core_session.version),"%s%s",VERSION_FULL,VERSION_DIRTY);
        const wchar_t *slash=wcsrchr(records,L'\\');
        if(ascii && log_session_id_valid(core_session.id) && slash &&
           swprintf(core_meta_path,MAX_PATH,L"%.*ls\\core.meta",(int)(slash-records),records)>=0) {
            char header[40];snprintf(header,sizeof(header),"ZSID1 %s\n",core_session.id);
            log_record_file_sink(&launcher_record_output,header,39);
            core_session_known=report_session_meta_save(core_meta_path,&core_session);
        }
    }
    log_set_record_sink(log_record_file_sink, &launcher_record_output);
    log_set_record_reject_sink(log_record_file_rejected, &launcher_record_output);
    atexit(flush_launcher_records);
}
#endif
int main(int argc, char **argv) {
#ifdef _WIN32
    start_launcher_records();
#endif
    if (pal_init() != PS5C_OK) {
        fprintf(stderr, "failed to initialize platform layer\n");
        return 1;
    }
#ifdef _WIN32
    core_session_stage(2);
#endif
    pal_utf8_args(&argc, &argv);
    if(argc==2 && (!strcmp(argv[1],"--find-system-modules") || !strcmp(argv[1],"--collect-system-modules"))) {
        bool collect=!strcmp(argv[1],"--collect-system-modules");
        module_path_result location;
        if(!module_path_resolve(NULL,"system/modules",collect,&location)){pal_shutdown();return 3;}
        printf("%s\n",location.directory);pal_shutdown();return location.copy_failed?4:0;
    }
    log_set_sink(pal_log_sink, NULL);
    log_set_clock(pal_time_ns);
    /* Grab the guest address range before any large host allocation can. */
    if (gmem_reserve_early() != PS5C_OK) {
        LOG_W(MEM, "guest address range already in use at startup; 'load'/'run' may fail");
    }

    options o;
    if (!parse_args(argc, argv, &o)) {
        return 2;
    }

#ifdef _WIN32
    core_session_stage(3);
#endif
    int rc = 0;
    if (!o.command || strcmp(o.command, "--help") == 0 || strcmp(o.command, "help") == 0) {
        print_usage();
    } else if (strcmp(o.command, "--version") == 0) {
        printf("%s%s (build %d, %s)\n", VERSION_FULL, VERSION_DIRTY, PS5C_BUILD_NUMBER, PS5C_BUILD_DATE);
    } else if (strcmp(o.command, "--list-gpus") == 0) {
        renderer_gpu gpus[16];
        uint32_t n = renderer_list_gpus(gpus, 16);
        for (uint32_t i = 0; i < n && i < 16; i++) printf("%u\t%s\t%s\n", i, gpus[i].discrete ? "dedicated" : "integrated", gpus[i].name);
        rc = n ? 0 : 1;
    } else if (strcmp(o.command, "--host-info") == 0) {
        LOG_I(CORE, "PS5C %s on %s", PS5C_VERSION_STRING, pal_os_name());
        LOG_I(CORE, "host page size: %llu bytes, allocation granularity: %llu bytes",
              (unsigned long long)pal_page_size(), (unsigned long long)pal_alloc_granularity());
    } else if (strcmp(o.command, "audio") == 0) {
        rc = o.input && o.input2 ? cmd_audio(o.input, o.input2) : 2;
    } else if (strcmp(o.command, "video") == 0) {
        rc = o.input && o.input2 ? cmd_video(o.input, o.input2) : 2;
#ifdef _WIN32
    } else if (strcmp(o.command, "camera") == 0) {
        rc = cmd_camera(o.input);
    } else if (strcmp(o.command, "--list-cameras") == 0) {
        rc = cmd_list_cameras();
#endif
    } else if (strcmp(o.command, "disasm") == 0) {
        rc = o.input ? cmd_disasm(o.input) : 2;
    } else if (strcmp(o.command, "pkg") == 0) {
        rc = o.input ? cmd_pkg(o.input, o.input2) : 2;
    } else if (strcmp(o.command, "shaders") == 0) {
        rc = o.input ? cmd_shaders(o.input, o.input2) : 2;
    } else if (strcmp(o.command, "spirv") == 0) {
        rc = o.input ? cmd_spirv(o.input, o.input2 ? o.input2 : "vs") : 2;
    } else if (strcmp(o.command, "info") == 0 || strcmp(o.command, "analyze") == 0 ||
               strcmp(o.command, "load") == 0 || strcmp(o.command, "run") == 0) {
        if (!o.input) {
            fprintf(stderr, "missing input file\n\n");
            print_usage();
            rc = 2;
        } else {
            char image_eboot[1024];
            if (exfat_image_path(o.input)) {
                ps5c_err e = exfat_game_path(o.input, image_eboot, sizeof(image_eboot));
                if (e != PS5C_OK) {
                    LOG_E(LOADER, "cannot open exFAT game: %s (%s)", o.input, ps5c_err_str(e));
                    pal_shutdown(); return 1;
                }
                LOG_I(LOADER, "direct exFAT game: %s", image_eboot);
                o.input = image_eboot;
            }
            nid_db db;
            nid_db_init(&db);
            if (!load_nid_db(&db, o.nid_db_path, argv[0])) {
                rc = 1;
            } else {
                if (strcmp(o.command, "info") == 0) rc = cmd_info(&o, &db);
                else if (strcmp(o.command, "analyze") == 0) rc = cmd_analyze(&o, &db);
                else if (strcmp(o.command, "load") == 0) rc = cmd_load(&o);
                else rc = cmd_run(&o, &db);
            }
            nid_db_free(&db);
        }
    } else {
        fprintf(stderr, "unknown command '%s'\n\n", o.command);
        print_usage();
        rc = 2;
    }

    pal_shutdown();
    return rc;
}
