/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System services an offline console without a UI answers on its own: the
 * imports games read through an output pointer, found by the static survey of
 * unimplemented imports (PS5C_LIST_UNIMPLEMENTED). Answers follow AnyPS5:
 *   - dialogs (IME, player invitation, web browser, PlayGo) finish at once,
 *     cancelled by the user (PlayGo: OK, everything is installed);
 *   - no system events, game intents, updates or Remote Play connections.
 *
 * Layouts (SDK):
 *   SceImeDialogResult              { s32 endstatus; u8 reserved[12]; }
 *   ScePlayerInvitationDialogResult { s32 errorCode; s32 result; u8 reserved[32]; }
 *   ScePlayGoDialogResult           { s32 errorCode; s32 result; u8 reserved[32]; }
 *   SceWebBrowserDialogResult       { s32 result; ... }
 *   SceSystemServicePlayerDialogParam { u64 size; s32 mode; s32 userId; u64 accountId; u8 reserved[40]; }
 *   SceGameUpdateCheckResult        { u64 size; bool found; bool addcontFound; ... }
 *   SceSaveDataIcon                 { void *buf; u64 bufSize; u64 dataSize; u8 reserved[32]; } */
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define DIALOG_NONE 0
#define DIALOG_INITIALIZED 1
#define DIALOG_FINISHED 3
#define RESULT_OK 0
#define RESULT_USER_CANCELED 1

#define COMMON_DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003u)
#define COMMON_DIALOG_NOT_FINISHED ((int32_t)0x80B80005u)

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

static void put_s32(hle_call *c, guest_addr at, int32_t v) {
    if (at) gmem_write(c->mem, at, &v, 4);
}

static void put_u64(hle_call *c, guest_addr at, uint64_t v) {
    if (at) gmem_write(c->mem, at, &v, 8);
}

static void put_zero(hle_call *c, guest_addr at, uint32_t size) {
    uint8_t z[64] = {0};
    if (at && size <= sizeof(z)) gmem_write(c->mem, at, z, size);
}

/* { s32 errorCode; s32 result; u8 reserved[32]; } of a finished dialog */
static int32_t dialog_result40(hle_call *c, int32_t status, guest_addr out, int32_t result) {
    if (status == DIALOG_NONE) return COMMON_DIALOG_NOT_INITIALIZED;
    if (status != DIALOG_FINISHED) return COMMON_DIALOG_NOT_FINISHED;
    put_zero(c, out, 40);
    put_s32(c, out ? out + 4 : 0, result);
    return 0;
}

/* ---- System service ---- */

#define SYSTEM_SERVICE_NO_EVENT ((int32_t)0x80A10004u)

static void ss_receive_event(hle_call *c) { hle_ret_s32(c, SYSTEM_SERVICE_NO_EVENT); }

static void ss_init_player_dialog_param(hle_call *c) {
    guest_addr p = hle_arg_ptr(c, 0);
    put_zero(c, p, 0x40);
    put_u64(c, p, 0x40);
    put_s32(c, p ? p + 12 : 0, -1);
    hle_ret_s32(c, 0);
}

static void ss_load_exec(hle_call *c) {
    LOG_W(HLE, "sceSystemServiceLoadExec: relaunching the game is not supported, ignored");
    hle_ret_s32(c, 0);
}

/* ---- NP game intents: none arrive ---- */

#define GAME_INTENT_NOT_FOUND ((int32_t)0x80553806u)
#define GAME_INTENT_VALUE_NOT_FOUND ((int32_t)0x80553807u)

static void gi_receive(hle_call *c) { hle_ret_s32(c, GAME_INTENT_NOT_FOUND); }

/* (const data *, const char *key, char *buf, size_t size) */
static void gi_property_string(hle_call *c) {
    uint8_t nul = 0;
    if (hle_arg_ptr(c, 2) && hle_arg(c, 3)) gmem_write(c->mem, hle_arg_ptr(c, 2), &nul, 1);
    hle_ret_s32(c, GAME_INTENT_VALUE_NOT_FOUND);
}

/* ---- registry, modules ---- */

/* (s64 key, s32 unk, s32 *data) */
static void reg_nonsys_get_int(hle_call *c) {
    put_s32(c, hle_arg_ptr(c, 2), 0);
    hle_ret_s32(c, 0);
}

/* (handle, args, argp, flags, opt, int *res): the module stays loaded */
static void kernel_stop_unload_module(hle_call *c) {
    put_s32(c, hle_arg_ptr(c, 5), 0);
    hle_ret_s32(c, 0);
}

/* ---- IME dialog: cancelled, the game keeps its text ---- */

#define IME_NONE 0
#define IME_FINISHED 2
#define IME_END_USER_CANCELED 1
#define IME_END_ABORTED 2
#define IME_NOT_OPENED ((int32_t)0x80BC0002u)

