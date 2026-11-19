/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/panic.h"

#include <stdlib.h>

#include "base/log.h"

void ps5c_panic(const char *file, int line, const char *msg) {
    log_emit(LOG_CAT_CORE, LOG_LVL_FATAL, "%s:%d: %s", file, line, msg);
    abort();
}
