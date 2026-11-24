/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Platform features a game sets up at boot and that stay idle offline:
 * libSceNpWebApi2 (contexts without a server), libSceShare and
 * libSceSharePlay (nothing recorded or shared), libSceSystemGesture (no touch
 * pad events), libSceTextToSpeech2 (no voice), and the initialization calls of
 * libraries with nothing behind them (live streaming, game updates, core
 * dumps, VR headset, game intents, session signaling, login dialog, content
 * export, audio decoders, voice QoS).
 *
 * Which functions games call, and their structures, were checked against the
 * AnyPS5 project's declarations (boykopovar/AnyPS5, GPL-2.0); this is our own
 * implementation. */
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

/* Clears `size` bytes at argument `arg`; false when it is not writable */
static bool clear_arg(hle_call *c, uint32_t arg, uint32_t size) {
    void *p = hle_arg_ptr(c, arg) ? hle_mem(c, hle_arg_ptr(c, arg), size) : NULL;
    if (p) memset(p, 0, size);
    return p != NULL;
}

/* int sceGameLiveStreamingGetCurrentStatus2(SceGameLiveStreamingStatus2 *status): 72 bytes
 * {int32 userId; bool isOnAir; u32 spectators, textMessages, commandMessages; u8 reserved[52]}.
 * Nobody broadcasts: all zero, userId -1 (layout from KytyPS5) */
static void live_streaming_status2(hle_call *c) {
    if (!clear_arg(c, 0, 72)) {
        hle_ret_s32(c, (int32_t)0x80A00002); /* SCE_GAME_LIVE_STREAMING_ERROR_INVALID_PARAM */
        return;
    }
    int32_t *user = (int32_t *)hle_mem(c, hle_arg_ptr(c, 0), 4);
    *user = -1;
    hle_ret_s32(c, 0);
}

/* ---- libSceNpWebApi2: contexts, users, push-event handles and filters are ids ---- */

#define WEBAPI2_ERR_INVALID_ARGUMENT ((int32_t)0x80553402)
static volatile int32_t g_webapi_next = 1;
static uint64_t g_webapi_pool; /* pool size given to sceNpWebApi2Initialize */

static void webapi_new_id(hle_call *c) { hle_ret_s32(c, __atomic_fetch_add(&g_webapi_next, 1, __ATOMIC_RELAXED)); }

/* int sceNpWebApi2Initialize(int http_ctx, size_t pool_size): the library context id */
static void webapi_initialize(hle_call *c) {
    __atomic_store_n(&g_webapi_pool, hle_arg(c, 1), __ATOMIC_RELAXED);
    webapi_new_id(c);
}

/* int sceNpWebApi2GetMemoryPoolStats(int ctx, {size_t pool, max_inuse, inuse; s32 reserved} *stats) */
static void webapi_get_memory_pool_stats(hle_call *c) {
    uint64_t *stats = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 24);
    if (!stats) {
        hle_ret_s32(c, WEBAPI2_ERR_INVALID_ARGUMENT);
        return;
    }
    stats[0] = __atomic_load_n(&g_webapi_pool, __ATOMIC_RELAXED);
    stats[1] = stats[2] = 0;
    hle_ret_s32(c, 0);
}

/* int sceNpWebApi2CreateRequest(int user_ctx, const char *api_group, const char *path, method,
 * const content_param *, int64_t *request_id): an id; sending it then fails as offline */
static void webapi_create_request(hle_call *c) {
    int64_t *id = (int64_t *)hle_mem(c, hle_arg_ptr(c, 5), 8);
    if (!id) {
        hle_ret_s32(c, WEBAPI2_ERR_INVALID_ARGUMENT);
        return;
    }
    *id = __atomic_fetch_add(&g_webapi_next, 1, __ATOMIC_RELAXED);
    hle_ret_s32(c, 0);
}

/* int sceNpWebApi2SendRequest(int64_t request, const void *data, size_t size, response_info *):
 * no network. Answering success left the response unfilled (Wobbly Life then read through null). */
static void webapi_send_request(hle_call *c) { hle_ret_s32(c, (int32_t)0x80410132 /* NET_ERROR_NETDOWN */); }
/* responses never arrive: ReadData, header queries and push contexts fail like the request did
 * (header outputs are cleared first, so a caller that ignores the error reads "" / 0) */