static int32_t g_ime_status, g_ime_end;

static void ime_init(hle_call *c) {
    LOG_I(HLE, "IME dialog: no keyboard UI, answered as cancelled");
    g_ime_status = IME_FINISHED;
    g_ime_end = IME_END_USER_CANCELED;
    hle_ret_s32(c, 0);
}

static void ime_status(hle_call *c) { hle_ret_s32(c, g_ime_status); }

static void ime_result(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    if (!out || g_ime_status != IME_FINISHED) {
        hle_ret_s32(c, IME_NOT_OPENED);
        return;
    }
    put_zero(c, out, 16);
    put_s32(c, out, g_ime_end);
    hle_ret_s32(c, 0);
}

static void ime_abort(hle_call *c) {
    if (g_ime_status == IME_NONE) {
        hle_ret_s32(c, IME_NOT_OPENED);
        return;
    }
    g_ime_status = IME_FINISHED;
    g_ime_end = IME_END_ABORTED;
    hle_ret_s32(c, 0);
}

static void ime_term(hle_call *c) {
    g_ime_status = IME_NONE;
    hle_ret_s32(c, 0);
}

/* (param, extended, u32 *width, u32 *height) */
static void ime_panel_size_ext(hle_call *c) {
    put_s32(c, hle_arg_ptr(c, 2), 1920);
    put_s32(c, hle_arg_ptr(c, 3), 1080);
    hle_ret_s32(c, 0);
}

/* ---- player invitation dialog: cancelled ---- */

static int32_t g_invite;

static void inv_initialize(hle_call *c) {
    g_invite = DIALOG_INITIALIZED;
    hle_ret_s32(c, 0);
}

static void inv_open(hle_call *c) {
    g_invite = DIALOG_FINISHED;
    hle_ret_s32(c, 0);
}

static void inv_status(hle_call *c) { hle_ret_s32(c, g_invite); }

static void inv_result(hle_call *c) {
    hle_ret_s32(c, dialog_result40(c, g_invite, hle_arg_ptr(c, 0), RESULT_USER_CANCELED));
}

static void inv_terminate(hle_call *c) {
    g_invite = DIALOG_NONE;
    hle_ret_s32(c, 0);
}

/* ---- PlayGo dialog: everything is installed, OK ---- */

static int32_t g_playgo;

static void pgd_initialize(hle_call *c) {
    g_playgo = DIALOG_INITIALIZED;
    hle_ret_s32(c, 0);
}

static void pgd_open(hle_call *c) {
    g_playgo = DIALOG_FINISHED;
    hle_ret_s32(c, 0);
}

static void pgd_status(hle_call *c) { hle_ret_s32(c, g_playgo); }

static void pgd_result(hle_call *c) { hle_ret_s32(c, dialog_result40(c, g_playgo, hle_arg_ptr(c, 0), RESULT_OK)); }

static void pgd_terminate(hle_call *c) {
    g_playgo = DIALOG_NONE;
    hle_ret_s32(c, 0);
}

/* ---- web browser dialog: closed by the user (lib_dialogs.c keeps status NONE) ---- */

static void wb_result(hle_call *c) {
    put_s32(c, hle_arg_ptr(c, 0), RESULT_USER_CANCELED);
    hle_ret_s32(c, 0);
}

/* ---- game update: no update on the server ---- */

static void gu_create_request(hle_call *c) { hle_ret_s32(c, 1); }

/* (requestId, const param *, result *) */
static void gu_check(hle_call *c) {
    guest_addr r = hle_arg_ptr(c, 2);
    if (r) {
        uint8_t none[2] = {0, 0};
        gmem_write(c->mem, r + 8, none, 2);
    }
    hle_ret_s32(c, 0);
}

#define GAME_UPDATE_NOT_FOUND ((int32_t)0x80412805u)

static void gu_addcont_latest(hle_call *c) { hle_ret_s32(c, GAME_UPDATE_NOT_FOUND); }

/* ---- Remote Play: never connected ---- */

static void rp_connection_status(hle_call *c) {
    put_s32(c, hle_arg_ptr(c, 1), 0);
    hle_ret_s32(c, 0);
}

/* ---- icons nobody has ---- */

/* (mountPoint, SceSaveDataIcon *): an empty icon */
static void sd_load_icon(hle_call *c) {
    guest_addr icon = hle_arg_ptr(c, 1);
    put_u64(c, icon ? icon + 16 : 0, 0);
    hle_ret_s32(c, 0);
}

#define TROPHY_ICON_NOT_FOUND ((int32_t)0x80551622u)

/* (context, handle[, id], void *buffer, size_t *size): no icon file */
static void trophy2_game_icon(hle_call *c) {
    put_u64(c, hle_arg_ptr(c, 3), 0);
    hle_ret_s32(c, TROPHY_ICON_NOT_FOUND);
}

