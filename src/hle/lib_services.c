/* SPDX-License-Identifier: GPL-2.0-or-later */
/* System services a game queries at boot and every frame:
 * libSceUserService (one local user), libSceNpManager (signed out),
 * libScePad (one controller fed by the host keyboard and gamepad),
 * libSceMouse (optional native input), libSceSystemService status. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "base/log.h"
#include "core/runtime.h"
#include "core/settings.h"
#include "core/user_config.h"
#include "hle/addcont.h"
#include "hle/hle.h"
#include "hle/pad_script.h"
#include "hle/video_out.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define USER_ID 1
#define USER_NAME "PS5C"

#define US_ERR_INVALID_ARGUMENT ((int32_t)0x80960002)
#define US_ERR_NOT_LOGGED_IN ((int32_t)0x80960009)
#define US_ERR_NO_EVENT ((int32_t)0x80960007)
#define PAD_ERR_INVALID_ARG ((int32_t)0x80920001)
#define PAD_ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define NP_ERR_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_ERR_SIGNED_OUT ((int32_t)0x80550006)
#define NP_AUTH_ERR_SERVICE_DOWN ((int32_t)0x80550401)

#define PAD_HANDLE 1

static int32_t write_s32(hle_call *c, uint32_t arg, int32_t v, int32_t err) {
    int32_t *out = (int32_t *)hle_mem(c, hle_arg_ptr(c, arg), 4);
    if (!out) return err;
    *out = v;
    return 0;
}

/* Local players: [controle] jogadores=N (1-4, default 1). Player n (from 0) is user id n + 1
 * and pad handle n + 1; player 0 reads the keyboard and the first controller, the others one
 * XInput controller each (pal_input_read_player). */
static uint32_t players(void) {
    static uint32_t n;
    if (!n) {
        int v = settings_get_int("controle", "jogadores", 1);
        n = v < 1 ? 1 : v > 4 ? 4 : (uint32_t)v;
        if (n > 1) LOG_I(HLE, "local players: %u", n);
    }
    return n;
}
static int32_t player_of_user(int32_t user) { return user >= USER_ID && user < USER_ID + (int32_t)players() ? user - USER_ID : -1; }
static int32_t player_of_handle(int32_t h) { return h >= PAD_HANDLE && h < PAD_HANDLE + (int32_t)players() ? h - PAD_HANDLE : -1; }

/* ---- libSceUserService ---- */

static void us_ok(hle_call *c) { hle_ret_s32(c, 0); }

static void us_get_initial_user(hle_call *c) { hle_ret_s32(c, write_s32(c, 0, USER_ID, US_ERR_INVALID_ARGUMENT)); }

/* int sceUserServiceGetUserNumber(int user_id, int *number): the player number, from 1
 * (Sonic Frontiers takes number - 1 as the player's pad index: 0 left it at -1, no pad) */
static void us_get_user_number(hle_call *c) {
    int32_t p = player_of_user(hle_arg_s32(c, 0));
    hle_ret_s32(c, p < 0 ? US_ERR_NOT_LOGGED_IN : write_s32(c, 1, p + 1, US_ERR_INVALID_ARGUMENT));
}

/* struct { int user_id[4]; } */
static void us_get_login_user_id_list(hle_call *c) {
    int32_t *list = (int32_t *)hle_mem(c, hle_arg_ptr(c, 0), 16);
    if (!list) {
        hle_ret_s32(c, US_ERR_INVALID_ARGUMENT);
        return;
    }
    for (uint32_t i = 0; i < 4; i++) list[i] = i < players() ? USER_ID + (int32_t)i : -1;
    hle_ret_s32(c, 0);
}

/* int sceUserServiceGetUserName(int user_id, char *name, size_t size) */
static void us_get_user_name(hle_call *c) {
    uint64_t size = hle_arg(c, 2);
    char *name = size ? (char *)hle_mem(c, hle_arg_ptr(c, 1), size) : NULL;
    if (!name || size < sizeof(USER_NAME)) {
        hle_ret_s32(c, US_ERR_INVALID_ARGUMENT);
        return;
    }
    if (player_of_user(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, US_ERR_NOT_LOGGED_IN);
        return;
    }
    int32_t pl = player_of_user(hle_arg_s32(c, 0));
    if (pl == 0) memcpy(name, USER_NAME, sizeof(USER_NAME));
    else snprintf(name, (size_t)size, "Jogador %d", (int)pl + 1);
    hle_ret_s32(c, 0);
}

/* Getters of the form (int user_id, int32_t *value) */
static void us_value(hle_call *c, int32_t value) {
    if (player_of_user(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, US_ERR_NOT_LOGGED_IN);
        return;
    }
    hle_ret_s32(c, write_s32(c, 1, value, US_ERR_INVALID_ARGUMENT));
}

static void us_get_age_level(hle_call *c) { us_value(c, 21); }
static void us_get_zero(hle_call *c) { us_value(c, 0); }
static void us_get_one(hle_call *c) { us_value(c, 1); }

/* int sceUserServiceGetGamePresets(int user_id, presets *p): { size_t this_size; uint32_t fields[8]; } */
static void us_get_game_presets(hle_call *c) {
    uint64_t *p = (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 40);
    if (!p) {
        hle_ret_s32(c, US_ERR_INVALID_ARGUMENT);
        return;
    }
    if (player_of_user(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, US_ERR_NOT_LOGGED_IN);
        return;
    }
    uint64_t size = p[0] && p[0] <= 40 ? p[0] : 40;
    memset(p, 0, (size_t)size);
    p[0] = 40;
    hle_ret_s32(c, 0);
}

/* int sceUserServiceGetEvent(SceUserServiceEvent *ev): { int type; int user_id; }.
 * The system reports the initial user's login once, as the console does at boot. */