static void webapi_net_down(hle_call *c) { hle_ret_s32(c, (int32_t)0x80410132); }
/* (s64 request, const char *field, char *value, size_t size) */
static void webapi_header_value(hle_call *c) {
    uint8_t nul = 0;
    if (hle_arg_ptr(c, 2) && hle_arg(c, 3)) gmem_write(c->mem, hle_arg_ptr(c, 2), &nul, 1);
    webapi_net_down(c);
}
/* (s64 request, const char *field, size_t *length) */
static void webapi_header_length(hle_call *c) {
    uint64_t zero = 0;
    if (hle_arg_ptr(c, 2)) gmem_write(c->mem, hle_arg_ptr(c, 2), &zero, 8);
    webapi_net_down(c);
}


/* ---- libSceShare / libSceSharePlay ---- */

/* int sceShareGetCurrentStatus(u32 feature, SceShareCurrentStatus *status): 16 bytes, nothing recording */
static void share_get_current_status(hle_call *c) {
    if (!hle_arg_u32(c, 0) || !clear_arg(c, 1, 16)) {
        hle_ret_s32(c, (int32_t)0x81960002);
        return;
    }
    hle_ret_s32(c, 0);
}

/* int sceSharePlayGetCurrentConnectionInfoA(info *): { s32 status, mode; NpOnlineId host, visitor
 * (20 bytes each); u64 host_account, visitor_account; s32 host_user (+0x40), visitor_user (+0x44) }:
 * not connected, both users invalid */
static void share_play_get_connection_info(hle_call *c) {
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 0x48);
    if (!info) {
        hle_ret_s32(c, (int32_t)0x810E0001);
        return;
    }
    memset(info, 0, 0x48);
    int32_t invalid = -1;
    memcpy(info + 0x40, &invalid, 4);
    memcpy(info + 0x44, &invalid, 4);
    hle_ret_s32(c, 0);
}

/* ---- libSceSystemGesture: one handle, no touch events ---- */

#define GESTURE_HANDLE 1
#define GESTURE_ERR_INVALID_ARGUMENT ((int32_t)0x80D10002)
#define GESTURE_ERR_INVALID_HANDLE ((int32_t)0x80D10003)

/* int sceSystemGestureOpen(int input_type, const param *) */
static void gesture_open(hle_call *c) { hle_ret_s32(c, GESTURE_HANDLE); }

static void gesture_handle_ok(hle_call *c) {
    hle_ret_s32(c, hle_arg_s32(c, 0) == GESTURE_HANDLE ? 0 : GESTURE_ERR_INVALID_HANDLE);
}

/* Event queries end with u32 *count: GetTouchEvents(handle, recognizer, buffer, capacity, count)
 * and GetPrimitiveTouchEvents(handle, buffer, capacity, count) */
static void gesture_no_events(hle_call *c, uint32_t count_arg) {
    uint32_t *count = (uint32_t *)hle_mem(c, hle_arg_ptr(c, count_arg), 4);
    if (hle_arg_s32(c, 0) != GESTURE_HANDLE) {
        hle_ret_s32(c, GESTURE_ERR_INVALID_HANDLE);
        return;
    }
    if (!count) {
        hle_ret_s32(c, GESTURE_ERR_INVALID_ARGUMENT);
        return;
    }
    *count = 0;
    hle_ret_s32(c, 0);
}
static void gesture_get_touch_events(hle_call *c) { gesture_no_events(c, 4); }
static void gesture_get_primitive_touch_events(hle_call *c) { gesture_no_events(c, 3); }

/* int sceGameLiveStreamingGetProgramInfo(info *): 0x134C bytes, all zero (no broadcast
 * program); size and error code as in AnyPS5 */
static void live_streaming_program_info(hle_call *c) {
    hle_ret_s32(c, clear_arg(c, 0, 0x134C) ? 0 : (int32_t)0x80A00002);
}

/* ---- libSceLoginDialog: offline, the PSN sign-in ends at once as cancelled (AnyPS5) ---- */

#define LOGIN_ERR_NOT_INITIALIZED ((int32_t)0x81340001)
#define LOGIN_ERR_ALREADY_INITIALIZED ((int32_t)0x81340002)
#define LOGIN_ERR_INVALID_ARGUMENT ((int32_t)0x81340003)
#define LOGIN_ERR_BUSY ((int32_t)0x81340005)
#define LOGIN_ERR_INVALID_USER ((int32_t)0x81340007)
enum { CD_STATUS_NONE, CD_STATUS_INITIALIZED, CD_STATUS_RUNNING, CD_STATUS_FINISHED };
static volatile int32_t g_login_status;

