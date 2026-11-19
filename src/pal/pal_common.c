/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PAL helpers built only on top of the PAL interface itself. */
#include <stdlib.h>

#include "pal/pal.h"

ps5c_err pal_file_read_all(const char *path, uint8_t **data_out, uint64_t *size_out) {
    pal_file *f;
    ps5c_err err = pal_file_open(path, PAL_FILE_READ, &f);
    if (err != PS5C_OK) {
        return err;
    }

    uint64_t size = 0;
    err = pal_file_size(f, &size);
    if (err == PS5C_OK && size > SIZE_MAX - 1) {
        err = PS5C_ERR_NOMEM;
    }
    uint8_t *data = NULL;
    if (err == PS5C_OK) {
        data = (uint8_t *)malloc((size_t)size + 1); /* +1: never malloc(0); allows NUL-termination */
        if (!data) {
            err = PS5C_ERR_NOMEM;
        }
    }
    uint64_t got = 0;
    if (err == PS5C_OK) {
        err = pal_file_read(f, data, size, &got);
        if (err == PS5C_OK && got != size) {
            err = PS5C_ERR_IO;
        }
    }
    pal_file_close(f);

    if (err != PS5C_OK) {
        free(data);
        return err;
    }
    data[size] = 0;
    *data_out = data;
    *size_out = size;
    return PS5C_OK;
}