static void us_get_event(hle_call *c) {
    static volatile long reported; /* login events reported so far */
    int32_t *ev = (int32_t *)hle_mem(c, hle_arg_ptr(c, 0), 8);
    if (!ev) {
        hle_ret_s32(c, US_ERR_INVALID_ARGUMENT);
        return;
    }
    long n = __atomic_fetch_add(&reported, 1, __ATOMIC_SEQ_CST);
    if (n >= (long)players()) {
        hle_ret_s32(c, US_ERR_NO_EVENT);
        return;
    }
    ev[0] = 0; /* SCE_USER_SERVICE_EVENT_TYPE_LOGIN */
    ev[1] = USER_ID + (int32_t)n;
    hle_ret_s32(c, 0);
}

/* ---- libSceNpManager: the user is signed out of PSN ----
 * [psn] conta=1 (or PS5C_PSN_ACCOUNT=1): the user has a PSN account (account id,
 * online id, country), as a console user offline does, but stays signed out
 * (Dying Light The Beast reopens the sign-in dialog until it gets an account id).
 * conta=2: also signed in, with PSN still unreachable. */

#define NP_STATE_SIGNED_OUT 1
#define NP_STATE_SIGNED_IN 2
#define NP_ACCOUNT_ID 0x5A65726150533501ull

static int np_account_level(void) {
    static int level = -1;
    if (level < 0) {
        const char *env = getenv("PS5C_PSN_ACCOUNT");
        level = env ? (*env ? atoi(env) : 1) : (int)settings_get_int("psn", "conta", 0);
    }
    return level;
}
static bool np_account(void) { return np_account_level() >= 1; }
static bool np_signed_in(void) { return np_account_level() >= 2; }

/* int sceNpGetState(int user_id, int *state) */
static void np_get_state(hle_call *c) {
    hle_ret_s32(c, write_s32(c, 1, np_signed_in() ? NP_STATE_SIGNED_IN : NP_STATE_SIGNED_OUT, NP_ERR_INVALID_ARGUMENT));
}

/* int sceNpGetAccountIdA(int user_id, SceNpAccountId *id) */
static void np_get_account_id(hle_call *c) {
    if (!np_account()) {
        hle_ret_s32(c, NP_ERR_SIGNED_OUT);
        return;
    }
    hle_ret_s32(c, hle_arg_ptr(c, 1) && gmem_write_u64(c->mem, hle_arg_ptr(c, 1), NP_ACCOUNT_ID) ? 0 : NP_ERR_INVALID_ARGUMENT);
}

/* int sceNpGetOnlineId(int user_id, SceNpOnlineId *id): char data[16], term, 3 bytes of padding */
static void np_get_online_id(hle_call *c) {
    if (!np_account()) {
        hle_ret_s32(c, NP_ERR_SIGNED_OUT);
        return;
    }
    char id[20] = "ZeraPS5";
    hle_ret_s32(c, hle_arg_ptr(c, 1) && gmem_write(c->mem, hle_arg_ptr(c, 1), id, sizeof(id)) ? 0 : NP_ERR_INVALID_ARGUMENT);
}

/* int sceNpGetAccountCountryA(int user_id, SceNpCountryCode *cc): char data[2], term, padding */
static void np_get_country(hle_call *c) {
    if (!np_account()) {
        hle_ret_s32(c, NP_ERR_SIGNED_OUT);
        return;
    }
    char cc[4] = "br";
    hle_ret_s32(c, hle_arg_ptr(c, 1) && gmem_write(c->mem, hle_arg_ptr(c, 1), cc, sizeof(cc)) ? 0 : NP_ERR_INVALID_ARGUMENT);
}

/* int sceNpGetNpReachabilityState(int user_id, int *state): 0 = unavailable */
static void np_get_reachability(hle_call *c) { hle_ret_s32(c, write_s32(c, 1, 0, NP_ERR_INVALID_ARGUMENT)); }

/* int sceNpHasSignedUp(int user_id, bool *signed_up): no PSN account unless [psn] conta */
static void np_has_signed_up(hle_call *c) {
    if (!hle_arg_ptr(c, 1)) {
        hle_ret_s32(c, NP_ERR_INVALID_ARGUMENT);
        return;
    }
    gmem_write(c->mem, hle_arg_ptr(c, 1), &(uint8_t){np_account()}, 1);
    hle_ret_s32(c, 0);
}

/* Account queries need a signed-in user. */

/* int sceNpEntitlementAccessGetSkuFlag(SceNpEntitlementAccessSkuFlag *flag): the
 * full game (3; 1 is a trial). GTA V waits on its loading screen without it. */
static void np_ent_get_sku_flag(hle_call *c) { hle_ret_s32(c, write_s32(c, 0, 3, NP_ERR_INVALID_ARGUMENT)); }

/* Entitlements that come with a purchased game (unified entitlement labels):
 * data/entitlements.txt lines "<title id> <label> [<label> ...]" (GTA V's
 * full game includes its Story Mode, GTAVSTORYMODE001), and the game's own
 * settings may add more: [jogo] licencas=LABEL1,LABEL2 */
#define ENT_MAX 32
static char g_ent[ENT_MAX][17];
static uint32_t g_ent_count, g_ent_game; /* the game's own first, then add-ons */
#define ENT_PACKAGE_UNIFIED 3 /* an entitlement with nothing to download */
#define ENT_PACKAGE_PSAC 1    /* add-on content with data */
#define ENT_STATUS_INSTALLED 4

static void ent_add(const char *s, size_t n) {
    if (!n || n > 16 || g_ent_count >= ENT_MAX) return;
    memcpy(g_ent[g_ent_count], s, n);
    g_ent[g_ent_count++][n] = 0;
}