static void login_initialize(hle_call *c) {
    int32_t was = __atomic_exchange_n(&g_login_status, CD_STATUS_INITIALIZED, __ATOMIC_ACQ_REL);
    if (was != CD_STATUS_NONE) __atomic_store_n(&g_login_status, was, __ATOMIC_RELEASE);
    hle_ret_s32(c, was == CD_STATUS_NONE ? 0 : LOGIN_ERR_ALREADY_INITIALIZED);
}
static void login_terminate(hle_call *c) {
    int32_t was = __atomic_exchange_n(&g_login_status, CD_STATUS_NONE, __ATOMIC_ACQ_REL);
    hle_ret_s32(c, was == CD_STATUS_NONE ? LOGIN_ERR_NOT_INITIALIZED : 0);
}
/* int sceLoginDialogOpen(const param *) */
static void login_open(hle_call *c) {
    int32_t s = __atomic_load_n(&g_login_status, __ATOMIC_ACQUIRE);
    if (!hle_arg_ptr(c, 0)) { hle_ret_s32(c, LOGIN_ERR_INVALID_ARGUMENT); return; }
    if (s == CD_STATUS_NONE) { hle_ret_s32(c, LOGIN_ERR_NOT_INITIALIZED); return; }
    if (s == CD_STATUS_RUNNING) { hle_ret_s32(c, LOGIN_ERR_BUSY); return; }
    __atomic_store_n(&g_login_status, CD_STATUS_FINISHED, __ATOMIC_RELEASE);
    hle_ret_s32(c, 0);
}
static void login_status(hle_call *c) { hle_ret_s32(c, __atomic_load_n(&g_login_status, __ATOMIC_ACQUIRE)); }
/* int sceLoginDialogGetResult({int result; int errorCode; ...} *) */
static void login_get_result(hle_call *c) {
    int32_t *r = hle_arg_ptr(c, 0) ? (int32_t *)hle_mem(c, hle_arg_ptr(c, 0), 8) : NULL;
    int32_t s = __atomic_load_n(&g_login_status, __ATOMIC_ACQUIRE);
    if (!r) { hle_ret_s32(c, LOGIN_ERR_INVALID_ARGUMENT); return; }
    if (s == CD_STATUS_NONE) { hle_ret_s32(c, LOGIN_ERR_NOT_INITIALIZED); return; }
    r[0] = 1; /* SCE_COMMON_DIALOG_RESULT_USER_CANCELED */
    r[1] = LOGIN_ERR_INVALID_USER;
    hle_ret_s32(c, s == CD_STATUS_FINISHED ? 0 : LOGIN_ERR_BUSY);
}
static void login_close(hle_call *c) {
    if (__atomic_load_n(&g_login_status, __ATOMIC_ACQUIRE) == CD_STATUS_NONE) {
        hle_ret_s32(c, LOGIN_ERR_NOT_INITIALIZED);
        return;
    }
    __atomic_store_n(&g_login_status, CD_STATUS_FINISHED, __ATOMIC_RELEASE);
    hle_ret_s32(c, 0);
}

/* ---- libSceContentExport: export sessions (ids 0..), as in AnyPS5 ---- */

static volatile uint32_t g_export_sessions; /* bit per session id */
/* int sceContentExportStart(void): a session id */
static void content_export_start(hle_call *c) {
    for (int id = 0; id < 4; id++) {
        uint32_t bit = 1u << id;
        if (!(__atomic_fetch_or(&g_export_sessions, bit, __ATOMIC_ACQ_REL) & bit)) {
            hle_ret_s32(c, id);
            return;
        }
    }
    hle_ret_s32(c, (int32_t)0x809D3007); /* execution max */
}
/* int sceContentExportFinish(int id) */
static void content_export_finish(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0);
    if (id >= 0 && id < 4) __atomic_fetch_and(&g_export_sessions, ~(1u << id), __ATOMIC_ACQ_REL);
    hle_ret_s32(c, 0);
}

/* ---- libSceConvertKeycode: the user's keyboard is the US layout (IME keyboard type 1) ---- */