static void trophy2_icon4(hle_call *c) {
    put_u64(c, hle_arg_ptr(c, 4), 0);
    hle_ret_s32(c, TROPHY_ICON_NOT_FOUND);
}

/* ---- PlayStation Store (commerce) dialog: closed by the user, nothing bought ----
 * SceNpCommerceDialogParam { base[48]; u32 size; s32 userId; u32 mode; u32 option;
 *                            const char **products; u32 count; pad; u64 argument; u64 userData; ... }
 * SceNpCommerceDialogResult { s32 result; bool authorized; pad; u64 userData; u8 reserved[32]; } */

static int32_t g_commerce;
static uint64_t g_commerce_user_data;

static void com_initialize(hle_call *c) {
    g_commerce = DIALOG_INITIALIZED;
    hle_ret_s32(c, 0);
}

static void com_open(hle_call *c) {
    if (g_commerce == DIALOG_NONE) {
        hle_ret_s32(c, COMMON_DIALOG_NOT_INITIALIZED);
        return;
    }
    g_commerce_user_data = 0;
    if (hle_arg_ptr(c, 0)) gmem_read(c->mem, hle_arg_ptr(c, 0) + 88, &g_commerce_user_data, 8);
    LOG_I(HLE, "PlayStation Store dialog: no store offline, answered as cancelled");
    g_commerce = DIALOG_FINISHED;
    hle_ret_s32(c, 0);
}

static void com_status(hle_call *c) { hle_ret_s32(c, g_commerce); }

static void com_result(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0);
    if (g_commerce == DIALOG_NONE) {
        hle_ret_s32(c, COMMON_DIALOG_NOT_INITIALIZED);
        return;
    }
    if (g_commerce != DIALOG_FINISHED) {
        hle_ret_s32(c, COMMON_DIALOG_NOT_FINISHED);
        return;
    }
    put_zero(c, out, 48);
    put_s32(c, out, RESULT_USER_CANCELED);
    put_u64(c, out ? out + 8 : 0, g_commerce_user_data);
    hle_ret_s32(c, 0);
}

static void com_terminate(hle_call *c) {
    g_commerce = DIALOG_NONE;
    hle_ret_s32(c, 0);
}

/* ---- "authorize this app" dialog (PSN account linking): no PSN, closed by the user ----
 * Only the leading s32 result of its result struct is written; the rest of the layout is
 * not known. */

static int32_t g_auth_app;

static void aad_initialize(hle_call *c) {
    g_auth_app = DIALOG_INITIALIZED;
    hle_ret_s32(c, 0);
}

static void aad_open(hle_call *c) {
    if (g_auth_app == DIALOG_NONE) {
        hle_ret_s32(c, COMMON_DIALOG_NOT_INITIALIZED);
        return;
    }
    LOG_I(HLE, "PSN app authorization dialog: no PSN offline, answered as cancelled");
    g_auth_app = DIALOG_FINISHED;
    hle_ret_s32(c, 0);
}

static void aad_status(hle_call *c) { hle_ret_s32(c, g_auth_app); }

static void aad_result(hle_call *c) {
    if (g_auth_app != DIALOG_FINISHED) {
        hle_ret_s32(c, g_auth_app == DIALOG_NONE ? COMMON_DIALOG_NOT_INITIALIZED
                                                 : COMMON_DIALOG_NOT_FINISHED);
        return;
    }
    put_s32(c, hle_arg_ptr(c, 0), RESULT_USER_CANCELED);
    hle_ret_s32(c, 0);
}

static void aad_terminate(hle_call *c) {
    g_auth_app = DIALOG_NONE;
    hle_ret_s32(c, 0);
}

/* void sceImeParamInit(SceImeParam *p): zeroed, userId -1 (SceImeParam is 96 bytes; its last
 * 8 reserved bytes are left alone in case a newer SDK made the struct shorter) */
static void ime_param_init(hle_call *c) {
    put_zero(c, hle_arg_ptr(c, 0), 64);
    put_zero(c, hle_arg_ptr(c, 0) ? hle_arg_ptr(c, 0) + 64 : 0, 24);
    put_s32(c, hle_arg_ptr(c, 0), -1);
    hle_ret_s32(c, 0);
}