static void ent_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    char title[32];
    if (!settings_param_string("titleId", title, sizeof(title))) title[0] = 0;
    uint8_t *data = NULL;
    uint64_t size = 0;
    if (title[0] && pal_file_read_all("data/entitlements.txt", &data, &size) == PS5C_OK) {
        for (char *line = (char *)data, *end = line + size; line < end;) {
            char *eol = memchr(line, '\n', (size_t)(end - line));
            if (!eol) eol = end;
            size_t tl = strlen(title);
            if (line[0] != '#' && (size_t)(eol - line) > tl && !memcmp(line, title, tl) && (line[tl] == ' ' || line[tl] == '\t')) {
                for (char *p = line + tl; p < eol;) {
                    while (p < eol && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
                    char *w = p;
                    while (p < eol && *p != ' ' && *p != '\t' && *p != '\r') p++;
                    ent_add(w, (size_t)(p - w));
                }
            }
            line = eol + 1;
        }
        free(data);
    }
    const char *extra = settings_get("jogo", "licencas");
    for (const char *p = extra; p && *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && p[0] == ' ') p++, n--;
        while (n && p[n - 1] == ' ') n--;
        ent_add(p, n);
        p = e ? e + 1 : NULL;
    }
    for (uint32_t i = 0; i < g_ent_count; i++) LOG_I(HLE, "entitlement %s (comes with the game)", g_ent[i]);
    g_ent_game = g_ent_count;
    for (uint32_t i = 0; i < addcont_count(); i++) ent_add(addcont_label(i), strlen(addcont_label(i)));
}

/* the entry of entitlement i: add-ons installed under user/dlc are PSAC packages, installed (4) */
static void ent_entry(uint32_t i, uint8_t e[0x1c]) {
    memset(e, 0, 0x1c);
    memcpy(e, g_ent[i], strlen(g_ent[i]));
    int32_t type = i < g_ent_game ? ENT_PACKAGE_UNIFIED : ENT_PACKAGE_PSAC;
    int32_t status = i < g_ent_game ? 0 : ENT_STATUS_INSTALLED;
    memcpy(e + 0x14, &type, 4);
    memcpy(e + 0x18, &status, 4);
}

/* int sceNpEntitlementAccessGetAddcontEntitlementInfoList(SceNpServiceLabel label,
 *     SceNpEntitlementAccessAddcontEntitlementInfo *list, uint32_t list_num, uint32_t *hit_num):
 * entries of 0x1c bytes { char label[17]; pad; s32 package_type (+0x14); s32 download_status (+0x18) };
 * no list: only the count */
static void np_ent_get_addcont_list(hle_call *c) {
    ent_load();
    guest_addr list = hle_arg_ptr(c, 1), hit = hle_arg_ptr(c, 3);
    uint32_t num = hle_arg_u32(c, 2);
    for (uint32_t i = 0; list && i < num && i < g_ent_count; i++) {
        uint8_t e[0x1c];
        ent_entry(i, e);
        gmem_write(c->mem, list + (guest_addr)i * sizeof(e), e, sizeof(e));
    }
    if (hit) gmem_write(c->mem, hit, &g_ent_count, 4);
    hle_ret_s32(c, hit || list ? 0 : NP_ERR_INVALID_ARGUMENT);
}

/* int sceNpEntitlementAccessGetAddcontEntitlementInfo(SceNpServiceLabel label,
 *     const SceNpUnifiedEntitlementLabel *entitlement, SceNpEntitlementAccessAddcontEntitlementInfo *info):
 * one entry of the list above, NOT_FOUND for anything the game does not come with */
#define NP_ENT_ERR_PARAMETER ((int32_t)0x80558003)
#define NP_ENT_ERR_NOT_FOUND ((int32_t)0x80558007)
static void np_ent_get_addcont_info(hle_call *c) {
    ent_load();
    char label[17] = {0};
    guest_addr in = hle_arg_ptr(c, 1), out = hle_arg_ptr(c, 2);
    if (!in || !out || !gmem_read(c->mem, in, label, 16)) {
        hle_ret_s32(c, NP_ENT_ERR_PARAMETER);
        return;
    }
    for (uint32_t i = 0; i < g_ent_count; i++) {
        if (strncmp(g_ent[i], label, 16) != 0) continue;
        uint8_t e[0x1c];
        ent_entry(i, e);
        hle_ret_s32(c, gmem_write(c->mem, out, e, sizeof(e)) ? 0 : NP_ENT_ERR_PARAMETER);
        return;
    }
    hle_ret_s32(c, NP_ENT_ERR_NOT_FOUND);
}

/* Np state callbacks: sceNpRegisterStateCallbackA(cb(user, state, arg), arg) returns a
 * callback id. Signed out they are never called; with [psn] conta the next
 * sceNpCheckCallback reports the signed-in state once to each (conta=2). */
#define NP_STATE_CBS 8
static struct {
    guest_addr fn, arg;
    bool told;
} g_np_cbs[NP_STATE_CBS];
static uint32_t g_np_cb_count;

static void np_register_state_callback(hle_call *c) {
    guest_addr fn = hle_arg_ptr(c, 0);
    if (!fn) {
        hle_ret_s32(c, NP_ERR_INVALID_ARGUMENT);
        return;
    }
    if (g_np_cb_count < NP_STATE_CBS) {
        g_np_cbs[g_np_cb_count].fn = fn;
        g_np_cbs[g_np_cb_count].arg = hle_arg_ptr(c, 1);
        g_np_cbs[g_np_cb_count++].told = false;
    }
    hle_ret_s32(c, (int32_t)g_np_cb_count);
}

static void np_unregister_state_callback(hle_call *c) {
    uint32_t id = hle_arg_u32(c, 0);
    if (id >= 1 && id <= g_np_cb_count) g_np_cbs[id - 1].fn = 0;
    hle_ret_s32(c, 0);
}

/* int sceNpCheckCallback(void) */
static void np_check_callback(hle_call *c) {
    for (uint32_t i = 0; np_signed_in() && i < g_np_cb_count; i++) {
        if (!g_np_cbs[i].fn || g_np_cbs[i].told) continue;
        g_np_cbs[i].told = true;
        LOG_I(HLE, "Np state callback: signed in");
        uint64_t rax;
        runtime_call_nested(c->rt, g_np_cbs[i].fn, USER_ID, NP_STATE_SIGNED_IN, g_np_cbs[i].arg, &rax);
    }
    hle_ret_s32(c, 0);
}
static void np_register_callback(hle_call *c) { hle_ret_s32(c, hle_arg_ptr(c, 0) ? 0 : NP_ERR_INVALID_ARGUMENT); }

