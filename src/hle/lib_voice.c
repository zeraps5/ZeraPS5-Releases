/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceVoice: voice chat ports (microphone and headset streams, encoded
 * voice). There is no voice chat: ports exist with their type, bit rate and
 * frame size so the game's setup succeeds (GTA V divides by the bit rate and
 * retries GetPortInfo forever while it fails), reads return no data and writes
 * are dropped. */
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define VOICE_ERR_ARGUMENT_INVALID ((int32_t)0x8023310a)
#define VOICE_ERR_PORT_INVALID ((int32_t)0x8023310b)
#define VOICE_ERR_RESOURCE_INSUFFICIENT ((int32_t)0x80233104)

#define MAX_PORTS 64
#define DEFAULT_BITRATE 16000 /* bits per second */
#define FRAME_MS 20           /* encoded voice frames */
#define PCM_FRAME_BYTES 640   /* 20 ms of 16-bit mono at 16 kHz */

/* SceVoicePortType */
enum { PORT_IN_DEVICE = 0, PORT_IN_PCMAUDIO, PORT_IN_VOICE, PORT_OUT_PCMAUDIO, PORT_OUT_VOICE, PORT_OUT_DEVICE };

typedef struct voice_port {
    bool used;
    int32_t type;
    uint32_t bitrate;
} voice_port;

static struct {
    pal_mutex *lock;
    voice_port ports[MAX_PORTS];
} g_voice;

static voice_port *port(uint32_t id) { return id < MAX_PORTS && g_voice.ports[id].used ? &g_voice.ports[id] : NULL; }

static uint32_t frame_bytes(const voice_port *p) {
    bool voice = p->type == PORT_IN_VOICE || p->type == PORT_OUT_VOICE;
    return voice ? p->bitrate * FRAME_MS / 8000 : PCM_FRAME_BYTES;
}

static void voice_ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int sceVoiceCreatePort(uint32_t *portId, const SceVoicePortParam *param):
 * param = { int32 portType; u16 threshold; u16 bMute; float volume; union {
 * voice: { SceVoiceBitRate bitrate } ... } at +0x0c } */
static void voice_create_port(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0), param = hle_arg_ptr(c, 1);
    int32_t type = 0;
    uint32_t bitrate = 0;
    if (!out || !param || !gmem_read(c->mem, param, &type, 4)) {
        hle_ret_s32(c, VOICE_ERR_ARGUMENT_INVALID);
        return;
    }
    if (type == PORT_IN_VOICE || type == PORT_OUT_VOICE) gmem_read(c->mem, param + 0x0c, &bitrate, 4);
    if (!bitrate || bitrate > 64000) bitrate = DEFAULT_BITRATE;
    pal_mutex_lock(g_voice.lock);
    uint32_t id = 0;
    while (id < MAX_PORTS && g_voice.ports[id].used) id++;
    if (id < MAX_PORTS) g_voice.ports[id] = (voice_port){true, type, bitrate};
    pal_mutex_unlock(g_voice.lock);
    if (id == MAX_PORTS) {
        hle_ret_s32(c, VOICE_ERR_RESOURCE_INSUFFICIENT);
        return;
    }
    gmem_write(c->mem, out, &id, 4);
    LOG_D(HLE, "sceVoiceCreatePort: type %d, bit rate %u -> port %u", type, bitrate, id);
    hle_ret_s32(c, 0);
}

static void voice_delete_port(hle_call *c) {
    pal_mutex_lock(g_voice.lock);
    voice_port *p = port(hle_arg_u32(c, 0));
    if (p) p->used = false;
    pal_mutex_unlock(g_voice.lock);
    hle_ret_s32(c, p ? 0 : VOICE_ERR_PORT_INVALID);
}

/* int sceVoiceGetPortInfo(uint32_t portId, SceVoiceBasePortInfo *info):
 * { int32 portType; int32 state; u32 *pEdge; u32 numByte; u32 frameSize;
 *   u16 numEdge; u16 reserved } */
