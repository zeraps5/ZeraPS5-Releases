/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System dialogs without a UI: a dialog finishes as soon as it opens, as if
 * the user confirmed it (OK button); the PSN sign-in dialog finishes as
 * cancelled (no account to sign in with). Status values: 0 none, 1 initialized,
 * 2 running, 3 finished.
 *
 * Layouts (SDK):
 *   SceSaveDataDialogParam  { u8 base[48]; s32 size; s32 mode; s32 dispType; pad; void *anim; void *items;
 *                             void *userMsg; void *sysMsg; void *errorCode; void *progBar; void *userData; ... }
 *   SceSaveDataDialogItems  { s32 userId; pad; const TitleId *titleId; const DirName *dirNames; u32 dirNamesNum; }
 *   SceSaveDataDialogResult { s32 mode; s32 result; s32 buttonId; pad; DirName *dirName; Param *param;
 *                             void *userData; u8 reserved[32]; }
 *   SceMsgDialogParam       { u8 base[48]; u64 size; s32 mode; ... }
 *   SceMsgDialogResult      { s32 mode; s32 result; s32 buttonId; u8 reserved[32]; } */
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define ST_NONE 0
#define ST_INITIALIZED 1
#define ST_FINISHED 3
#define BUTTON_OK 1

typedef struct dialog_state {
    int32_t status;
    int32_t mode;
    uint64_t user_data;
    char dir_name[32];
} dialog_state;

static dialog_state g_save, g_msg, g_error, g_signin;

static void init_state(dialog_state *d) {
    memset(d, 0, sizeof(*d));
    d->status = ST_INITIALIZED;
}

/* ---- Save data dialog ---- */

static void sdd_initialize(hle_call *c) {
    init_state(&g_save);
    hle_ret_s32(c, 0);
}

static void sdd_open(hle_call *c) {
    const uint8_t *p = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 120);
    g_save.dir_name[0] = '\0';
    if (p) {
        uint64_t items = 0;
        memcpy(&g_save.mode, p + 52, 4);
        memcpy(&items, p + 72, 8);
        memcpy(&g_save.user_data, p + 112, 8);
        const uint8_t *it = items ? (const uint8_t *)hle_mem(c, items, 32) : NULL;
        if (it) {
            uint64_t names = 0;
            uint32_t num = 0;
            memcpy(&names, it + 16, 8);
            memcpy(&num, it + 24, 4);
            for (uint32_t i = 0; i < num && names; i++) {
                const char *n = (const char *)hle_mem(c, names + (uint64_t)i * 32, 32);
                if (n && n[0]) {
                    memcpy(g_save.dir_name, n, 31);
                    g_save.dir_name[31] = '\0';
                    break;
                }
            }
        }
    }
    LOG_D(HLE, "save data dialog: mode %d, dir '%s' (answered OK)", g_save.mode, g_save.dir_name);
    g_save.status = ST_FINISHED;
    hle_ret_s32(c, 0);
}

static void sdd_status(hle_call *c) { hle_ret_s32(c, g_save.status); }

static void sdd_get_result(hle_call *c) {
    uint8_t *r = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 88);
    if (r) {
        int32_t result = 0, button = BUTTON_OK;
        memcpy(r, &g_save.mode, 4);
        memcpy(r + 4, &result, 4);
        memcpy(r + 8, &button, 4);
        memcpy(r + 40, &g_save.user_data, 8);
        uint64_t dir = 0;
        memcpy(&dir, r + 16, 8);
        char *d = dir && g_save.dir_name[0] ? (char *)hle_mem(c, dir, 32) : NULL;
        if (d) memcpy(d, g_save.dir_name, 32);
    }
    hle_ret_s32(c, 0);
}

static void sdd_close(hle_call *c) {
    g_save.status = ST_FINISHED;
    hle_ret_s32(c, 0);
}

static void sdd_terminate(hle_call *c) {
    memset(&g_save, 0, sizeof(g_save));
    hle_ret_s32(c, 0);
}

static void dialog_ok(hle_call *c) { hle_ret_s32(c, 0); }
static void ready_to_display(hle_call *c) { hle_ret_s32(c, 1); }

/* ---- Message dialog ---- */

static void msg_initialize(hle_call *c) {
    init_state(&g_msg);
    hle_ret_s32(c, 0);
}

static void msg_open(hle_call *c) {
    const uint8_t *p = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 64);
    if (p) memcpy(&g_msg.mode, p + 56, 4);
    g_msg.status = ST_FINISHED;
    hle_ret_s32(c, 0);
}

static void msg_status(hle_call *c) { hle_ret_s32(c, g_msg.status); }

static void msg_get_result(hle_call *c) {
    uint8_t *r = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 44);
    if (r) {
        int32_t result = 0, button = BUTTON_OK;
        memcpy(r, &g_msg.mode, 4);
        memcpy(r + 4, &result, 4);
        memcpy(r + 8, &button, 4);
    }
    hle_ret_s32(c, 0);
}

static void msg_close(hle_call *c) {
    g_msg.status = ST_FINISHED;
    hle_ret_s32(c, 0);
}

