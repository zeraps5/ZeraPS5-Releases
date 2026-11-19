/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_PAL_EXFAT_H
#define PS5C_PAL_EXFAT_H
#include "pal/pal.h"

/* A raw partition image is exposed as image.exfat/<relative path>.
 * The image itself remains an ordinary host file. No OS mount or extraction. */
typedef struct exfat_file exfat_file;
bool exfat_path(const char *path);
bool exfat_image_path(const char *path);
ps5c_err exfat_init(void);
void exfat_shutdown(void); /* after callers have closed all virtual files */
ps5c_err exfat_open(const char *path, uint32_t flags, exfat_file **out);
void exfat_close(exfat_file *f);
ps5c_err exfat_read(exfat_file *f, void *buf, uint64_t size, uint64_t offset, uint64_t *got);
ps5c_err exfat_read_current(exfat_file *f, void *buf, uint64_t size, uint64_t *got);
ps5c_err exfat_seek(exfat_file *f, int64_t offset, int whence, uint64_t *pos);
uint64_t exfat_size(exfat_file *f);
ps5c_err exfat_stat(const char *path, pal_path_info *out);
ps5c_err exfat_list(const char *path, pal_dir_fn fn, void *user);
/* Locate exactly one eboot.bin at the root or under wrapper directories.
 * The @game view overlays already-decrypted executables/modules on game data.
 * Ambiguous images are rejected; an explicit virtual path can be used instead. */
ps5c_err exfat_game_path(const char *image, char *out, size_t cap);
/* PlayGo on a .pkg volume: whether [offset, offset + size) of the package file
 * (PLGX mchunk ranges are package offsets) is there to read. `path` is any
 * path inside the package. It checks the range against the file's real size
 * and reads its first and last bytes; it does not hash the data. *present is
 * false for a range past the end (a partial download). UNSUPPORTED: not a
 * package volume; IO: the file did not read. */
ps5c_err exfat_pkg_range_present(const char *path, uint64_t offset, uint64_t size, bool *present);
#endif
