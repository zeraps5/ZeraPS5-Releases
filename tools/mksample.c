/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Writes a small synthetic PS5 "game" to <dir>: eboot.bin plus
 * sce_module/libSceNgs2.sprx. Used by the end-to-end CLI test and as an
 * example input for `ps5c info` / `ps5c analyze`. */
#include <stdio.h>
#include <stdlib.h>

#include "guest/elf_sce.h"
#include "pal/pal.h"
#include "support/sce_builder.h"

static int write_file(const char *path, const uint8_t *data, size_t size) {
    pal_file *f;
    if (pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) != PS5C_OK) {
        fprintf(stderr, "cannot create %s (does the directory exist?)\n", path);
        return 1;
    }
    ps5c_err err = pal_file_write(f, data, size, NULL);
    pal_file_close(f);
    return err == PS5C_OK ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mksample <dir>   (<dir>/sce_module must exist)\n");
        return 2;
    }
    pal_init();

    sce_builder b;
    sb_init(&b, ET_SCE_DYNEXEC);
    b.original_filename = "eboot.bin";
    sb_add_needed(&b, "libkernel", 1);
    sb_add_needed(&b, "libSceVideoOut", 2);
    sb_add_needed(&b, "libSceNgs2", 3);
    sb_add_import_lib(&b, "libkernel", 1);
    sb_add_import_lib(&b, "libSceVideoOut", 2);
    sb_add_import_lib(&b, "libSceNgs2", 3);
    sb_import(&b, "sceKernelUsleep", 1, 1, STT_FUNC);
    sb_import(&b, "__stack_chk_guard", 1, 1, STT_OBJECT);
    sb_import(&b, "__stack_chk_fail", 1, 1, STT_FUNC);
    sb_import(&b, "sceVideoOutOpen", 2, 2, STT_FUNC);
    sb_import(&b, "sceNgs2SystemCreate", 3, 3, STT_FUNC);
    size_t size;
    uint8_t *eboot = sb_build(&b, &size);

    sb_init(&b, ET_SCE_DYNAMIC);
    sb_set_module_info(&b, "libSceNgs2", 1);
    sb_add_export_lib(&b, "libSceNgs2", 1);
    sb_export(&b, "sceNgs2SystemCreate", 1, 1, 0x100);
    size_t mod_size;
    uint8_t *mod = sb_build(&b, &mod_size);

    char path[1024];
    int rc = !eboot || !mod;
    snprintf(path, sizeof(path), "%s/eboot.bin", argv[1]);
    rc = rc || write_file(path, eboot, size);
    snprintf(path, sizeof(path), "%s/sce_module/libSceNgs2.sprx", argv[1]);
    rc = rc || write_file(path, mod, mod_size);
    free(eboot);
    free(mod);
    if (rc == 0) {
        printf("sample written to %s\n", argv[1]);
    }
    return rc;
}