/* ---- libSceNpAuth: requests exist, sign-in codes and tokens fail: signed out, or
 * with the emulated sign-in ([psn] conta=2) the service is down, since there is
 * no real code or token to give ---- */

static int32_t np_auth_error(void) { return np_signed_in() ? NP_AUTH_ERR_SERVICE_DOWN : NP_ERR_SIGNED_OUT; }
static void np_auth_unavailable(hle_call *c) { hle_ret_s32(c, np_auth_error()); }
static void np_signed_out(hle_call *c) { hle_ret_s32(c, NP_ERR_SIGNED_OUT); }

static void np_auth_create_request(hle_call *c) {
    static volatile int32_t next;
    hle_ret_s32(c, __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED));
}

/* int sceNpAuthWaitAsync / sceNpAuthPollAsync(int req, int *result): done, unavailable */
static void np_auth_wait_async(hle_call *c) { hle_ret_s32(c, write_s32(c, 1, np_auth_error(), NP_ERR_INVALID_ARGUMENT)); }

static void np_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceNpPollAsync(int req, int *result): finished, unavailable */
#define np_poll_async np_auth_wait_async

/* int sceNpGetUserIdByAccountId(u64 account, int *user) */
static void np_user_by_account(hle_call *c) {
    if (!np_account() || hle_arg(c, 0) != NP_ACCOUNT_ID) {
        hle_ret_s32(c, NP_ERR_INVALID_ARGUMENT);
        return;
    }
    hle_ret_s32(c, write_s32(c, 1, USER_ID, NP_ERR_INVALID_ARGUMENT));
}

/* sceNpEntitlementAccessPoll*(s64 req, int *result, info *): finished, not entitled */
static void np_ent_poll(hle_call *c) {
    write_s32(c, 1, NP_ENT_ERR_NOT_FOUND, 0);
    hle_ret_s32(c, 0);
}
static void np_ent_not_found(hle_call *c) { hle_ret_s32(c, NP_ENT_ERR_NOT_FOUND); }


/* ---- libScePad ---- */

static void pad_init(hle_call *c) { hle_ret_s32(c, 0); }

/* int scePadOpen(int user_id, int type, int index, const void *param) */
static void pad_open(hle_call *c) {
    int32_t p = player_of_user(hle_arg_s32(c, 0));
    hle_ret_s32(c, p >= 0 && hle_arg_s32(c, 2) == 0 ? PAD_HANDLE + p : PAD_ERR_INVALID_ARG);
}

static void pad_get_handle(hle_call *c) {
    int32_t p = player_of_user(hle_arg_s32(c, 0));
    hle_ret_s32(c, p >= 0 ? PAD_HANDLE + p : PAD_ERR_INVALID_ARG);
}

/* ScePadControllerInformation (28 bytes) */
static void pad_get_controller_information(hle_call *c) {
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 28);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!info) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    memset(info, 0, 28);
    float density = 44.86f;
    uint16_t res[2] = {1920, 943};
    memcpy(info, &density, 4);
    memcpy(info + 4, res, 4);
    info[8] = info[9] = 13; /* stick dead zones */
    info[11] = 1;          /* connected count */
    info[12] = 1;          /* connected */
    hle_ret_s32(c, 0);
}

/* int scePadDeviceClassGetExtendedInformation(int handle, info *i): 20 bytes, standard controller */
static void pad_device_class_ext_info(hle_call *c) {
    void *info = hle_mem(c, hle_arg_ptr(c, 1), 20);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!info) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    memset(info, 0, 20);
    hle_ret_s32(c, 0);
}

/* int scePadIsRemoteController(handle, bool *remote): never (Remote Play) */
static void pad_is_remote_controller(hle_call *c) {
    uint8_t *remote = hle_mem(c, hle_arg_ptr(c, 1), 1);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!remote) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    *remote = 0;
    hle_ret_s32(c, 0);
}

/* scePadDeviceClassParseData(handle, const ScePadData *, ScePadDeviceClassData *):
 * a standard controller (class 0) with no class data (bDataValid at 4 false) */
static void pad_device_class_parse_data(hle_call *c) {
    uint8_t *out = hle_mem(c, hle_arg_ptr(c, 2), 8);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!out || !hle_arg_ptr(c, 1)) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    memset(out, 0, 8);
    hle_ret_s32(c, 0);
}

/* ScePadData (120 bytes): u32 buttons; u8 lx, ly, rx, ry, l2, r2; orientation
 * quaternion at 12; ...; connected at 76; timestamp at 80; connected count at 104.
 * The state comes from the host devices (pal_input_read). */
/* Debugging aid: PS5C_PAD_SCRIPT="12:cross,15.5:down,f880:cross" holds each
 * button (150 ms, two reads at least) at that many seconds after the first pad read, or ("f")
 * once that many frames have been flipped, on top of the host devices (drives
 * menus without touching the host keyboard). ls_up, ls_down, ls_left, ls_right
 * and the rs_ ones push a stick all the way.
 * PS5C_PAD_SCRIPT=@<file> reads an append-only sequence from a local file,
 * terminated by a newline. Appending f0:<button> issues a live pulse.
 * Optional /seconds holds a command for up to 300 seconds, e.g. f0:l2/3.
 * shake supplies motion sensor input; it still requires the guest to enable sensors.
 * PS5C_PAD_SCRIPT_ONLY=1 ignores host devices during scripted tests. */
static pal_mutex *g_pad_script_lock; /* file mode only; initialized before guest threads */
static bool g_pad_script_only; /* initialized before guest threads; opt-in */

/* Caller holds g_pad_script_lock. Keep accepted entries at stable indexes:
 * their pulse timestamps/read counts must survive file refreshes. */
