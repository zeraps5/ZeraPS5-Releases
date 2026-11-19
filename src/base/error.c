/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "base/base.h"

const char *ps5c_err_str(ps5c_err err) {
    switch (err) {
    case PS5C_OK: return "ok";
    case PS5C_ERR_NOMEM: return "out of memory";
    case PS5C_ERR_INVALID: return "invalid argument or data";
    case PS5C_ERR_IO: return "I/O error";
    case PS5C_ERR_NOT_FOUND: return "not found";
    case PS5C_ERR_UNSUPPORTED: return "unsupported";
    case PS5C_ERR_OS: return "host OS error";
    case PS5C_ERR_BUSY: return "busy";
    case PS5C_ERR_TIMEOUT: return "timeout";
    case PS5C_ERR_OUT_OF_RANGE: return "out of range";
    default: return "unknown error";
    }
}