static void msg_terminate(hle_call *c) {
    memset(&g_msg, 0, sizeof(g_msg));
    hle_ret_s32(c, 0);
}

/* ---- Error dialog ---- */

static void err_initialize(hle_call *c) {
    init_state(&g_error);
    hle_ret_s32(c, 0);
}

static void err_open(hle_call *c) {
    g_error.status = ST_FINISHED;
    hle_ret_s32(c, 0);
}

static void err_status(hle_call *c) { hle_ret_s32(c, g_error.status); }

static void err_terminate(hle_call *c) {
    memset(&g_error, 0, sizeof(g_error));
    hle_ret_s32(c, 0);
}

/* ---- PSN sign-in dialog: the user cancels ---- */

#define RESULT_USER_CANCELED 1

static void signin_initialize(hle_call *c) {
    init_state(&g_signin);
    hle_ret_s32(c, 0);
}

static void signin_open(hle_call *c) {
    LOG_I(HLE, "PSN sign-in dialog: no account, answered as cancelled");
    g_signin.status = ST_FINISHED;
    hle_ret_s32(c, 0);
}

static void signin_status(hle_call *c) { hle_ret_s32(c, g_signin.status); }

/* int sceSigninDialogGetResult(SceSigninDialogResult *r): { s32 result; u8 reserved[] } */
static void signin_get_result(hle_call *c) {
    int32_t result = RESULT_USER_CANCELED;
    if (hle_arg_ptr(c, 0)) gmem_write(c->mem, hle_arg_ptr(c, 0), &result, 4);
    hle_ret_s32(c, 0);
}

static void signin_terminate(hle_call *c) {
    memset(&g_signin, 0, sizeof(g_signin));
    hle_ret_s32(c, 0);
}

#define SDD "libSceSaveDataDialog.native"
#define SDD4 "libSceSaveDataDialog"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY("libSceCommonDialog", "sceCommonDialogInitialize", dialog_ok),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogInitialize", sdd_initialize),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogOpen", sdd_open),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogUpdateStatus", sdd_status),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogGetStatus", sdd_status),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogGetResult", sdd_get_result),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogClose", sdd_close),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogTerminate", sdd_terminate),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogIsReadyToDisplay", ready_to_display),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogProgressBarSetValue", dialog_ok),
    HLE_FUNC_ENTRY(SDD, "sceSaveDataDialogProgressBarInc", dialog_ok),
    /* PS4 games import the same functions from libSceSaveDataDialog (LEGO City Undercover) */
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogInitialize", sdd_initialize),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogOpen", sdd_open),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogUpdateStatus", sdd_status),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogGetStatus", sdd_status),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogGetResult", sdd_get_result),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogClose", sdd_close),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogTerminate", sdd_terminate),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogIsReadyToDisplay", ready_to_display),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogProgressBarSetValue", dialog_ok),
    HLE_FUNC_ENTRY(SDD4, "sceSaveDataDialogProgressBarInc", dialog_ok),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogInitialize", msg_initialize),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogOpen", msg_open),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogUpdateStatus", msg_status),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogGetStatus", msg_status),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogGetResult", msg_get_result),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogClose", msg_close),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogTerminate", msg_terminate),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogProgressBarSetValue", dialog_ok),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogProgressBarInc", dialog_ok),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogInitialize", msg_initialize),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogOpen", msg_open),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogUpdateStatus", msg_status),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogGetStatus", msg_status),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogGetResult", msg_get_result),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogClose", msg_close),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogTerminate", msg_terminate),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogProgressBarSetValue", dialog_ok),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogProgressBarInc", dialog_ok),
    HLE_FUNC_ENTRY("libSceMsgDialog", "sceMsgDialogProgressBarSetMsg", dialog_ok),
    HLE_FUNC_ENTRY("libSceMsgDialog.native", "sceMsgDialogProgressBarSetMsg", dialog_ok),
    /* bool sceCommonDialogIsUsed(void): false, no system dialog is ever on screen */
    HLE_FUNC_ENTRY("libSceCommonDialog", "sceCommonDialogIsUsed", dialog_ok),
    HLE_FUNC_ENTRY("libSceErrorDialog", "sceErrorDialogInitialize", err_initialize),
    HLE_FUNC_ENTRY("libSceErrorDialog", "sceErrorDialogOpen", err_open),
    HLE_FUNC_ENTRY("libSceErrorDialog", "sceErrorDialogUpdateStatus", err_status),
    HLE_FUNC_ENTRY("libSceErrorDialog", "sceErrorDialogGetStatus", err_status),
    HLE_FUNC_ENTRY("libSceErrorDialog", "sceErrorDialogTerminate", err_terminate),
    HLE_FUNC_ENTRY("libSceErrorDialog", "sceErrorDialogClose", err_open),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogInitialize", signin_initialize),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogOpen", signin_open),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogUpdateStatus", signin_status),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogGetStatus", signin_status),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogGetResult", signin_get_result),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogClose", dialog_ok),
    HLE_FUNC_ENTRY("libSceSigninDialog", "sceSigninDialogTerminate", signin_terminate),
};

void hle_register_dialogs(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