static const char *pad_script_source(const char *setting) {
    static char accepted[8192], candidate[8192];
    static size_t length;
    static uint64_t next_read;
    if (!setting || setting[0] != '@') return setting;
    if (!g_pad_script_lock) return NULL;
    uint64_t now = pal_time_ns();
    if (now < next_read) return accepted;
    next_read = now + 100000000ull;
    pal_file *file = NULL;
    uint64_t size = 0, read = 0;
    if (pal_file_open(setting + 1, PAL_FILE_READ, &file) != PS5C_OK) return accepted;
    bool ok = pal_file_size(file, &size) == PS5C_OK && size > 0 && size < sizeof(candidate) &&
              pal_file_read(file, candidate, size, &read) == PS5C_OK && read == size;
    pal_file_close(file);
    /* Ignore partial writes, embedded NULs and replacements of old entries.
     * Writers should replace the file atomically with the appended sequence. */
    if (!ok || candidate[size - 1] != '\n' || memchr(candidate, 0, (size_t)size)) return accepted;
    size_t n = (size_t)size;
    while (n && (candidate[n - 1] == '\n' || candidate[n - 1] == '\r')) n--;
    candidate[n] = 0;
    if (n < length || memcmp(candidate, accepted, length) != 0 ||
        (length && n > length && candidate[length] != ',')) return accepted;
    if (n > length) {
        memcpy(accepted, candidate, n + 1);
        length = n;
        LOG_I(INPUT, "pad script: accepted %zu bytes from the test command file", length);
    }
    return accepted;
}
static uint32_t pad_script_buttons_locked(void) {
    static const char *script;
    static uint64_t start;
    if (!start) {
        start = pal_time_ns();
        script = getenv("PS5C_PAD_SCRIPT");
    }
    const char *source = pad_script_source(script);
    if (!source) return 0;
    static double frame_hit[1024]; /* when each frame entry's count was reached (0: not yet) */
    static uint64_t pulse_flip[1024];
    static bool pulse_started[1024];
    double now = (double)(pal_time_ns() - start) / 1e9;
    uint64_t flips = video_out_flip_count();
    uint32_t buttons = 0, entry = 0;
    for (const char *p = source; *p; entry++) {
        char *end;
        bool frames = *p == 'f';
        double t = strtod(p + frames, &end);
        if (end == p + frames || *end != ':' || !isfinite(t) || t < 0 || t >= 18446744073709551616.0) break;
        p = end + 1;
        size_t n = strcspn(p, ",");
        double duration;
        uint32_t hold_flips;
        uint32_t parsed = pad_script_parse_hold(p, n, &duration, &hold_flips);
        if (frames && entry < 1024) {
            if (!frame_hit[entry] && flips >= (uint64_t)t) frame_hit[entry] = now > 0 ? now : 1e-9;
            t = frame_hit[entry] ? frame_hit[entry] : 1e30;
        }
        /* held 150 ms and for at least two reads: slow frames poll rarely */
        static uint8_t reads[1024];
        if (now >= t && entry < 1024 && !pulse_started[entry]) {
            pulse_flip[entry] = flips;
            pulse_started[entry] = true;
        }
        bool held = pad_script_held(now, t, duration, hold_flips,
            entry < 1024 ? pulse_flip[entry] : flips, flips,
            entry < 1024 ? reads[entry] : 2);
        if (hold_flips && entry >= 1024) held = false;
        if (held && entry < 1024 && reads[entry] < 255) reads[entry]++;
        if (held) buttons |= parsed;
        p += n;
        if (*p == ',') p++;
    }
    return buttons;
}

/* Sensors are available at open; a guest may explicitly disable its own pad.
 * Reads and setters can run on different guest threads. */
static atomic_bool g_pad_motion[4] = {true,true,true,true};

static uint32_t pad_script_buttons(void) {
    if (g_pad_script_lock) pal_mutex_lock(g_pad_script_lock);
    uint32_t buttons = pad_script_buttons_locked();
    if (g_pad_script_lock) pal_mutex_unlock(g_pad_script_lock);
    return buttons;
}

static void fill_pad_data(uint8_t *d, int32_t player) {
    pal_pad_state in;
    bool connected = true;
    if (player > 0) { /* another local player: no keyboard, no script */
        memset(&in, 0, sizeof(in));
        in.lx = in.ly = in.rx = in.ry = 128;
        connected = !g_pad_script_only && pal_input_read_player((uint32_t)player, &in);
    } else if (g_pad_script_only) {
        memset(&in, 0, sizeof(in));
        in.lx = in.ly = in.rx = in.ry = 128;
    } else {
        pal_input_read(&in);
        static char device[64] = "?";
        const char *now = pal_input_device();
        if (strcmp(device, now)) {
            snprintf(device, sizeof(device), "%s", now);
            LOG_I(HLE, "pad: host controller %s", *now ? now : "none (keyboard)");
        }
    }
    uint32_t script = player > 0 ? 0 : pad_script_buttons();
    pad_script_apply(&in, script, pal_time_ns());
    /* A click without a finger (keyboard, XInput Back) gets one: on the right half,
     * or the left for its own key (PSPHD and PS1HD: right START, left SELECT). */
    bool left = in.buttons & PAL_PAD_TOUCH_LEFT;
    in.buttons &= ~(uint32_t)PAL_PAD_TOUCH_LEFT;
    if (left) in.buttons |= PAL_PAD_TOUCH_PAD;
    if ((in.buttons & PAL_PAD_TOUCH_PAD) && !in.touch_count) {
        in.touch[0].x = left ? 480 : 1440;
        in.touch[0].y = 470;
        in.touch[0].id = 0x7f;
        in.touch_count = 1;
    }
    memset(d, 0, 120);
    memcpy(d, &in.buttons, 4);
    d[4] = in.lx;
    d[5] = in.ly;
    d[6] = in.rx;
    d[7] = in.ry;
    d[8] = in.l2;
    d[9] = in.r2;
    float w = 1.0f;
    memcpy(d + 24, &w, 4); /* orientation quaternion w */
    memcpy(d + 32, &w, 4); /* rest acceleration: gravity along +Y */
    if (in.motion && atomic_load_explicit(&g_pad_motion[player],memory_order_relaxed)) {
        memcpy(d + 28, in.accel, 12); /* acceleration (G) */
        memcpy(d + 40, in.gyro, 12);  /* angular velocity (rad/s) */
    }
    /* touch: count at 52, then two points {u16 x, u16 y, u8 id, 3 reserved} at 60 */
    d[52] = in.touch_count;
    for (uint32_t i = 0; i < in.touch_count && i < 2; i++) {
        memcpy(d + 60 + 8 * i, &in.touch[i].x, 2);
        memcpy(d + 62 + 8 * i, &in.touch[i].y, 2);
        d[64 + 8 * i] = in.touch[i].id;
    }
    d[76] = connected;     /* connected */
    uint64_t ts = pal_time_ns() / 1000u;
    memcpy(d + 80, &ts, 8);
    d[104] = 1; /* connected count */
}

