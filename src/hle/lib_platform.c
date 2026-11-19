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
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Initialize", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Terminate", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Open", tts_not_supported),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Close", ok),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2GetSpeechStatus", tts_not_supported),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Speak", tts_silent),
    HLE_FUNC_ENTRY("libSceTextToSpeech2", "sceTextToSpeech2Cancel", tts_silent),
    HLE_FUNC_ENTRY("libSceGameLiveStreaming", "sceGameLiveStreamingInitialize", ok),
    HLE_FUNC_ENTRY("libSceGameLiveStreaming", "sceGameLiveStreamingTerminate", ok),
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
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogInitialize", ok),
    HLE_FUNC_ENTRY("libSceLoginDialog", "sceLoginDialogTerminate", ok),
    HLE_FUNC_ENTRY("libSceContentExport", "sceContentExportInit2", ok),
    HLE_FUNC_ENTRY("libSceContentExport", "sceContentExportTerm", ok),
    HLE_FUNC_ENTRY("libSceAudiodec", "sceAudiodecInitLibrary", ok),
    HLE_FUNC_ENTRY("libSceAudiodec", "sceAudiodecTermLibrary", ok),
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