static void voice_get_port_info(hle_call *c) {
    const voice_port *p = port(hle_arg_u32(c, 0));
    guest_addr out = hle_arg_ptr(c, 1);
    if (!p || !out) {
        hle_ret_s32(c, p ? VOICE_ERR_ARGUMENT_INVALID : VOICE_ERR_PORT_INVALID);
        return;
    }
    uint8_t info[0x20];
    memset(info, 0, sizeof(info));
    int32_t state = 1; /* SCE_VOICE_PORTSTATE_READY */
    uint32_t frame = frame_bytes(p);
    memcpy(info, &p->type, 4);
    memcpy(info + 4, &state, 4);
    memcpy(info + 0x14, &frame, 4);
    gmem_write(c->mem, out, info, sizeof(info));
    hle_ret_s32(c, 0);
}

/* int sceVoiceGetBitRate(uint32_t portId, uint32_t *bitrate) */
static void voice_get_bit_rate(hle_call *c) {
    const voice_port *p = port(hle_arg_u32(c, 0));
    guest_addr out = hle_arg_ptr(c, 1);
    uint32_t rate = p ? p->bitrate : DEFAULT_BITRATE;
    if (out) gmem_write(c->mem, out, &rate, 4);
    hle_ret_s32(c, out ? 0 : VOICE_ERR_ARGUMENT_INVALID);
}

/* int sceVoiceSetBitRate(uint32_t portId, SceVoiceBitRate bitrate) */
static void voice_set_bit_rate(hle_call *c) {
    pal_mutex_lock(g_voice.lock);
    voice_port *p = port(hle_arg_u32(c, 0));
    uint32_t rate = hle_arg_u32(c, 1);
    if (p && rate && rate <= 64000) p->bitrate = rate;
    pal_mutex_unlock(g_voice.lock);
    hle_ret_s32(c, p ? 0 : VOICE_ERR_PORT_INVALID);
}

/* int sceVoiceReadFromOPort(uint32_t portId, void *data, uint32_t *size): no voice */
static void voice_read_from_oport(hle_call *c) {
    guest_addr size = hle_arg_ptr(c, 2);
    if (size) gmem_write(c->mem, size, &(uint32_t){0}, 4);
    hle_ret_s32(c, 0);
}

/* int sceVoiceWriteToIPort(uint32_t portId, const void *data, uint32_t *size, int16_t frameGaps): dropped */
static void voice_write_to_iport(hle_call *c) { hle_ret_s32(c, 0); }

#define V "libSceVoice"

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(V, "sceVoiceInit", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceInitHQ", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceEnd", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceStart", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceStop", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceSetThreadsParams", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceCreatePort", voice_create_port),
    HLE_FUNC_ENTRY(V, "sceVoiceDeletePort", voice_delete_port),
    HLE_FUNC_ENTRY(V, "sceVoiceConnectIPortToOPort", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceDisconnectIPortFromOPort", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceGetPortInfo", voice_get_port_info),
    HLE_FUNC_ENTRY(V, "sceVoiceGetBitRate", voice_get_bit_rate),
    HLE_FUNC_ENTRY(V, "sceVoiceSetBitRate", voice_set_bit_rate),
    HLE_FUNC_ENTRY(V, "sceVoiceGetMuteFlag", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceSetMuteFlag", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceSetMuteFlagAll", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceGetVolume", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceSetVolume", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoicePausePort", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceResumePort", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoicePausePortAll", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceResumePortAll", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceResetPort", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceReadFromOPort", voice_read_from_oport),
    HLE_FUNC_ENTRY(V, "sceVoiceWriteToIPort", voice_write_to_iport),
    HLE_FUNC_ENTRY(V, "sceVoiceUpdatePort", voice_ok),
    HLE_FUNC_ENTRY(V, "sceVoiceCheckTopology", voice_ok),
};

void hle_register_voice(void) {
    if (!g_voice.lock && pal_mutex_create(&g_voice.lock) != PS5C_OK) {
        LOG_E(HLE, "cannot create the voice lock");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