#define KEYCODE_ERR_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define KEYCODE_ERR_INVALID_KEYBOARD_TYPE ((int32_t)0x80BC0032)
/* USB HID usage -> virtual key of the base (US) layout (table as in AnyPS5) */
static const uint8_t k_virtual_keys[256] = {
    255, 255, 255, 255, 65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,
    83,  84,  85,  86,  87,  88,  89,  90,  49,  50,  51,  52,  53,  54,  55,  56,  57,  48,  13,  27,  8,   9,
    32,  187, 219, 221, 186, 191, 191, 192, 222, 220, 188, 190, 189, 20,  112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 44,  145, 19,  45,  36,  33,  46,  35,  34,  39,  37,  40,  38,  144, 111, 106, 109, 107,
    13,  97,  98,  99,  100, 101, 102, 103, 104, 105, 96,  110, 226, 93,  [0xD8] = 12, [0xE0] = 162, 160, 164, 91, 163, 161, 165, 92};
/* int sceConvertKeycodeGetImeKeyboardType(int userId, u32 *type) */
static void keycode_ime_keyboard_type(hle_call *c) {
    uint32_t *t = hle_arg_ptr(c, 1) ? (uint32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4) : NULL;
    if (!t) { hle_ret_s32(c, KEYCODE_ERR_INVALID_ADDRESS); return; }
    *t = 1;
    hle_ret_s32(c, 0);
}
/* int sceConvertKeycodeGetVirtualKeycode(u16 keycode, u32 keyboardType, u16 *vk) */
static void keycode_virtual(hle_call *c) {
    uint16_t *vk = hle_arg_ptr(c, 2) ? (uint16_t *)hle_mem(c, hle_arg_ptr(c, 2), 2) : NULL;
    uint32_t code = hle_arg_u32(c, 0) & 0xFFFF, type = hle_arg_u32(c, 1);
    if (!vk) { hle_ret_s32(c, KEYCODE_ERR_INVALID_ADDRESS); return; }
    if (type == 0 || type > 39) { hle_ret_s32(c, KEYCODE_ERR_INVALID_KEYBOARD_TYPE); return; }
    uint8_t v = code < 256 ? k_virtual_keys[code] : 255;
    *vk = v ? v : 255;
    hle_ret_s32(c, 0);
}

/* int sceSystemGestureGetTouchEventByIndex(handle, recognizer, u32 index, event *): there are none */
static void gesture_event_by_index(hle_call *c) {
    if (hle_arg_s32(c, 0) != GESTURE_HANDLE) { hle_ret_s32(c, GESTURE_ERR_INVALID_HANDLE); return; }
    hle_ret_s32(c, hle_arg_ptr(c, 1) ? (int32_t)0x80890004 /* index out of array */ : GESTURE_ERR_INVALID_ARGUMENT);
}
/* int sceSystemGestureRemoveTouchRecognizer(handle, recognizer *) */
static void gesture_remove_recognizer(hle_call *c) {
    if (hle_arg_s32(c, 0) != GESTURE_HANDLE) { hle_ret_s32(c, GESTURE_ERR_INVALID_HANDLE); return; }
    hle_ret_s32(c, hle_arg_ptr(c, 1) ? 0 : GESTURE_ERR_INVALID_ARGUMENT);
}

/* ---- System IPC (libSceIpmi): libraries the game ships (NpEntitlementAccess) open a client
 * to a system service; there is none, and they stop there ---- */

/* int IPMI::Client::create(Client **out, const Config *, void *, void *) */
static void ipmi_client_create(hle_call *c) {
    uint64_t none = 0;
    gmem_write(c->mem, hle_arg_ptr(c, 0), &none, 8);
    hle_ret_s32(c, (int32_t)0x80020003); /* ESRCH: no such service */
}

/* ---- libSceCes: character set conversion contexts (Void Terrarium sets up Shift-JIS) ---- */

/* int sceCesUcsProfileInitSJis1997Cp932(profile *, int flags) / sceCesMbcsUcsContextInit(ctx *, profile *) */
static void ces_init(hle_call *c) { hle_ret_s32(c, hle_arg_ptr(c, 0) ? 0 : (int32_t)0x80020016); }

/* ---- Other queries ---- */

/* int sceNpSessionSignalingGetMemoryInfo({size_t total, max used, used} *info) */
static void session_signaling_memory_info(hle_call *c) {
    hle_ret_s32(c, clear_arg(c, 0, 24) ? 0 : (int32_t)0x80020016);
}

/* int sceAudioOut2UserGetSupportedAttributes(int user, u32 *attributes, u32 *count): none */
static void audio_out2_supported_attributes(hle_call *c) {
    clear_arg(c, 1, 4);
    clear_arg(c, 2, 4);
    hle_ret_s32(c, 0);
}