#define SS "libSceSystemService"
#define IME "libSceImeDialog"
#define INV "libScePlayerInvitationDialog"
#define PGD "libScePlayGoDialog"
#define WB "libSceWebBrowserDialog"
#define GU "libSceGameUpdate"
#define T2 "libSceNpTrophy2"
#define NC "libSceNpCommerce"
#define AAD "libSceNpAuthAuthorizedAppDialog"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogInitialize", aad_initialize),
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogOpen", aad_open),
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogUpdateStatus", aad_status),
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogGetStatus", aad_status),
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogGetResult", aad_result),
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogClose", ok),
    HLE_FUNC_ENTRY(AAD, "sceNpAuthAuthorizedAppDialogTerminate", aad_terminate),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogInitialize", com_initialize),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogOpen", com_open),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogOpen2", com_open),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogUpdateStatus", com_status),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogGetStatus", com_status),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogGetResult", com_result),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogClose", ok),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceDialogTerminate", com_terminate),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceShowPsStoreIcon", ok),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceHidePsStoreIcon", ok),
    HLE_FUNC_ENTRY(NC, "sceNpCommerceSetPsStoreIconLayout", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceReceiveEvent", ss_receive_event),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceInitializePlayerDialogParam", ss_init_player_dialog_param),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceLaunchPlayerDialog", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceLoadExec", ss_load_exec),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceReportAbnormalTermination", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceDisableMusicPlayer", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceReenableMusicPlayer", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServicePowerTick", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceSetNoticeScreenSkipFlag", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceShowControllerSettings", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceOpenChallengeActivity", ok),
    HLE_FUNC_ENTRY(SS, "sceSystemServiceOpenTournamentOccurrence", ok),
    HLE_FUNC_ENTRY("libSceNpGameIntent", "sceNpGameIntentReceiveIntent", gi_receive),
    HLE_FUNC_ENTRY("libSceNpGameIntent", "sceNpGameIntentGetPropertyValueString", gi_property_string),
    HLE_FUNC_ENTRY("libSceRegMgr", "sceRegMgrNonSysGetInt", reg_nonsys_get_int),
    HLE_FUNC_ENTRY("libSceSysmodule", "sceSysmoduleLoadModuleByNameInternal", ok),
    HLE_FUNC_ENTRY("libkernel", "sceKernelStopUnloadModule", kernel_stop_unload_module),
    HLE_FUNC_ENTRY("libSceIme", "sceImeParamInit", ime_param_init),
    HLE_FUNC_ENTRY(IME, "sceImeDialogInit", ime_init),
    HLE_FUNC_ENTRY(IME, "sceImeDialogGetStatus", ime_status),
    HLE_FUNC_ENTRY(IME, "sceImeDialogGetResult", ime_result),
    HLE_FUNC_ENTRY(IME, "sceImeDialogAbort", ime_abort),
    HLE_FUNC_ENTRY(IME, "sceImeDialogTerm", ime_term),
    HLE_FUNC_ENTRY(IME, "sceImeDialogGetPanelSizeExtended", ime_panel_size_ext),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogInitialize", inv_initialize),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogOpen", inv_open),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogUpdateStatus", inv_status),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogGetStatus", inv_status),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogGetResult", inv_result),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogClose", ok),
    HLE_FUNC_ENTRY(INV, "scePlayerInvitationDialogTerminate", inv_terminate),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogInitialize", pgd_initialize),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogOpen", pgd_open),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogUpdateStatus", pgd_status),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogGetStatus", pgd_status),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogGetResult", pgd_result),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogClose", ok),
    HLE_FUNC_ENTRY(PGD, "scePlayGoDialogTerminate", pgd_terminate),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogInitialize", ok),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogTerminate", ok),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogOpen", ok),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogOpenForPredeterminedContent", ok),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogClose", ok),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogGetResult", wb_result),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogResetCookie", ok),
    HLE_FUNC_ENTRY(WB, "sceWebBrowserDialogSetCookie", ok),
    HLE_FUNC_ENTRY(GU, "sceGameUpdateCreateRequest", gu_create_request),
    HLE_FUNC_ENTRY(GU, "sceGameUpdateDeleteRequest", ok),
    HLE_FUNC_ENTRY(GU, "sceGameUpdateCheck", gu_check),
    HLE_FUNC_ENTRY(GU, "sceGameUpdateGetAddcontLatestVersion", gu_addcont_latest),
    HLE_FUNC_ENTRY("libSceRemoteplay", "sceRemoteplayInitialize", ok),
    HLE_FUNC_ENTRY("libSceRemoteplay", "sceRemoteplayTerminate", ok),
    HLE_FUNC_ENTRY("libSceRemoteplay", "sceRemoteplayGetConnectionStatus", rp_connection_status),
    HLE_FUNC_ENTRY("libSceSaveData_native", "sceSaveDataLoadIcon", sd_load_icon),
    HLE_FUNC_ENTRY(T2, "sceNpTrophy2GetGameIcon", trophy2_game_icon),
    HLE_FUNC_ENTRY(T2, "sceNpTrophy2GetGroupIcon", trophy2_icon4),
    HLE_FUNC_ENTRY(T2, "sceNpTrophy2GetRewardIcon", trophy2_icon4),
    HLE_FUNC_ENTRY(T2, "sceNpTrophy2GetTrophyIcon", trophy2_icon4),
};

void hle_register_services_offline(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