static void pad_read_state(hle_call *c) {
    uint8_t *d = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 120);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!d) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    fill_pad_data(d, player_of_handle(hle_arg_s32(c, 0)));
    hle_ret_s32(c, 0);
}

/* int scePadRead(int handle, ScePadData *data, int num): returns entries written */
static void pad_read(hle_call *c) {
    int32_t num = hle_arg_s32(c, 2);
    uint8_t *d = num > 0 ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 120) : NULL;
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!d) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    fill_pad_data(d, player_of_handle(hle_arg_s32(c, 0)));
    hle_ret_s32(c, 1);
}

static void pad_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* Unknown status query, not a modelled pad feature. The observed caller passes
 * a handle and an uninitialised four-byte stack output, then reads it only on
 * success. Reject explicitly instead of the generic success/no-output stub.
 * The ABI/name and state enum are unverified: do not invent an output value. */
static void pad_unknown_status_unsupported(hle_call *c) {
    hle_ret_s32(c, SCE_KERNEL_ERROR_ENOSYS);
}

/* Setters of the one controller: int scePadSetXxx(int handle, ...) */
static void pad_handle_ok(hle_call *c) { hle_ret_s32(c, player_of_handle(hle_arg_s32(c, 0)) >= 0 ? 0 : PAD_ERR_INVALID_HANDLE); }

/* int scePadGetTriggerEffectState(int handle, {s32 state[2]} *info): no effect running */
static void pad_get_trigger_effect_state(hle_call *c) {
    int32_t *info = (int32_t *)hle_mem(c, hle_arg_ptr(c, 1), 8);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!info) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    info[0] = info[1] = 0;
    hle_ret_s32(c, 0);
}


/* int scePadSetMotionSensorState(int handle, bool enable) */
static void pad_set_motion(hle_call *c) {
    int32_t player=player_of_handle(hle_arg_s32(c,0));
    if(player<0) {hle_ret_s32(c,PAD_ERR_INVALID_HANDLE);return;}
    atomic_store_explicit(&g_pad_motion[player],hle_arg_s32(c,1)!=0,memory_order_relaxed);
    hle_ret_s32(c, 0);
}

/* int scePadSetVibration(int handle, const {u8 large, u8 small} *param): [controle] vibracao=0 turns it off */
static void pad_set_vibration(hle_call *c) {
    const uint8_t *v = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 2);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!v) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    static int on = -1;
    if (on < 0) on = settings_get_int("controle", "vibracao", 1) != 0;
    if (on && player_of_handle(hle_arg_s32(c, 0)) == 0) pal_input_set_rumble(v[0], v[1]);
    hle_ret_s32(c, 0);
}

/* int scePadSetLightBar(int handle, const {u8 r, g, b, reserved} *param) */
static void pad_set_light_bar(hle_call *c) {
    const uint8_t *v = (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), 3);
    if (player_of_handle(hle_arg_s32(c, 0)) < 0) {
        hle_ret_s32(c, PAD_ERR_INVALID_HANDLE);
        return;
    }
    if (!v) {
        hle_ret_s32(c, PAD_ERR_INVALID_ARG);
        return;
    }
    if (player_of_handle(hle_arg_s32(c, 0)) == 0) pal_input_set_light(v[0], v[1], v[2]);
    hle_ret_s32(c, 0);
}

/* int scePadResetLightBar(int handle): the user's color (player 1 blue) */
static void pad_reset_light_bar(hle_call *c) {
    if (player_of_handle(hle_arg_s32(c, 0)) == 0) pal_input_set_light(0, 0, 255);
    hle_ret_s32(c, player_of_handle(hle_arg_s32(c, 0)) >= 0 ? 0 : PAD_ERR_INVALID_HANDLE);
}

/* int sceSystemServiceHideSplashScreen(void) */
/* int sceSystemServiceGetNoticeScreenSkipFlag(bool *skip): the health notice is shown */
static void ss_get_notice_screen_skip_flag(hle_call *c) {
    uint8_t *out = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 1);
    if (out) *out = 0;
    hle_ret_s32(c, out ? 0 : (int32_t)0x80A10003);
}

/* int sceSystemServiceParamGetString(int id, char *buf, size_t size): 6 = the system name */
static void ss_param_get_string(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0);
    uint64_t size = hle_arg(c, 2);
    char *buf = size ? (char *)hle_mem(c, hle_arg_ptr(c, 1), size) : NULL;
    if (!buf) {
        hle_ret_s32(c, (int32_t)0x80A10003);
        return;
    }
    if (id != 6) {
        LOG_W(HLE, "sceSystemServiceParamGetString: unknown parameter %d", id);
        hle_ret_s32(c, (int32_t)0x80A10003);
        return;
    }
    snprintf(buf, (size_t)size, "%s", "PS5");
    hle_ret_s32(c, 0);
}


