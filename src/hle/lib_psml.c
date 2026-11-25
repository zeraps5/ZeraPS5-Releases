/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libScePsml: PlayStation Spectral Super Resolution (PSSR), the PS5 Pro's ML
 * upscaler (MFSR and MFSR2). Used only when the console's libScePsml.sprx is
 * not in a module folder (policy AUTO: the original module wins).
 *
 * Behavior is that of the console library outside PS5 Pro mode, read from
 * libScePsml.sprx v1.1: scePsmlMfsrInit and scePsmlMfsr2Init print
 * "MFSR isn't supported in sceKernelIsTrinityMode() == 0" and fail with
 * 0x8A810012 / 0x8A8A0012 without becoming initialized, so every other MFSR
 * call answers NOT_INITIALIZED (0x8A810001 / 0x8A8A0001) and the game keeps
 * its own upscaler. The option setters (quality 8K, lightweight, specular AA,
 * no-submission) only set flags and return 0.
 *
 * In PS5 Pro mode ([geral] console=1) the console library would build the
 * upscaler's GPU work; this one cannot, so it still answers "not supported"
 * and logs that PSSR needs the console's libScePsml.sprx. */
#include "base/log.h"
#include "hle/hle.h"

void hle_register_table(const hle_export *table, size_t count);

#define MFSR_ERROR_NOT_INITIALIZED ((int32_t)0x8A810001)
#define MFSR_ERROR_INVALID_VALUE ((int32_t)0x8A81000D)
#define MFSR_ERROR_NOT_SUPPORTED ((int32_t)0x8A810012)
#define MFSR2_ERROR_NOT_INITIALIZED ((int32_t)0x8A8A0001)
#define MFSR2_ERROR_NOT_SUPPORTED ((int32_t)0x8A8A0012)

static void unsupported_log(const char *which) {
    LOG_I(HLE, "[PSML] ### libScePsml.sprx Release version load ###");
    if (hle_kernel_trinity_mode())
        LOG_W(HLE, "[PSML] %s: PSSR needs the console's libScePsml.sprx in the system folder; the game "
                   "keeps its own upscaler", which);
    else
        LOG_I(HLE, "[PSML] %s isn't supported in sceKernelIsTrinityMode() == 0", which);
}

/* int scePsmlMfsrInit(void) */
static void mfsr_init(hle_call *c) {
    unsupported_log("MFSR");
    hle_ret_s32(c, MFSR_ERROR_NOT_SUPPORTED);
}

/* int scePsmlMfsr2Init(void) */
static void mfsr2_init(hle_call *c) {
    unsupported_log("MFSR2");
    hle_ret_s32(c, MFSR2_ERROR_NOT_SUPPORTED);
}

static void mfsr_not_initialized(hle_call *c) { hle_ret_s32(c, MFSR_ERROR_NOT_INITIALIZED); }
static void mfsr2_not_initialized(hle_call *c) { hle_ret_s32(c, MFSR2_ERROR_NOT_INITIALIZED); }
static void ok(hle_call *c) { hle_ret_s32(c, 0); }

/* int (const char *version): the MFSR version to use (7 characters kept); a null one is invalid */
static void set_mfsr_version(hle_call *c) { hle_ret_s32(c, hle_arg_ptr(c, 0) ? 0 : MFSR_ERROR_INVALID_VALUE); }

#define L "libScePsml"
#define L2 "libScePsml_mfsr2"
static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY(L, "scePsmlMfsrInit", mfsr_init),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetSharedResourcesInitRequirement", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrCreateSharedResources", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrReleaseSharedResources", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetContextBufferRequirement", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetContextBufferRequirement800M3", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetContextBufferRequirement800M3_2", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetContextBufferRequirement1100", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "#IMlj247LdTo", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrCreateContext", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrCreateContext800M3_2", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrCreateContext1100", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "#vUk2pWMx3KQ", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "#pwUkfVu0c0c", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrReleaseContext", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetDispatchMfsrPacketSizeInDwords", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "#CN7uea0UQso", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "#wHto4IznfBE", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetDispatchMfsrPacket900", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetDispatchMfsrPacket1000", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetDispatchMfsrPacket1100", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrGetMipmapBias", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrRequestCapture", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrIsCaptureInProgress", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L, "scePsmlMfsrSelectConfig", mfsr_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2Init", mfsr2_init),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2GetSharedResourcesInitRequirement", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2CreateSharedResources", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2ReleaseSharedResources", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2GetContextInitRequirement", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2CreateContext", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2ReleaseContext", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2GetDispatchPacketsSizeInDwords", mfsr2_not_initialized),
    HLE_FUNC_ENTRY(L2, "scePsmlMfsr2GetDispatchPackets", mfsr2_not_initialized),
    HLE_FUNC_ENTRY("libScePsml_nosubmission", "#6T4lBsFKo7U", ok),
    HLE_FUNC_ENTRY("libScePsml_nosubmission", "#tDFGCPGK0Fo", ok),
    HLE_FUNC_ENTRY("libScePsml_nosubmission", "#fwcwm67Nopg", ok),
    HLE_FUNC_ENTRY("libScePsml_quality8K", "scePsmlMfsrEnableQuality8K", ok),
    HLE_FUNC_ENTRY("libScePsml_lightweight", "#TupVXn-8vU4", ok),
    HLE_FUNC_ENTRY("libScePsml_specularAA", "scePsmlMfsrEnableSpecularAA", ok),
    HLE_FUNC_ENTRY("libScePsml_set_mfsrversion", "#3E85qTivV78", set_mfsr_version),
};

void hle_register_psml(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