/* {u64 size, u64 align} sceHmd2ReprojectionQueryBufferSizeAlign(void), returned in rax:rdx:
 * the console's reprojection work buffer (0x100828 bytes, 256-aligned, as AnyPS5 documents).
 * Left unimplemented, Tetris Effect allocated whatever rdx held and stopped at boot. */
static void hmd2_reprojection_buffer(hle_call *c) {
    c->a->ret[0] = 0x100828;
    c->a->ret[1] = 0x100;
}

/* int sceHmd2GetDeviceInformation(info *, ...): no VR headset; the game's structure is left
 * as it is (its "not connected" state) */
static void hmd2_device_information(hle_call *c) { hle_ret_s32(c, 0); }

/* ---- libSceTextToSpeech2: no speech engine ---- */

static void tts_not_supported(hle_call *c) { hle_ret_s32(c, (int32_t)0x8002002D); /* EOPNOTSUPP */ }

/* Speak/Cancel on a handle the game opened elsewhere (Minecraft's narrator
 * never calls Open here): accepted, nothing is said. */
static void tts_silent(hle_call *c) { hle_ret_s32(c, 0); }

#define NPW "libSceNpWebApi2"
#define SH "libSceShare"
#define SG "libSceSystemGesture"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2Initialize", webapi_initialize),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2Terminate", ok),
    /* PS4 games (NpToolkit) start the first Web API the same way */
    HLE_FUNC_ENTRY("libSceNpWebApi", "sceNpWebApiInitialize", webapi_initialize),
    HLE_FUNC_ENTRY("libSceNpWebApi", "sceNpWebApiTerminate", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2CreateUserContext", webapi_new_id),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2DeleteUserContext", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventCreateHandle", webapi_new_id),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventDeleteHandle", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventAbortHandle", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventCreateFilter", webapi_new_id),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventDeleteFilter", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventRegisterCallback", webapi_new_id),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventUnregisterCallback", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2GetMemoryPoolStats", webapi_get_memory_pool_stats),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2CreateRequest", webapi_create_request),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2AddHttpRequestHeader", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2SendRequest", webapi_send_request),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2ReadData", webapi_net_down),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2GetHttpResponseHeaderValue", webapi_header_value),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2GetHttpResponseHeaderValueLength", webapi_header_length),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2SetRequestTimeout", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventCreatePushContext", webapi_net_down),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventDeletePushContext", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventRegisterPushContextCallback", webapi_new_id),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventUnregisterPushContextCallback", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2PushEventStartPushContextCallback", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2DeleteRequest", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2AbortRequest", ok),
    HLE_FUNC_ENTRY(NPW, "sceNpWebApi2CheckTimeout", ok), /* requests fail at once: nothing times out */
    HLE_FUNC_ENTRY(SH, "sceShareInitialize", ok),
    HLE_FUNC_ENTRY(SH, "sceShareTerminate", ok),
    HLE_FUNC_ENTRY(SH, "sceShareFeaturePermit", ok),
    HLE_FUNC_ENTRY(SH, "sceShareFeatureProhibit", ok),
    HLE_FUNC_ENTRY(SH, "sceShareSetContentParam", ok),
    HLE_FUNC_ENTRY(SH, "sceShareSetContentParamForApplicationTitle", ok),
    HLE_FUNC_ENTRY(SH, "sceShareSetScreenshotOverlayImage", ok),
    HLE_FUNC_ENTRY(SH, "sceShareGetCurrentStatus", share_get_current_status),
    HLE_FUNC_ENTRY(SH, "sceShareRegisterContentEventCallback", ok), /* no share events here (RE4, TMNT) */
    HLE_FUNC_ENTRY("libSceSharePlay", "sceSharePlayInitialize", ok),
    HLE_FUNC_ENTRY("libSceSharePlay", "sceSharePlayTerminate", ok),
    HLE_FUNC_ENTRY("libSceSharePlay", "sceSharePlayGetCurrentConnectionInfoA", share_play_get_connection_info),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureAppendTouchRecognizer", gesture_handle_ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureUpdateAllTouchRecognizer", gesture_handle_ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureGetTouchEventsCount", gesture_handle_ok), /* the count: 0 */
    HLE_FUNC_ENTRY(SG, "sceSystemGestureOpen", gesture_open),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureClose", gesture_handle_ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureCreateTouchRecognizer", gesture_handle_ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureInitializePrimitiveTouchRecognizer", ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureUpdateTouchRecognizer", gesture_handle_ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureUpdatePrimitiveTouchRecognizer", ok),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureGetTouchEvents", gesture_get_touch_events),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureGetPrimitiveTouchEvents", gesture_get_primitive_touch_events),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureGetTouchEventByIndex", gesture_event_by_index),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureRemoveTouchRecognizer", gesture_remove_recognizer),
    HLE_FUNC_ENTRY(SG, "sceSystemGestureFinalizePrimitiveTouchRecognizer", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Initialize", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Terminate", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Open", tts_not_supported),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Close", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2GetSystemStatus", tts_not_supported),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2GetSpeechStatus", tts_not_supported),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Speak", tts_silent),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Cancel", tts_silent),
    HLE_FUNC_ENTRY("libSceGameLiveStreaming", "sceGameLiveStreamingInitialize", ok),
    HLE_FUNC_ENTRY("libSceGameLiveStreaming", "sceGameLiveStreamingTerminate", ok),
    HLE_FUNC_ENTRY("libSceGameLiveStreaming", "sceGameLiveStreamingGetCurrentStatus2", live_streaming_status2),
    HLE_FUNC_ENTRY("libSceGameLiveStreaming", "sceGameLiveStreamingGetProgramInfo", live_streaming_program_info),
    HLE_FUNC_ENTRY("libSceGameUpdate", "sceGameUpdateInitialize", ok),
    HLE_FUNC_ENTRY("libSceGameUpdate", "sceGameUpdateTerminate", ok),
    HLE_FUNC_ENTRY("libSceCoredump", "sceCoredumpRegisterCoredumpHandler", ok),
    HLE_FUNC_ENTRY("libSceCoredump", "sceCoredumpUnregisterCoredumpHandler", ok),
    HLE_FUNC_ENTRY("libSceHmd2", "sceHmd2Initialize", ok),
    HLE_FUNC_ENTRY("libSceHmd2", "sceHmd2Terminate", ok),
    HLE_FUNC_ENTRY("libSceNpGameIntent", "sceNpGameIntentInitialize", ok),
    HLE_FUNC_ENTRY("libSceNpGameIntent", "sceNpGameIntentTerminate", ok),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingInitialize", ok),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingTerminate", ok),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogInitialize", login_initialize),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogTerminate", login_terminate),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogOpen", login_open),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogUpdateStatus", login_status),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogGetStatus", login_status),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogGetResult", login_get_result),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogClose", login_close),
    HLE_FUNC_ENTRY("libScePlayerSelectionDialog", "scePlayerSelectionDialogTerminate", ok),
    HLE_FUNC_ENTRY("libSceConvertKeycode", "sceConvertKeycodeGetImeKeyboardType", keycode_ime_keyboard_type),
    HLE_FUNC_ENTRY("libSceConvertKeycode", "sceConvertKeycodeGetVirtualKeycode", keycode_virtual),
    HLE_FUNC_ENTRY("libSceContentExport", "sceContentExportInit2", ok),
    HLE_FUNC_ENTRY("libSceContentExport", "sceContentExportTerm", ok),
    HLE_FUNC_ENTRY("libSceContentExport", "sceContentExportStart", content_export_start),
    HLE_FUNC_ENTRY("libSceContentExport", "sceContentExportFinish", content_export_finish),
    HLE_FUNC_ENTRY("libSceVoiceQoS", "sceVoiceQoSInit", ok),
    HLE_FUNC_ENTRY("libSceVoiceQoS", "sceVoiceQoSEnd", ok),
    HLE_FUNC_ENTRY("libSceIpmi", "_ZN4IPMI6Client6ConfigC1Ev", ok),
    HLE_FUNC_ENTRY("libSceIpmi", "_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_", ipmi_client_create),
    HLE_FUNC_ENTRY("libSceCes", "sceCesUcsProfileInitSJis1997Cp932", ces_init),
    HLE_FUNC_ENTRY("libSceCes", "sceCesMbcsUcsContextInit", ces_init),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingGetMemoryInfo", session_signaling_memory_info),
    HLE_FUNC_ENTRY("libSceAudioOut2", "sceAudioOut2UserGetSupportedAttributes", audio_out2_supported_attributes),
    HLE_FUNC_ENTRY("libSceHmd2", "sceHmd2GetDeviceInformation", hmd2_device_information),
    HLE_FUNC_ENTRY("libSceHmd2", "sceHmd2ReprojectionQueryBufferSizeAlign", hmd2_reprojection_buffer),
};

void hle_register_platform(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