static void ss_hide_splash(hle_call *c) {
    video_out_hide_splash();
    hle_ret_s32(c, 0);
}

/* ---- libSceSystemService (per-frame status) ---- */

/* int sceSystemServiceGetDisplaySafeAreaInfo(SceSystemServiceDisplaySafeAreaInfo *info):
 * { float ratio; u8 reserved[128]; } - the whole screen is safe (games scale
 * their HUD by the ratio: 0 shrinks it to nothing) */
static void ss_get_display_safe_area_info(hle_call *c) {
    uint8_t *info = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 132);
    if (!info) {
        hle_ret_s32(c, (int32_t)0x80A10003); /* SCE_SYSTEM_SERVICE_ERROR_PARAMETER */
        return;
    }
    memset(info, 0, 132);
    float ratio = 1.0f;
    memcpy(info, &ratio, 4);
    hle_ret_s32(c, 0);
}

/* int sceSystemServiceGetStatus(SceSystemServiceStatus *status): no pending
 * events, not overlaid, in the foreground */
static void ss_get_status(hle_call *c) {
    uint8_t *st = (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 12);
    if (!st) {
        hle_ret_s32(c, (int32_t)0x80A10003); /* invalid parameter */
        return;
    }
    memset(st, 0, 12);
    hle_ret_s32(c, 0);
}

/* int sceSystemServiceGetHdrToneMapLuminance(SceSystemServiceHdrToneMapLuminance *out):
 * {float max_full_frame, max, min} in nits */
static void ss_get_hdr_tone_map_luminance(hle_call *c) {
    float *out = (float *)hle_mem(c, hle_arg_ptr(c, 0), 12);
    if (!out) {
        hle_ret_s32(c, (int32_t)0x80A10003);
        return;
    }
    out[0] = 1000.0f;
    out[1] = 1000.0f;
    out[2] = 0.01f;
    hle_ret_s32(c, 0);
}

/* System language: the SCE code (0 Japanese, 1 English US, 2 French,
 * 3 Spanish, 4 German, 5 Italian, 7 Portuguese, 17 Portuguese Brazil,
 * 18 English UK, ...) of the user settings ([geral] idioma, Portuguese
 * (Brazil) by default); PS5C_LANGUAGE overrides it. */
static int32_t system_language(void) {
    static int32_t lang = -1;
    if (lang < 0) {
        const char *env = getenv("PS5C_LANGUAGE");
        lang = env && *env ? atoi(env) : (int32_t)settings_get_int("geral", "idioma", USER_LANGUAGE_DEFAULT);
        if (lang < 0 || lang > 30) lang = USER_LANGUAGE_DEFAULT;
    }
    return lang;
}

/* int sceSystemServiceParamGetInt(int param_id, int *value): a Brazilian
 * console by default, 24-hour clock, UTC, cross confirms. */
static void ss_param_get_int(hle_call *c) {
    int32_t id = hle_arg_s32(c, 0), v;
    switch (id) {
    case 1: v = system_language(); break;
    case 2: v = 0; break;    /* date format: YYYYMMDD */
    case 3: v = 1; break;    /* time format: 24 hour */
    case 4: v = 0; break;    /* time zone offset, minutes */
    case 5: v = 0; break;    /* daylight saving */
    case 7: v = 0; break;    /* game parental level: off */
    case 1000: v = 1; break; /* enter button: cross */
    default:
        LOG_W(HLE, "sceSystemServiceParamGetInt: unknown parameter %d", id);
        hle_ret_s32(c, (int32_t)0x80A10003);
        return;
    }
    hle_ret_s32(c, write_s32(c, 1, v, (int32_t)0x80A10003));
}

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceInitialize", us_ok),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceInitialize2", us_ok),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceTerminate", us_ok),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetInitialUser", us_get_initial_user),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetLoginUserIdList", us_get_login_user_id_list),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetUserName", us_get_user_name),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAgeLevel", us_get_age_level),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetGamePresets", us_get_game_presets),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetEvent", us_get_event),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetUserNumber", us_get_user_number),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAccessibilityChatTranscription", us_get_zero),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAccessibilityPressAndHoldDelay", us_get_zero),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAccessibilityTriggerEffect", us_get_one),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAccessibilityVibration", us_get_one),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAccessibilityZoomEnabled", us_get_zero),
    HLE_FUNC_ENTRY("libSceUserService", "sceUserServiceGetAccessibilityZoomFollowFocus", us_get_zero),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCreateRequest", np_auth_create_request),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCreateAsyncRequest", np_auth_create_request),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpDeleteRequest", np_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpAbortRequest", np_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpPollAsync", np_poll_async),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCheckPremium", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCheckPlus", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpNotifyPremiumFeature", np_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCheckNpReachability", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCheckNpAvailabilityA", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetAccountLanguage2", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetAccountAge", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetUserIdByAccountId", np_user_by_account),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessDeleteRequest", np_ok),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessAbortRequest", np_ok),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessGetEntitlementKey", np_ent_not_found),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessGenerateTransactionId", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessRequestUnifiedEntitlementInfo", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessRequestServiceEntitlementInfo", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessRequestConsumeUnifiedEntitlement", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessRequestConsumeServiceEntitlement", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessPollUnifiedEntitlementInfo", np_ent_poll),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessPollServiceEntitlementInfo", np_ent_poll),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessPollConsumeEntitlement", np_ent_poll),
    HLE_FUNC_ENTRY("libSceNpUtility", "sceNpBandwidthTestInitStart", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpUtility", "sceNpBandwidthTestInitStartDownload", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpUtility", "sceNpBandwidthTestInitStartUpload", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpUtility", "sceNpBandwidthTestGetStatus", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpUtility", "sceNpBandwidthTestShutdown", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpUtility", "sceNpBandwidthTestAbort", np_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetState", np_get_state),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessGetSkuFlag", np_ent_get_sku_flag),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessGetAddcontEntitlementInfoList", np_ent_get_addcont_list),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetNpReachabilityState", np_get_reachability),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetAccountIdA", np_get_account_id),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetAccountCountryA", np_get_country),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpGetOnlineId", np_get_online_id),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpHasSignedUp", np_has_signed_up),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthCreateRequest", np_auth_create_request),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthCreateAsyncRequest", np_auth_create_request),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthDeleteRequest", np_ok),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthAbortRequest", np_ok),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthGetAuthorizationCode", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthGetAuthorizationCodeV3", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthGetIdToken", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthGetIdTokenV3", np_auth_unavailable),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthWaitAsync", np_auth_wait_async),
    HLE_FUNC_ENTRY("libSceNpAuth", "sceNpAuthPollAsync", np_auth_wait_async),
    HLE_FUNC_ENTRY("libScePad", "scePadInit", pad_init),
    HLE_FUNC_ENTRY("libScePad", "scePadOpen", pad_open),
    HLE_FUNC_ENTRY("libScePad", "scePadClose", pad_ok),
    HLE_FUNC_ENTRY("libScePad", "scePadGetHandle", pad_get_handle),
    HLE_FUNC_ENTRY("libScePad", "scePadGetControllerInformation", pad_get_controller_information),
    HLE_FUNC_ENTRY("libScePad", "scePadDeviceClassGetExtendedInformation", pad_device_class_ext_info),
    HLE_FUNC_ENTRY("libScePad", "scePadDeviceClassParseData", pad_device_class_parse_data),
    HLE_FUNC_ENTRY("libScePad", "scePadIsRemoteController", pad_is_remote_controller),
    HLE_FUNC_ENTRY("libScePad", "#zBsgF0q8DIM", pad_unknown_status_unsupported),
    HLE_FUNC_ENTRY("libScePad", "scePadReadState", pad_read_state),
    HLE_FUNC_ENTRY("libScePad", "scePadRead", pad_read),
    HLE_FUNC_ENTRY("libScePad", "scePadSetMotionSensorState", pad_set_motion),
    HLE_FUNC_ENTRY("libScePad", "scePadSetLightBar", pad_set_light_bar),
    HLE_FUNC_ENTRY("libScePad", "scePadResetLightBar", pad_reset_light_bar),
    HLE_FUNC_ENTRY("libScePad", "scePadSetVibration", pad_set_vibration),
    HLE_FUNC_ENTRY("libScePad", "scePadSetVibrationMode", pad_ok),
    HLE_FUNC_ENTRY("libScePad", "scePadSetTriggerEffect", pad_ok),
    HLE_FUNC_ENTRY("libScePad", "scePadResetOrientation", pad_ok),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceGetStatus", ss_get_status),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo", ss_get_display_safe_area_info),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceParamGetInt", ss_param_get_int),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceGetHdrToneMapLuminance", ss_get_hdr_tone_map_luminance),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceHideSplashScreen", ss_hide_splash),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceGetNoticeScreenSkipFlag", ss_get_notice_screen_skip_flag),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceDisableNoticeScreenSkipFlagAutoSet", us_ok),
    HLE_FUNC_ENTRY("libSceSystemService", "sceSystemServiceParamGetString", ss_param_get_string),
    HLE_FUNC_ENTRY("libScePad", "scePadGetTriggerEffectState", pad_get_trigger_effect_state),
    HLE_FUNC_ENTRY("libScePad", "scePadSetTiltCorrectionState", pad_handle_ok),
    HLE_FUNC_ENTRY("libScePad", "scePadSetAngularVelocityDeadbandState", pad_handle_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpCheckCallback", np_check_callback),
    /* the PS4 NpToolkit's private entry points into NpManager / UserService */
    HLE_FUNC_ENTRY("libSceNpManagerForToolkit", "sceNpCheckCallbackForLib", np_check_callback),
    HLE_FUNC_ENTRY("libSceNpManagerForToolkit", "sceNpRegisterStateCallbackForToolkit", np_register_state_callback),
    HLE_FUNC_ENTRY("libSceNpManagerForToolkit", "sceNpUnregisterStateCallbackForToolkit", np_unregister_state_callback),
    HLE_FUNC_ENTRY("libSceUserServiceForNpToolkit", "sceUserServiceRegisterEventCallback", np_register_callback),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpSetNpTitleId", np_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpRegisterStateCallbackA", np_register_state_callback),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpUnregisterStateCallbackA", np_unregister_state_callback),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpRegisterNpReachabilityStateCallback", np_register_callback),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpUnregisterNpReachabilityStateCallback", np_ok),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpRegisterPremiumEventCallback", np_register_callback),
    HLE_FUNC_ENTRY("libSceNpManager", "sceNpUnregisterPremiumEventCallback", np_ok),
    /* PSN-backed requests cannot start without a signed-in network session.
     * Returning success here would advertise a request/result never created. */
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessRequestUnifiedEntitlementInfoList", np_signed_out),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessPollUnifiedEntitlementInfoList", np_signed_out),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessRequestServiceEntitlementInfoList", np_signed_out),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessPollServiceEntitlementInfoList", np_signed_out),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessInitialize", np_ok),
    HLE_FUNC_ENTRY("libSceNpEntitlementAccess", "sceNpEntitlementAccessGetAddcontEntitlementInfo", np_ent_get_addcont_info),
};

void hle_register_services(void) {
    const char *script = getenv("PS5C_PAD_SCRIPT");
    const char *only = getenv("PS5C_PAD_SCRIPT_ONLY");
    g_pad_script_only = script && script[0] && only && strcmp(only, "1") == 0;
    if (g_pad_script_only) LOG_I(INPUT, "pad script: host controls disabled for this test");
    if (script && script[0] == '@' && !g_pad_script_lock &&
        pal_mutex_create(&g_pad_script_lock) != PS5C_OK)
        LOG_W(INPUT, "pad script file disabled: cannot create its lock");
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
    hle_register_input();
}
