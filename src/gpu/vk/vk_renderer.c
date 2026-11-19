/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Vulkan renderer: device, swapchain, guest image cache and presentation.
 *
 * Guest render targets and VideoOut buffers live in guest memory; the
 * renderer keeps a host image per guest surface (keyed by address and size).
 * A surface the GPU never rendered is uploaded from guest memory at each
 * present, so CPU-drawn frames still show. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "base/log.h"
#include "core/settings.h"
#include "gpu/renderer.h"
#include "gpu/shader/shader.h"
#include "gpu/vk/vk_api.h"
#include "gpu/vk/vk_internal.h"
#include "gpu/vk/vk_pipeline_async.h"
#include "gpu/vk/vk_shader_cache.h"
#include "gpu/vk/vk_resource_limits.h"
#include "gpu/vk/vk_image_formats.h"
#include "gpu/vk/vk_overlay.h"
#include "pal/pal.h"
#include "ps5c/version.h"
#include "ps5c/build_info.h"

#define FRAMES_IN_FLIGHT 2

typedef struct vk_checkpoint_record {
    uint64_t a, b, ha, hb;
    uint32_t kind;
} vk_checkpoint_record;

typedef struct vk_checkpoint_state {
    PFN_vkCmdSetCheckpointNV set;
    PFN_vkGetQueueCheckpointDataNV get;
    vk_checkpoint_record records[4096]; /* never recycled while GPU may retain a marker */
    uint16_t index[8192];
    uint32_t count;
    bool full;
} vk_checkpoint_state;

void vk_diagnostic_checkpoint(renderer *r, uint32_t kind, uint64_t a, uint64_t b, uint64_t ha, uint64_t hb) {
    vk_checkpoint_state *s = r->checkpoints;
    if (!s) return;
    uint64_t hash = a ^ (b << 1) ^ ha ^ (hb >> 1) ^ kind;
    uint32_t slot = (uint32_t)(hash ^ (hash >> 32)) & 8191;
    while (s->index[slot]) {
        vk_checkpoint_record *v = &s->records[s->index[slot] - 1];
        if (v->a == a && v->b == b && v->ha == ha && v->hb == hb && v->kind == kind) {
            s->set(r->cmd, v);
            return;
        }
        slot = (slot + 1) & 8191;
    }
    if (s->count == PS5C_ARRAY_COUNT(s->records)) {
        if (!s->full) LOG_W(GPU, "GPU checkpoint capacity exhausted; later unique shaders are unmarked");
        s->full = true;
        return;
    }
    vk_checkpoint_record *v = &s->records[s->count];
    *v = (vk_checkpoint_record){a, b, ha, hb, kind};
    s->index[slot] = (uint16_t)(++s->count);
    s->set(r->cmd, v);
}

static void checkpoint_device_loss(renderer *r, VkResult result) {
    vk_checkpoint_state *s = r->checkpoints;
    if (!s || result != VK_ERROR_DEVICE_LOST) return;
    VkCheckpointDataNV data[16] = {{0}};
    uint32_t count = PS5C_ARRAY_COUNT(data);
    for (uint32_t i = 0; i < count; i++) data[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
    s->get(r->queue, &count, data); /* only legal after the device was lost */
    LOG_E(GPU, "GPU device lost: %u checkpoint stages, %u immutable shader markers", count, s->count);
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t marker = (uintptr_t)data[i].pCheckpointMarker, start = (uintptr_t)s->records;
        if (marker < start || marker - start >= sizeof(s->records) ||
            (marker - start) % sizeof(s->records[0])) {
            LOG_E(GPU, "GPU checkpoint stage %08x: unrecognized marker", data[i].stage);
            continue;
        }
        const vk_checkpoint_record *v = &s->records[(marker - start) / sizeof(s->records[0])];
        LOG_E(GPU, "GPU checkpoint stage %08x kind %u: shader 0x%llx/%016llx, 0x%llx/%016llx",
              data[i].stage, v->kind, (unsigned long long)v->a, (unsigned long long)v->ha,
              (unsigned long long)v->b, (unsigned long long)v->hb);
    }
}

VkResult vk_check_device(renderer *r, VkResult result, const char *what) {
    if (result == VK_ERROR_DEVICE_LOST) vk_check_work(r, result, what);
    return result;
}

static bool check(renderer *r, VkResult res, const char *what) {
    vk_check_device(r, res, what);
    if (res == VK_SUCCESS) return true;
    LOG_E(GPU, "Vulkan: %s failed (%d)", what, (int)res);
    return false;
}

void vk_check_work(renderer *r, VkResult result, const char *what) {
    if (result == VK_SUCCESS) return;
    LOG_E(GPU, "Vulkan: %s failed (%d)", what, (int)result);
    checkpoint_device_loss(r, result);
    /* No failed fence/submit is a completion. The emulator hosts guest threads
     * in its own process: stop it before publishing stale bytes or labels.
     * Do not call Vulkan again to save caches or tear down a lost device. */
    pal_exit_process(1);
    abort(); /* pal_exit_process does not return on supported hosts */
}

/* ---- Instance and device ---- */

static bool has_extension(const VkExtensionProperties *exts, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(exts[i].extensionName, name)) return true;
    }
    return false;
}

/* Version the instance asks for: 1.4 when the loader knows it (the device's own version caps
 * what is really used), else 1.1 as before. PS5C_VK13=0 keeps 1.1. */
static uint32_t wanted_api(void) {
    const char *e = getenv("PS5C_VK13");
    if (e && !strcmp(e, "0")) return VK_API_VERSION_1_1;
    uint32_t loader = vk_api_instance_version();
    if (loader < VK_API_VERSION_1_1) return VK_API_VERSION_1_1;
    return loader < VK_API_VERSION_1_4 ? VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader), 0)
                                       : VK_API_VERSION_1_4;
}

static bool create_instance(renderer *r) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *exts = (VkExtensionProperties *)calloc(n ? n : 1, sizeof(*exts));
    if (!exts) return false;
    vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
    const char *wanted[3];
    uint32_t count = 0;
    wanted[count++] = "VK_KHR_surface";
    wanted[count++] = vk_surface_extension_name();
    bool ok = has_extension(exts, n, wanted[0]) && has_extension(exts, n, wanted[1]);
    free(exts);
    if (!ok) {
        LOG_W(GPU, "Vulkan: no window surface support");
        return false;
    }

    const char *layers[1];
    uint32_t layer_count = 0;
    if (getenv("PS5C_VK_VALIDATION")) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, NULL);
        VkLayerProperties *lp = (VkLayerProperties *)calloc(ln ? ln : 1, sizeof(*lp));
        if (lp) {
            vkEnumerateInstanceLayerProperties(&ln, lp);
            for (uint32_t i = 0; i < ln; i++) {
                if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
            }
            free(lp);
        }
        if (!layer_count) LOG_W(GPU, "Vulkan validation layer not installed");
    }

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PS5C";
    app.pEngineName = "PS5C";
    r->instance_api = wanted_api();
    app.apiVersion = r->instance_api;
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = count;
    ci.ppEnabledExtensionNames = wanted;
    ci.enabledLayerCount = layer_count;
    ci.ppEnabledLayerNames = layers;
    if (!check(r, vkCreateInstance(&ci, NULL, &r->instance), "vkCreateInstance")) return false;
    return vk_api_load_instance(r->instance);
}

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

/* The GPU the user chose (PS5C_GPU, else [video] placa: a name or a number),
 * else a dedicated one, else an integrated one. */
static bool pick_device(renderer *r) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(r->instance, &n, NULL);
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    if (!n || vkEnumeratePhysicalDevices(r->instance, &n, devs) < 0) return false;
    const char *want = getenv("PS5C_GPU");
    if (!want || !*want) want = settings_get("video", "placa");
    if (want && (!*want || same_name(want, "auto"))) want = NULL;
    char *end = NULL;
    long want_index = want ? strtol(want, &end, 10) : -1;
    if (want && *end) want_index = -1; /* a name */
    int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        LOG_I(GPU, "Vulkan device %u: %s", i, p.deviceName);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties qf[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, r->surface, &present);
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) || !present) {
                continue;
            }
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                        p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (want && (want_index >= 0 ? (long)i == want_index : same_name(p.deviceName, want))) score = 100;
            if (score > best_score) {
                best_score = score;
                r->phys = devs[i];
                r->queue_family = q;
                r->props = p;
            }
            break;
        }
    }
    if (best_score < 0) return false;
    if (want && best_score < 100) LOG_W(GPU, "GPU '%s' (settings [video] placa) not found: using the automatic choice", want);
    vkGetPhysicalDeviceMemoryProperties(r->phys, &r->mem_props);
    {
        uint32_t d = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(r->props.apiVersion), VK_API_VERSION_MINOR(r->props.apiVersion), 0);
        r->api = d < r->instance_api ? d : r->instance_api;
    }
    LOG_I(GPU, "Vulkan device: %s (API %u.%u.%u)", r->props.deviceName, VK_VERSION_MAJOR(r->props.apiVersion),
          VK_VERSION_MINOR(r->props.apiVersion), VK_VERSION_PATCH(r->props.apiVersion));
    return true;
}

uint32_t renderer_list_gpus(renderer_gpu *out, uint32_t max) {
    if (!vk_api_load()) return 0;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PS5C";
    app.apiVersion = wanted_api();
    /* the surface extensions too: the instance functions loaded below include theirs */
    const char *exts[2] = {"VK_KHR_surface", vk_surface_extension_name()};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VkInstance inst;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    if (vk_api_load_instance(inst)) {
        VkPhysicalDevice devs[16];
        n = 16;
        if (vkEnumeratePhysicalDevices(inst, &n, devs) < 0) n = 0;
        for (uint32_t i = 0; i < n && i < max; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            snprintf(out[i].name, sizeof(out[i].name), "%s", p.deviceName);
            out[i].discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        }
    }
    vkDestroyInstance(inst, NULL);
    return n;
}

static bool create_device(renderer *r) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = r->queue_family;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    const char *exts[32] = {"VK_KHR_swapchain"};
    uint32_t ext_count = 1;
    /* float controls: shaders keep Inf/NaN semantics (the guest's code relies on them) */
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(r->phys, NULL, &en, NULL);
    VkExtensionProperties *ep = en ? (VkExtensionProperties *)malloc(sizeof(*ep) * en) : NULL;
    bool float_controls = false, format_flags2 = false, demote_extension = false, sgsc_extension = false;
    bool atomic64_extension = false, image_atomic64_extension=false, interlock_extension = false, host_import_extension = false;
    bool address_extension = false, mesh_extension = false, bary_extension=false;
    bool min_lod_extension = false, indexing_extension = false;
    const char *bvh_va = getenv("PS5C_BVH_VA");
    bool address_wanted = bvh_va && !strcmp(bvh_va, "1");
    bool checkpoint_extension = false;
    const char *checkpoint_setting = getenv("PS5C_GPU_CHECKPOINTS");
    bool checkpoint_wanted = checkpoint_setting && !strcmp(checkpoint_setting, "1");
    bool lds_layout_extension = false, spirv14_extension = false, float_controls_extension = false;
    bool minmax_extension = false;
    if (ep && vkEnumerateDeviceExtensionProperties(r->phys, NULL, &en, ep) >= 0) {
        r->memory_budget = has_extension(ep, en, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        for (uint32_t i = 0; i < en; i++) mesh_extension |= !strcmp(ep[i].extensionName, "VK_EXT_mesh_shader");
        for(uint32_t i=0;i<en;i++)bary_extension |= !strcmp(ep[i].extensionName,"VK_KHR_fragment_shader_barycentric");
        for(uint32_t i=0;i<en;i++)min_lod_extension |= !strcmp(ep[i].extensionName,VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
        /* storage images read without a format: the Iris Xe allows them per format through
         * VK_KHR_format_feature_flags2, not with the shaderStorageImageReadWithoutFormat feature */
        for (uint32_t i = 0; i < en; i++) format_flags2 |= !strcmp(ep[i].extensionName, "VK_KHR_format_feature_flags2");
        for (uint32_t i = 0; i < en; i++) float_controls |= !strcmp(ep[i].extensionName, "VK_KHR_shader_float_controls");
        for (uint32_t i = 0; i < en; i++) r->format_list |= !strcmp(ep[i].extensionName, "VK_KHR_image_format_list");
        for (uint32_t i = 0; i < en; i++) r->vs_layer |= !strcmp(ep[i].extensionName, "VK_EXT_shader_viewport_index_layer");
        for (uint32_t i = 0; i < en; i++) demote_extension |= !strcmp(ep[i].extensionName, "VK_EXT_shader_demote_to_helper_invocation");
        for (uint32_t i = 0; i < en; i++) sgsc_extension |= !strcmp(ep[i].extensionName, "VK_EXT_subgroup_size_control");
        for (uint32_t i = 0; i < en; i++) atomic64_extension |= !strcmp(ep[i].extensionName, "VK_KHR_shader_atomic_int64");
        for (uint32_t i = 0; i < en; i++) image_atomic64_extension |= !strcmp(ep[i].extensionName, "VK_EXT_shader_image_atomic_int64");
        for (uint32_t i = 0; i < en; i++) lds_layout_extension |= !strcmp(ep[i].extensionName, "VK_KHR_workgroup_memory_explicit_layout");
        for (uint32_t i = 0; i < en; i++) spirv14_extension |= !strcmp(ep[i].extensionName, "VK_KHR_spirv_1_4");
        for (uint32_t i = 0; i < en; i++) interlock_extension |= !strcmp(ep[i].extensionName, "VK_EXT_fragment_shader_interlock");
        for (uint32_t i = 0; i < en; i++) host_import_extension |= !strcmp(ep[i].extensionName, "VK_EXT_external_memory_host");
        for (uint32_t i = 0; i < en; i++) indexing_extension |= !strcmp(ep[i].extensionName, "VK_EXT_descriptor_indexing");
        for (uint32_t i = 0; i < en; i++) address_extension |= !strcmp(ep[i].extensionName, "VK_KHR_buffer_device_address");
        for (uint32_t i = 0; i < en; i++) minmax_extension |= !strcmp(ep[i].extensionName, "VK_EXT_sampler_filter_minmax");
        for (uint32_t i = 0; i < en; i++) checkpoint_extension |= !strcmp(ep[i].extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        float_controls_extension = float_controls;
        /* newer features: detection only (step 1 of docs/plano-vulkan.md) */
        r->caps.extended_dynamic_state = has_extension(ep, en, "VK_EXT_extended_dynamic_state");
        r->caps.extended_dynamic_state2 = has_extension(ep, en, "VK_EXT_extended_dynamic_state2");
        r->caps.extended_dynamic_state3 = has_extension(ep, en, "VK_EXT_extended_dynamic_state3");
        r->caps.push_descriptor = has_extension(ep, en, "VK_KHR_push_descriptor");
        {
            const char *e = getenv("PS5C_VK_PUSH");
            if (r->caps.push_descriptor && !(e && !strcmp(e, "0"))) {
                VkPhysicalDevicePushDescriptorPropertiesKHR pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
                VkPhysicalDeviceProperties2 pq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp};
                PFN_vkGetPhysicalDeviceProperties2 gp =
                    (PFN_vkGetPhysicalDeviceProperties2)vk_api_instance_proc(r->instance, "vkGetPhysicalDeviceProperties2");
                if (gp) gp(r->phys, &pq);
                r->push_max = pp.maxPushDescriptors;
                if (r->push_max) exts[ext_count++] = "VK_KHR_push_descriptor";
            }
        }
        r->caps.pipeline_library = has_extension(ep, en, "VK_EXT_graphics_pipeline_library");
        r->caps.dynamic_rendering = has_extension(ep, en, "VK_KHR_dynamic_rendering");
        r->caps.synchronization2 = has_extension(ep, en, "VK_KHR_synchronization2");
        r->caps.maintenance4 = has_extension(ep, en, "VK_KHR_maintenance4");
        r->caps.maintenance5 = has_extension(ep, en, "VK_KHR_maintenance5");
        r->caps.timeline_semaphore = has_extension(ep, en, "VK_KHR_timeline_semaphore");
        r->caps.buffer_device_address = address_extension;
    }
    free(ep);
    {
        PFN_vkGetPhysicalDeviceFeatures2 f2 =
            (PFN_vkGetPhysicalDeviceFeatures2)vk_api_instance_proc(r->instance, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan14Features v14 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        if (f2 && r->api >= VK_API_VERSION_1_2) {
            if (r->api >= VK_API_VERSION_1_3) v12.pNext = &v13;
            if (r->api >= VK_API_VERSION_1_4) v13.pNext = &v14;
            VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
            f2(r->phys, &q);
            r->caps.timeline_semaphore |= v12.timelineSemaphore != 0;
            r->caps.buffer_device_address |= v12.bufferDeviceAddress != 0;
            r->caps.dynamic_rendering |= v13.dynamicRendering != 0;
            r->caps.synchronization2 |= v13.synchronization2 != 0;
            r->caps.maintenance4 |= v13.maintenance4 != 0;
            r->caps.extended_dynamic_state |= r->api >= VK_API_VERSION_1_3; /* core in 1.3 */
            r->caps.extended_dynamic_state2 |= r->api >= VK_API_VERSION_1_3;
            r->caps.push_descriptor |= v14.pushDescriptor != 0;
            r->caps.maintenance5 |= v14.maintenance5 != 0;
        }
        LOG_I(GPU, "Vulkan %u.%u (instance %u.%u): dynamic rendering %d, sync2 %d, dynamic state %d/%d/%d, "
              "push descriptors %d, pipeline library %d, timeline %d, buffer address %d, maintenance4/5 %d/%d",
              VK_API_VERSION_MAJOR(r->api), VK_API_VERSION_MINOR(r->api), VK_API_VERSION_MAJOR(r->instance_api),
              VK_API_VERSION_MINOR(r->instance_api), r->caps.dynamic_rendering, r->caps.synchronization2,
              r->caps.extended_dynamic_state, r->caps.extended_dynamic_state2, r->caps.extended_dynamic_state3,
              r->caps.push_descriptor, r->caps.pipeline_library, r->caps.timeline_semaphore,
              r->caps.buffer_device_address, r->caps.maintenance4, r->caps.maintenance5);
    }
    if (r->memory_budget) {
        r->memory_properties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)
            vk_api_instance_proc(r->instance, "vkGetPhysicalDeviceMemoryProperties2");
        r->memory_budget = r->memory_properties2 != NULL;
        if (r->memory_budget) exts[ext_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    }
    if (checkpoint_wanted && checkpoint_extension) exts[ext_count++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    if (minmax_extension) { /* min/max sampler reductions (depth pyramids, culling) */
        exts[ext_count++] = "VK_EXT_sampler_filter_minmax";
        r->sampler_minmax = true;
    }
    /* Update-after-bind layouts: Unreal 5 passes bind more than the 200 resources a stage Intel's
     * driver reports (Commandos: Origins, ~250); their limits there are far higher. Only the
     * layouts past the ordinary limits use it (vk_pipeline_build). PS5C_NO_UAB=1 turns it off. */
    if (indexing_extension && !getenv("PS5C_NO_UAB")) {
        VkPhysicalDeviceDescriptorIndexingProperties ip = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ip};
        vkGetPhysicalDeviceProperties2(r->phys, &p2);
        VkPhysicalDeviceLimits *l = &r->uab_limits;
        *l = r->props.limits;
        l->maxPerStageDescriptorSamplers = ip.maxPerStageDescriptorUpdateAfterBindSamplers;
        l->maxPerStageDescriptorStorageBuffers = ip.maxPerStageDescriptorUpdateAfterBindStorageBuffers;
        l->maxPerStageDescriptorSampledImages = ip.maxPerStageDescriptorUpdateAfterBindSampledImages;
        l->maxPerStageDescriptorStorageImages = ip.maxPerStageDescriptorUpdateAfterBindStorageImages;
        l->maxPerStageResources = ip.maxPerStageUpdateAfterBindResources;
        l->maxDescriptorSetSamplers = ip.maxDescriptorSetUpdateAfterBindSamplers;
        l->maxDescriptorSetStorageBuffers = ip.maxDescriptorSetUpdateAfterBindStorageBuffers;
        l->maxDescriptorSetSampledImages = ip.maxDescriptorSetUpdateAfterBindSampledImages;
        l->maxDescriptorSetStorageImages = ip.maxDescriptorSetUpdateAfterBindStorageImages;
        r->descriptor_uab = l->maxPerStageResources > r->props.limits.maxPerStageResources;
        if (r->descriptor_uab) {
            exts[ext_count++] = "VK_EXT_descriptor_indexing";
            LOG_I(GPU, "update-after-bind layouts: %u resources a stage (ordinary %u), %u storage buffers",
                  l->maxPerStageResources, r->props.limits.maxPerStageResources,
                  l->maxPerStageDescriptorStorageBuffers);
        }
    }
    const char *dynamic_buffers = getenv("PS5C_DYNAMIC_BUFFERS");
    const char *large_reads = getenv("PS5C_NGG_LARGE_READS");
    const char *compute_large=getenv("PS5C_COMPUTE_LARGE_BUFFERS");
    if (host_import_extension && (address_wanted || (dynamic_buffers && !strcmp(dynamic_buffers, "1")) ||
                                 (large_reads && (large_reads[0]=='@' || !strcmp(large_reads,"1"))) ||
                                 (compute_large && (compute_large[0]=='@' || strtoull(compute_large,NULL,16)!=0)))) {
        PFN_vkGetPhysicalDeviceExternalBufferProperties external_properties =
            (PFN_vkGetPhysicalDeviceExternalBufferProperties)vk_api_instance_proc(r->instance, "vkGetPhysicalDeviceExternalBufferProperties");
        VkPhysicalDeviceExternalBufferInfo query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        if (address_wanted && address_extension) query.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
        VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        if (external_properties) external_properties(r->phys, &query, &external);
        if ((external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
            !(external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)) {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
            vkGetPhysicalDeviceProperties2(r->phys, &props);
            VkDeviceSize align = host.minImportedHostPointerAlignment;
            if (align && !(align & (align - 1))) {
                r->host_import_align = align;
                exts[ext_count++] = "VK_EXT_external_memory_host";
            }
        }
    }
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    PFN_vkGetPhysicalDeviceFeatures2 get_features2 =
        (PFN_vkGetPhysicalDeviceFeatures2)vk_api_instance_proc(r->instance, "vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceBufferDeviceAddressFeaturesKHR address_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR};
    if (address_wanted && address_extension && get_features2 && r->host_import_align) {
        VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &address_features};
        get_features2(r->phys, &query);
        r->bvh_address = address_features.bufferDeviceAddress && query.features.shaderInt64;
        address_features.bufferDeviceAddressCaptureReplay = VK_FALSE;
        address_features.bufferDeviceAddressMultiDevice = VK_FALSE;
        if (r->bvh_address) exts[ext_count++] = "VK_KHR_buffer_device_address";
    }
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    const char *pops=getenv("PS5C_POPS_INTERLOCK");
    if(interlock_extension && get_features2 && pops && atoi(pops)!=0) {
        VkPhysicalDeviceFeatures2 features2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&interlock};
        get_features2(r->phys,&features2);
        r->pixel_interlock=interlock.fragmentShaderPixelInterlock!=0;
    }
    interlock.fragmentShaderSampleInterlock=VK_FALSE;
    interlock.fragmentShaderShadingRateInterlock=VK_FALSE;
    if(r->pixel_interlock) exts[ext_count++]="VK_EXT_fragment_shader_interlock";
    VkPhysicalDeviceShaderAtomicInt64Features atomic64 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    if (atomic64_extension && get_features2) {
        VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &atomic64};
        get_features2(r->phys, &features2);
    }
    if (getenv("PS5C_NO_BUFFER_ATOMIC64")) atomic64.shaderBufferInt64Atomics = VK_FALSE;
    bool buffer_atomic64 = atomic64.shaderBufferInt64Atomics != 0;
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT image64={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    if(image_atomic64_extension && get_features2 && !getenv("PS5C_NO_IMAGE_ATOMIC64")) {
        VkPhysicalDeviceFeatures2 features2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&image64};
        get_features2(r->phys,&features2);
        if(!features2.features.shaderInt64)image64.shaderImageInt64Atomics=VK_FALSE;
    }
    image64.sparseImageInt64Atomics=VK_FALSE;
    if(image64.shaderImageInt64Atomics)exts[ext_count++]="VK_EXT_shader_image_atomic_int64";
    sh_set_image_atomic64(image64.shaderImageInt64Atomics!=0);
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR lds_layout = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    if (lds_layout_extension && spirv14_extension && float_controls_extension && get_features2 &&
        atomic64.shaderSharedInt64Atomics && !getenv("PS5C_NO_LDS_ATOMIC64")) {
        VkPhysicalDeviceFeatures2 features2 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&lds_layout};
        get_features2(r->phys,&features2);
    }
    bool lds_atomic64 = lds_layout.workgroupMemoryExplicitLayout != 0;
    atomic64.shaderSharedInt64Atomics = lds_atomic64;
    lds_layout.workgroupMemoryExplicitLayoutScalarBlockLayout = VK_FALSE;
    lds_layout.workgroupMemoryExplicitLayout8BitAccess = VK_FALSE;
    lds_layout.workgroupMemoryExplicitLayout16BitAccess = VK_FALSE;
    if (buffer_atomic64 || lds_atomic64) exts[ext_count++] = "VK_KHR_shader_atomic_int64";
    if (lds_atomic64) {
        exts[ext_count++] = "VK_KHR_workgroup_memory_explicit_layout";
        exts[ext_count++] = "VK_KHR_spirv_1_4";
    }
    if (demote_extension && get_features2) {
        VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &demote};
        get_features2(r->phys, &features2);
        r->demote = demote.shaderDemoteToHelperInvocation != 0;
    }
    if (r->demote) exts[ext_count++] = "VK_EXT_shader_demote_to_helper_invocation";
    /* The guest's waves are 32 (or 64) lanes and its cross-lane code (DPP rows, permlanex16, swizzles)
     * reaches that far; a driver free to pick 8 or 16 (Intel) splits a wave. Compute pipelines
     * require the guest's width where the device allows (PS5C_NO_SUBGROUP_SIZE=1: driver's choice). */
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sgsc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
    if (sgsc_extension && get_features2 && !getenv("PS5C_NO_SUBGROUP_SIZE")) {
        VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &sgsc};
        get_features2(r->phys, &features2);
        VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};
        VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sp.pNext=&subgroup;
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sp};
        vkGetPhysicalDeviceProperties2(r->phys, &p2);
        VkSubgroupFeatureFlags ballot=VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_BALLOT_BIT;
        if((subgroup.supportedOperations&ballot)==ballot)r->ballot32_stages=subgroup.supportedStages;
        r->mesh_native32=sp.minSubgroupSize==32 && sp.maxSubgroupSize==32;
        if (sgsc.subgroupSizeControl && (sp.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
            sp.minSubgroupSize <= 32 && sp.maxSubgroupSize >= 32)
            r->cs_subgroup = 32;
        LOG_I(GPU, "subgroup sizes %u-%u; compute pipelines: %s", sp.minSubgroupSize, sp.maxSubgroupSize,
              r->cs_subgroup ? "32 lanes" : "the driver's choice");
    }
    if (r->cs_subgroup) {
        exts[ext_count++] = "VK_EXT_subgroup_size_control";
        sgsc.computeFullSubgroups = VK_FALSE;
        sgsc.pNext = NULL;
    }
    { /* guest stencil needs a combined depth/stencil format (PS5C_NO_STENCIL=1: depth only, the old way) */
        VkFormatProperties dsp;
        vkGetPhysicalDeviceFormatProperties(r->phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &dsp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        r->stencil = (dsp.optimalTilingFeatures & need) == need && !getenv("PS5C_NO_STENCIL");
    }
    if (float_controls) {
        VkPhysicalDeviceFloatControlsPropertiesKHR fc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &fc};
        vkGetPhysicalDeviceProperties2(r->phys, &p2);
        float_controls = fc.shaderSignedZeroInfNanPreserveFloat32;
    }
    if (float_controls || lds_atomic64) exts[ext_count++] = "VK_KHR_shader_float_controls";
    if (r->format_list) exts[ext_count++] = "VK_KHR_image_format_list";
    if (r->vs_layer) exts[ext_count++] = "VK_EXT_shader_viewport_index_layer";
    if (format_flags2) exts[ext_count++] = "VK_KHR_format_feature_flags2";
    sh_set_vs_layer(r->vs_layer);
    sh_set_preserve_inf_nan(float_controls);
    sh_set_buffer_atomic64(buffer_atomic64);
    sh_set_lds_atomic64(lds_atomic64);
    sh_set_pixel_interlock(r->pixel_interlock);
    vk_shader_cache_device(float_controls, buffer_atomic64, lds_atomic64, r->pixel_interlock);
    LOG_I(GPU,"ordered pixel interlock: %s",r->pixel_interlock?"native (experimental POPS)":"disabled or unavailable");
    LOG_I(GPU,"64-bit LDS atomics: %s",lds_atomic64?"native, aliased workgroup storage":"unavailable");
    LOG_I(GPU, "coherent buffer pairs: %s", buffer_atomic64 ? "64-bit device atomics" : "legacy dword accesses");
    LOG_I(GPU,"64-bit image atomics: %s",image64.shaderImageInt64Atomics?"native R64_UINT views":"unavailable or disabled");
    LOG_I(GPU, "Inf/NaN preserving shaders: %s", float_controls ? "yes" : "no");
    VkPhysicalDeviceFeatures supported, enabled;
    vkGetPhysicalDeviceFeatures(r->phys, &supported);
    memset(&enabled, 0, sizeof(enabled));
    /* Diagnostic for guest accesses exceeding a staged descriptor range.
     * This bounds Vulkan buffer accesses; it does not repair the guest range
     * analysis or guarantee zero for every out-of-bounds read (robustness1). */
    const char *robust_buffers = getenv("PS5C_ROBUST_BUFFERS");
    enabled.robustBufferAccess = supported.robustBufferAccess && robust_buffers && !strcmp(robust_buffers, "1");
    if (robust_buffers)
        LOG_I(GPU, "robust buffer bounds: %s (diagnostic)", enabled.robustBufferAccess ? "enabled" : "disabled or unsupported");
    enabled.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    enabled.shaderInt64 = supported.shaderInt64;
    enabled.shaderInt16 = supported.shaderInt16;
    enabled.shaderFloat64 = supported.shaderFloat64;
    enabled.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
    enabled.independentBlend = supported.independentBlend;
    const char *multi_draw = getenv("PS5C_NGG_MULTI_DRAW");
    enabled.multiDrawIndirect = supported.multiDrawIndirect && multi_draw &&
        (multi_draw[0]=='@' || !strcmp(multi_draw,"1"));
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    /* PS5C_NO_ANISO=1: samplers ignore the guest's anisotropy ratio (A/B, slow GPUs) */
    r->max_aniso = enabled.samplerAnisotropy && !getenv("PS5C_NO_ANISO") ? r->props.limits.maxSamplerAnisotropy : 0.0f;
    enabled.depthClamp = supported.depthClamp;
    enabled.fillModeNonSolid = supported.fillModeNonSolid;
    enabled.geometryShader = supported.geometryShader;
    sh_set_ps_layer(supported.geometryShader);
    enabled.pipelineStatisticsQuery = supported.pipelineStatisticsQuery;
    enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    enabled.shaderImageGatherExtended = supported.shaderImageGatherExtended;
    enabled.shaderClipDistance = supported.shaderClipDistance;
    r->clip_distance = supported.shaderClipDistance != 0;
    enabled.logicOp = supported.logicOp;
    r->logic_op = supported.logicOp != 0;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    const char *mesh_setting = getenv("PS5C_NGG_MESH");
    bool mesh_wanted = mesh_setting ? !strcmp(mesh_setting,"1") : r->ngg_culling_profile;
    if (mesh_wanted && mesh_extension && spirv14_extension && float_controls_extension && get_features2) {
        VkPhysicalDeviceFeatures2 query = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&mesh};
        get_features2(r->phys,&query);
        r->mesh_shader = mesh.meshShader != VK_FALSE;
        mesh = (VkPhysicalDeviceMeshShaderFeaturesEXT){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
                                                     .meshShader=r->mesh_shader};
        if (r->mesh_shader) {
            exts[ext_count++]="VK_EXT_mesh_shader";
            bool have_spirv=false,have_controls=false;
            for(uint32_t i=0;i<ext_count;i++) {
                have_spirv |= !strcmp(exts[i],"VK_KHR_spirv_1_4");
                have_controls |= !strcmp(exts[i],"VK_KHR_shader_float_controls");
            }
            if(!have_spirv)exts[ext_count++]="VK_KHR_spirv_1_4";
            if(!have_controls)exts[ext_count++]="VK_KHR_shader_float_controls";
            r->mesh_limits=(VkPhysicalDeviceMeshShaderPropertiesEXT){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&r->mesh_limits};
            vkGetPhysicalDeviceProperties2(r->phys,&props);
        }
    }
    struct bary_features {VkStructureType sType;void*pNext;VkBool32 fragmentShaderBarycentric;} bary={1000203000,NULL,0};
    const char *vertex_setting = getenv("PS5C_PS_VERTEX");
    bool vertex_enabled = vertex_setting && strcmp(vertex_setting,"0") != 0;
    r->ps_vertex=false;
    /* Keep this path opt-in while the remaining parameter selectors are
     * experimental. An explicit zero must select the fallback as well. */
    if(bary_extension && get_features2 && vertex_enabled) {
        VkPhysicalDeviceFeatures2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&bary};
        get_features2(r->phys,&query);
        if(bary.fragmentShaderBarycentric){exts[ext_count++]="VK_KHR_fragment_shader_barycentric";r->ps_vertex=true;}
        LOG_I(GPU,"per-vertex fragment inputs: %s",bary.fragmentShaderBarycentric?"enabled (experimental)":"unavailable");
    } else {
        LOG_I(GPU,"per-vertex fragment inputs: %s",vertex_enabled?"unavailable":
              vertex_setting?"disabled (PS5C_PS_VERTEX=0)":"disabled (opt-in PS5C_PS_VERTEX=1)");
    }
    VkPhysicalDeviceImageViewMinLodFeaturesEXT min_lod = {
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    const char *resident_setting=getenv("PS5C_RESIDENT_MIPS");
    if(min_lod_extension && get_features2 && resident_setting && !strcmp(resident_setting,"1")) {
        VkPhysicalDeviceFeatures2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&min_lod};
        get_features2(r->phys,&query);
        r->resident_mips=min_lod.minLod!=VK_FALSE;
        if(r->resident_mips)exts[ext_count++]=VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME;
    }
    if(resident_setting)LOG_I(GPU,"resident mip chains: %s (experimental)",r->resident_mips?"enabled":"unavailable or disabled");
    const char *sparse_setting=getenv("PS5C_SPARSE_MIPS");
    if(sparse_setting && !strcmp(sparse_setting,"1") && r->resident_mips &&
       supported.sparseBinding && supported.sparseResidencyImage2D) {
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(r->phys,&count,NULL);
        if(count<=64 && r->queue_family<count) {
            VkQueueFamilyProperties props[64];vkGetPhysicalDeviceQueueFamilyProperties(r->phys,&count,props);
            r->sparse_mips=(props[r->queue_family].queueFlags&VK_QUEUE_SPARSE_BINDING_BIT)!=0;
        }
        enabled.sparseBinding=enabled.sparseResidencyImage2D=r->sparse_mips?VK_TRUE:VK_FALSE;
    }
    if(sparse_setting)LOG_I(GPU,"sparse mip allocations: %s (experimental)",r->sparse_mips?"enabled":"unavailable or disabled");
    VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    if(r->resident_mips)ci.pNext=&min_lod;
    if(r->mesh_shader){mesh.pNext=(void*)ci.pNext;ci.pNext=&mesh;}
    if(bary.fragmentShaderBarycentric){bary.pNext=(void*)ci.pNext;ci.pNext=&bary;}
    if (buffer_atomic64 || lds_atomic64) { atomic64.pNext=(void *)ci.pNext;ci.pNext=&atomic64; }
    if(image64.shaderImageInt64Atomics){image64.pNext=(void *)ci.pNext;ci.pNext=&image64;}
    if (lds_atomic64) { lds_layout.pNext=(void *)ci.pNext;ci.pNext=&lds_layout; }
    if (r->pixel_interlock) { interlock.pNext=(void *)ci.pNext;ci.pNext=&interlock; }
    if (r->demote) { demote.pNext = (void *)ci.pNext; ci.pNext = &demote; }
    if (r->cs_subgroup) {
        sgsc.pNext = (void *)ci.pNext;
        ci.pNext = &sgsc;
    }
    if (r->bvh_address) { address_features.pNext = (void *)ci.pNext; ci.pNext = &address_features; }
    VkPhysicalDeviceDynamicRenderingFeatures dyn_render = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
                                                           .dynamicRendering = VK_TRUE};
    bool want_dyn_render = false;
    {
        const char *e = getenv("PS5C_VK_DYNRENDER");
        want_dyn_render = r->api >= VK_API_VERSION_1_3 && r->caps.dynamic_rendering && !(e && !strcmp(e, "0"));
        if (want_dyn_render) { dyn_render.pNext = (void *)ci.pNext; ci.pNext = &dyn_render; }
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                          .timelineSemaphore = VK_TRUE};
    bool want_timeline = false;
    {
        const char *e = getenv("PS5C_VK_TIMELINE");
        want_timeline = r->api >= VK_API_VERSION_1_2 && r->caps.timeline_semaphore && !(e && !strcmp(e, "0"));
        if (want_timeline) { timeline.pNext = (void *)ci.pNext; ci.pNext = &timeline; }
    }
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures = &enabled;
    if (!check(r, vkCreateDevice(r->phys, &ci, NULL, &r->device), "vkCreateDevice")) return false;
    if (!vk_api_load_device(r->device)) return false;
    {
        const char *e = getenv("PS5C_VK_DYNSTATE");
        if (r->api >= VK_API_VERSION_1_3 && !(e && !strcmp(e, "0"))) {
#define DYN_LOAD(field, name) r->field = (PFN_##name)vkGetDeviceProcAddr(r->device, #name)
            DYN_LOAD(set_cull_mode, vkCmdSetCullMode);
            DYN_LOAD(set_front_face, vkCmdSetFrontFace);
            DYN_LOAD(set_depth_test, vkCmdSetDepthTestEnable);
            DYN_LOAD(set_depth_write, vkCmdSetDepthWriteEnable);
            DYN_LOAD(set_depth_compare, vkCmdSetDepthCompareOp);
            DYN_LOAD(set_depth_bias_enable, vkCmdSetDepthBiasEnable);
            DYN_LOAD(set_stencil_test, vkCmdSetStencilTestEnable);
            DYN_LOAD(set_stencil_op, vkCmdSetStencilOp);
#undef DYN_LOAD
            r->dyn_state = r->set_cull_mode && r->set_front_face && r->set_depth_test && r->set_depth_write &&
                           r->set_depth_compare && r->set_depth_bias_enable && r->set_stencil_test && r->set_stencil_op;
        }
        LOG_I(GPU, "dynamic depth/stencil/cull state: %s", r->dyn_state ? "on" : "off");
    }
    if (want_dyn_render) {
        r->begin_rendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(r->device, "vkCmdBeginRendering");
        r->end_rendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(r->device, "vkCmdEndRendering");
        r->dyn_render = r->begin_rendering && r->end_rendering;
    }
    LOG_I(GPU, "dynamic rendering: %s", r->dyn_render ? "on" : "off");
    if (want_timeline) {
        r->wait_semaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(r->device, "vkWaitSemaphores");
        VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        if (!r->wait_semaphores || vkCreateSemaphore(r->device, &sem, NULL, &r->work_timeline) != VK_SUCCESS)
            r->work_timeline = VK_NULL_HANDLE;
    }
    LOG_I(GPU, "work timeline semaphore: %s", r->work_timeline ? "on" : "off");
    if (r->push_max) {
        r->push_descriptor_set = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(r->device, "vkCmdPushDescriptorSetKHR");
        if (!r->push_descriptor_set) r->push_max = 0;
    }
    LOG_I(GPU, "push descriptors: %s (up to %u per set)", r->push_max ? "on" : "off", r->push_max);
    if(r->mesh_shader) {
        r->draw_mesh_tasks=(PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(r->device,"vkCmdDrawMeshTasksEXT");
        r->mesh_shader=r->draw_mesh_tasks!=NULL;
    }
    if(mesh_wanted)LOG_I(GPU,"native NGG mesh backend: %s; limits %u vertices, %u primitives, %u invocations, %u shared bytes",
                        r->mesh_shader?"enabled (experimental)":"unavailable",r->mesh_limits.maxMeshOutputVertices,
                        r->mesh_limits.maxMeshOutputPrimitives,r->mesh_limits.maxMeshWorkGroupInvocations,
                        r->mesh_limits.maxMeshSharedMemorySize);
    if (r->host_import_align) {
        r->host_pointer_properties = (PFN_vkGetMemoryHostPointerPropertiesEXT)
            vkGetDeviceProcAddr(r->device, "vkGetMemoryHostPointerPropertiesEXT");
        if (!r->host_pointer_properties) r->host_import_align = 0;
        LOG_I(GPU, "dynamic buffers: host import %s, alignment %llu",
              r->host_import_align ? "enabled (experimental)" : "unavailable",
              (unsigned long long)r->host_import_align);
    }
    if (r->bvh_address) {
        r->buffer_address = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(r->device, "vkGetBufferDeviceAddressKHR");
        r->bvh_address = r->buffer_address && r->host_import_align;
    }
    if (address_wanted) LOG_I(GPU, "BVH guest address mapping: %s (experimental)", r->bvh_address ? "enabled" : "unavailable");
    vkGetDeviceQueue(r->device, r->queue_family, 0, &r->queue);
    if (checkpoint_wanted && checkpoint_extension) {
        vk_checkpoint_state *s = calloc(1, sizeof(*s));
        if (s) {
            s->set = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(r->device, "vkCmdSetCheckpointNV");
            s->get = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(r->device, "vkGetQueueCheckpointDataNV");
            if (s->set && s->get) r->checkpoints = s;
            else free(s);
        }
    }
    if (checkpoint_wanted) LOG_I(GPU, "GPU shader checkpoints: %s", r->checkpoints ? "enabled (diagnostic)" : "unavailable");
    r->features = enabled;
    return true;
}

/* ---- Swapchain ---- */

static void destroy_swapchain(renderer *r) {
    for (uint32_t i = 0; i < PS5C_ARRAY_COUNT(r->sc_rendered); i++) {
        if (r->sc_rendered[i]) vkDestroySemaphore(r->device, r->sc_rendered[i], NULL);
        r->sc_rendered[i] = VK_NULL_HANDLE;
    }
    if (r->swapchain) vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
    r->swapchain = VK_NULL_HANDLE;
    r->sc_count = 0;
}

static bool create_swapchain(renderer *r) {
    VkSurfaceCapabilitiesKHR caps;
    if (!check(r, vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->phys, r->surface, &caps), "surface caps")) return false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        uint32_t w = 0, h = 0;
        pal_window_client_size(r->window, &w, &h);
        ext.width = w;
        ext.height = h;
    }
    if (ext.width == 0 || ext.height == 0) return false; /* minimized */

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, r->surface, &fn, NULL);
    VkSurfaceFormatKHR formats[64];
    if (fn > 64) fn = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->phys, r->surface, &fn, formats);
    VkSurfaceFormatKHR fmt = formats[0];
    for (uint32_t i = 0; i < fn; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = formats[i];
        }
    }
    /* VideoOut paces the frames: present without waiting for the display when possible */
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->phys, r->surface, &pn, NULL);
    VkPresentModeKHR modes[16];
    if (pn > 16) pn = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->phys, r->surface, &pn, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < pn; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (mode == VK_PRESENT_MODE_FIFO_KHR) {
        for (uint32_t i = 0; i < pn; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }

    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = r->surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = r->swapchain;
    VkSwapchainKHR sc;
    if (!check(r, vkCreateSwapchainKHR(r->device, &ci, NULL, &sc), "vkCreateSwapchainKHR")) return false;
    destroy_swapchain(r);
    r->swapchain = sc;
    r->sc_format = fmt.format;
    r->sc_extent = ext;
    r->sc_count = PS5C_ARRAY_COUNT(r->sc_images);
    VkResult images = vkGetSwapchainImagesKHR(r->device, sc, &r->sc_count, r->sc_images);
    if (images != VK_SUCCESS || !r->sc_count || r->sc_count > PS5C_ARRAY_COUNT(r->sc_images)) {
        destroy_swapchain(r);
        return false;
    }
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < r->sc_count; i++) {
        if (!check(r, vkCreateSemaphore(r->device, &sci, NULL, &r->sc_rendered[i]), "vkCreateSemaphore (swapchain)")) {
            destroy_swapchain(r);
            return false;
        }
    }
    LOG_I(GPU, "swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, r->sc_count, (int)mode);
    return true;
}

/* ---- Memory helpers ---- */

bool vk_alloc_memory(renderer *r, const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkDeviceMemory *out) {
    for (uint32_t i = 0; i < r->mem_props.memoryTypeCount; i++) {
        if ((req->memoryTypeBits & (1u << i)) && (r->mem_props.memoryTypes[i].propertyFlags & want) == want) {
            VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            VkMemoryAllocateFlagsInfo address = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR};
            if (r->bvh_address) ai.pNext = &address;
            ai.allocationSize = req->size;
            ai.memoryTypeIndex = i;
            return check(r, vkAllocateMemory(r->device, &ai, NULL, out), "vkAllocateMemory");
        }
    }
    LOG_E(GPU, "Vulkan: no memory type for flags 0x%x", want);
    return false;
}

bool vk_buffer_create(renderer *r, VkDeviceSize size, VkBufferUsageFlags usage, bool host, vk_buffer *out) {
    memset(out, 0, sizeof(*out));
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage | (r->bvh_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR : 0);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(r, vkCreateBuffer(r->device, &ci, NULL, &out->buffer), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->device, out->buffer, &req);
    VkMemoryPropertyFlags flags = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                       : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    /* host buffers the CPU reads back (GPU results, captures): cached memory,
     * uncached reads run at a few dozen MB/s */
    bool read_back = host && (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    bool cached = false;
    for (uint32_t i = 0; read_back && i < r->mem_props.memoryTypeCount && !cached; i++) {
        VkMemoryPropertyFlags want = flags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        cached = (req.memoryTypeBits & (1u << i)) && (r->mem_props.memoryTypes[i].propertyFlags & want) == want;
    }
    if (cached) flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (!vk_alloc_memory(r, &req, flags, &out->memory)) {
        vkDestroyBuffer(r->device, out->buffer, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->size = size;
    /* Out of memory, binding or mapping fails here, not in vkAllocateMemory (Iris Xe) */
    if (!check(r, vkBindBufferMemory(r->device, out->buffer, out->memory, 0), "vkBindBufferMemory") ||
        (host && !check(r, vkMapMemory(r->device, out->memory, 0, size, 0, &out->mapped), "vkMapMemory"))) {
        vkDestroyBuffer(r->device, out->buffer, NULL);
        vkFreeMemory(r->device, out->memory, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

/* Optional work arena: preserve coherent CPU access while placing shader
 * data on the device. Readback/capture buffers keep their cached host policy.
 * Callers retain the ordinary arena if no compatible allocation succeeds. */
bool vk_buffer_create_local_host(renderer *r, VkDeviceSize size, VkBufferUsageFlags usage, vk_buffer *out) {
    memset(out,0,sizeof(*out));
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=usage|(r->bvh_address?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR:0),
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(r->device,&ci,NULL,&out->buffer)!=VK_SUCCESS)return false;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(r->device,out->buffer,&req);
    VkMemoryPropertyFlags want=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(uint32_t i=0;i<r->mem_props.memoryTypeCount;i++) {
        if(!(req.memoryTypeBits&(1u<<i)) || (r->mem_props.memoryTypes[i].propertyFlags&want)!=want)continue;
        uint32_t heap=r->mem_props.memoryTypes[i].heapIndex;
        if(heap>=r->mem_props.memoryHeapCount || req.size>r->mem_props.memoryHeaps[heap].size)continue;
        VkMemoryAllocateFlagsInfo address={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR};
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext=r->bvh_address?&address:NULL,.allocationSize=req.size,.memoryTypeIndex=i};
        if(vkAllocateMemory(r->device,&ai,NULL,&out->memory)!=VK_SUCCESS) {out->memory=VK_NULL_HANDLE;continue;}
        if(vkBindBufferMemory(r->device,out->buffer,out->memory,0)!=VK_SUCCESS ||
           vkMapMemory(r->device,out->memory,0,size,0,&out->mapped)!=VK_SUCCESS)break;
        out->size=size;
        LOG_I(GPU,"local work arena: %.1f MiB, memory type %u flags 0x%x heap %u",
            (double)size/1048576.0,i,r->mem_props.memoryTypes[i].propertyFlags,r->mem_props.memoryTypes[i].heapIndex);
        return true;
    }
    vk_buffer_destroy(r,out);
    return false;
}

bool vk_buffer_import_host(renderer *r, void *host, VkDeviceSize bytes, vk_buffer *out) {
    memset(out, 0, sizeof(*out));
    VkDeviceSize align = r->host_import_align;
    if (!align || !r->host_pointer_properties || !host || !bytes ||
        (uintptr_t)host % align || bytes % align || bytes > r->props.limits.maxStorageBufferRange)
        return false;
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (r->bvh_address) ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
    if (vkCreateBuffer(r->device, &ci, NULL, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->device, out->buffer, &req);
    /* Never round beyond the lifetime-protected allocation supplied by the caller. */
    if (req.size > bytes) {
        static uint32_t reports;
        if(r->bvh_address && reports++<8)LOG_W(GPU,"BVH import requirements: pointer %p size %llu needs %llu",
            host,(unsigned long long)bytes,(unsigned long long)req.size);
        goto fail;
    }
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult import_status=r->host_pointer_properties(r->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (import_status != VK_SUCCESS) {
        static uint32_t reports;
        if(r->bvh_address && reports++<8)LOG_W(GPU,"BVH import host properties: pointer %p result %d",host,import_status);
        goto fail;
    }
    uint32_t type = UINT32_MAX;
    VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < r->mem_props.memoryTypeCount; i++)
        if ((req.memoryTypeBits & hp.memoryTypeBits & (1u << i)) && (r->mem_props.memoryTypes[i].propertyFlags & need) == need) {
            type = i; break;
        }
    if (type == UINT32_MAX) {
        static uint32_t reports;
        if(r->bvh_address && reports++<8)LOG_W(GPU,"BVH import memory type: pointer %p buffer bits %x host bits %x",host,req.memoryTypeBits,hp.memoryTypeBits);
        goto fail;
    }
    VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host};
    VkMemoryAllocateFlagsInfo address = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = &import, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = r->bvh_address ? (void *)&address : (void *)&import,
        .allocationSize = bytes, .memoryTypeIndex = type};
    import_status=vkAllocateMemory(r->device, &ai, NULL, &out->memory);
    if(import_status==VK_SUCCESS)import_status=vkBindBufferMemory(r->device, out->buffer, out->memory, 0);
    if(import_status!=VK_SUCCESS) {
        static uint32_t reports;
        if(r->bvh_address && reports++<8)LOG_W(GPU,"BVH import allocation/bind: pointer %p size %llu result %d type %u flags %x heap %u (buffer bits %x host bits %x)",
            host,(unsigned long long)bytes,import_status,type,r->mem_props.memoryTypes[type].propertyFlags,
            r->mem_props.memoryTypes[type].heapIndex,req.memoryTypeBits,hp.memoryTypeBits);
        goto fail;
    }
    out->size = bytes;
    return true;
fail:
    vk_buffer_destroy(r, out);
    return false;
}

void vk_buffer_destroy(renderer *r, vk_buffer *b) {
    if (b->mapped) vkUnmapMemory(r->device, b->memory);
    if (b->buffer) vkDestroyBuffer(r->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(r->device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

/* use_seq of the batch being recorded. An image a command of it touches is in use: the cache must
 * not evict it before the batch is submitted (a destroyed image invalidates the command buffer,
 * and its submission lost the device: No Man's Sky's fill clears touched images no draw used). */
static uint64_t g_recording_seq;

void vk_barrier(VkCommandBuffer cmd, vk_image *img, VkImageLayout to, VkAccessFlags dst_access,
                VkPipelineStageFlags dst_stage) {
    if (g_recording_seq && img->last_use < g_recording_seq) img->last_use = g_recording_seq;
    if (img->layout == to && to != VK_IMAGE_LAYOUT_GENERAL && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        /* Read-only users may move between compute, vertex and fragment
         * without a transition. Keep every consumer in the source scope of
         * the next barrier, especially before the image is overwritten. */
        img->access |= dst_access;
        img->stage |= dst_stage;
        return;
    }
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = img->access;
    b.dstAccessMask = dst_access;
    b.oldLayout = img->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange.aspectMask = vk_format_has_stencil(img->format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                                        : img->aspect;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(cmd, img->stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    img->layout = to;
    img->access = dst_access;
    img->stage = dst_stage;
}

/* ---- Guest surface cache ---- */

uint32_t vk_format_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB: return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R64_UINT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

uint32_t vk_alias_formats(VkFormat f, VkFormat *out) {
    uint32_t bytes=vk_format_bytes(f),n=vk_image_alias_formats(f,bytes,out);
    if(bytes==8 && f!=VK_FORMAT_R64_UINT && sh_image_atomic64_supported())out[n++]=VK_FORMAT_R64_UINT;
    return n;
}

/* Integer color formats: a blit does not convert them to the swapchain's normalized one. */
static VkFormat present_unorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* Offset in direct memory of a guest address that is a view of it (several virtual
 * addresses can show the same memory), else VK_NO_PHYS. VMAs are cached for a batch. */
uint64_t vk_phys_of(renderer *r, guest_addr addr) {
    static int off = -1; /* PS5C_NO_IMAGE_ALIASES: images by virtual address only (debugging aid) */
    if (off < 0) off = getenv("PS5C_NO_IMAGE_ALIASES") != NULL;
    /* Shared views may start below the default boundary. Their identity
     * follows this memory instance, not the launcher's default layout. */
    if (off || !r->mem || addr < r->mem->ph_start) return VK_NO_PHYS;
    if (r->phys_cache_seq != r->use_seq) r->phys_cached = 0, r->phys_cache_seq = r->use_seq;
    uint32_t capacity = PS5C_ARRAY_COUNT(r->phys_cache);
    uint32_t n = r->phys_cached < capacity ? r->phys_cached : capacity;
    for (uint32_t i = 0; i < n; i++)
        if (addr >= r->phys_cache[i].start && addr < r->phys_cache[i].end)
            return r->phys_cache[i].phys == VK_NO_PHYS ? VK_NO_PHYS : r->phys_cache[i].phys + (addr - r->phys_cache[i].start);
    gmem_vma v;
    if (!gmem_query_vma(r->mem, addr, false, &v)) return VK_NO_PHYS;
    uint64_t phys = v.kind == GVMA_VIEW && !v.reserved ? v.phys + (v.start - v.os_base) : VK_NO_PHYS;
    uint32_t slot = r->phys_cached++ % capacity;
    r->phys_cache[slot].start = v.start;
    r->phys_cache[slot].end = v.start + v.size;
    r->phys_cache[slot].phys = phys;
    return phys == VK_NO_PHYS ? VK_NO_PHYS : phys + (addr - v.start);
}

/* img is at addr, or at another view of the same memory */
static bool at(const vk_image *img, guest_addr addr, uint64_t phys) {
    /* Remapping a VA must not reuse its old physical page's image; another
     * surviving alias may still need that old image and its GPU contents. */
    return phys != VK_NO_PHYS ? img->phys == phys : img->addr == addr;
}

/* Of two candidates, the one at the address itself unless an alias was written later */
static bool better(const vk_image *c, const vk_image *best, guest_addr addr) {
    if (!best) return true;
    if (c->written_seq != best->written_seq) return c->written_seq > best->written_seq;
    return c->addr == addr && best->addr != addr;
}

/* Runtime A/B control for CPU preparation only; no guest bytes or Vulkan
 * state change. Settings files are polled once per rendered guest frame. */
void vk_cpu_prepare(renderer *r, uint64_t flip) {
    if(r->cpu_prepare_seen && r->cpu_prepare_flip==flip)return;
    bool enabled=true;
    const char *s=r->cpu_prepare_setting;
    if(s && s[0]=='@') {
        FILE *f=fopen(s+1,"r");int value=0;
        if(f){enabled=fscanf(f,"%d",&value)==1 && value==1;fclose(f);}else enabled=false;
    } else if(s) enabled=!strcmp(s,"1");
    if(!r->cpu_prepare_seen || enabled!=r->cpu_prepare_enabled)
        LOG_I(GPU,"CPU preparation lookup cache: %s",enabled?"enabled":"disabled");
    r->cpu_prepare_enabled=enabled;r->cpu_prepare_seen=true;r->cpu_prepare_flip=flip;
}

/* Both exact guest addresses and physical aliases participate, as in at().
 * A new/reused slot invalidates candidate lists. Oversized alias sets fall
 * back to the full scan. Writes do not invalidate: better() runs every time. */
static const uint16_t *image_candidates(renderer *r,guest_addr addr,uint64_t phys,uint32_t *count) {
    if(!r->cpu_prepare_enabled)return NULL;
    uint32_t slot=(uint32_t)((addr>>8)^(addr>>21))&255u;
    if(r->image_lookup[slot].generation!=r->image_lookup_generation ||
       r->image_lookup[slot].addr!=addr || r->image_lookup[slot].phys!=phys) {
        r->image_lookup[slot].generation=r->image_lookup_generation;
        r->image_lookup[slot].addr=addr;r->image_lookup[slot].phys=phys;
        r->image_lookup[slot].count=0;
        for(uint32_t i=0;i<r->image_count;i++)if(at(&r->images[i],addr,phys)) {
            uint32_t n=r->image_lookup[slot].count;
            if(n==PS5C_ARRAY_COUNT(r->image_lookup[slot].indices)) {
                r->image_lookup[slot].count=UINT32_MAX;break;
            }
            r->image_lookup[slot].indices[n]=(uint16_t)i;
            r->image_lookup[slot].count++;
        }
    }
    if(r->image_lookup[slot].count==UINT32_MAX)return NULL;
    *count=r->image_lookup[slot].count;
    return r->image_lookup[slot].indices;
}

/* The array image at this address with the most layers */
vk_image *vk_image_find_array(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format) {
    vk_image *best = NULL;
    uint64_t phys = vk_phys_of(r, addr);
    uint32_t count=r->image_count;
    const uint16_t *candidates=image_candidates(r,addr,phys,&count);
    for (uint32_t i = 0; i < count; i++) {
        vk_image *img = &r->images[candidates?candidates[i]:i];
        if (at(img, addr, phys) && img->width == width && img->height == height && img->format == format && !img->depth &&
            img->levels <= 1 && img->layers > 1 && (!best || img->layers > best->layers || (img->layers == best->layers && better(img, best, addr))))
            best = img;
    }
    if (best) best->last_use = r->use_seq;
    return best;
}

vk_image *vk_image_find_layers(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format,
                                uint32_t layers) {
    vk_image *best = NULL;
    uint64_t phys = vk_phys_of(r, addr);
    uint32_t count=r->image_count;
    const uint16_t *candidates=image_candidates(r,addr,phys,&count);
    for (uint32_t i = 0; i < count; i++) {
        vk_image *img = &r->images[candidates?candidates[i]:i];
        if (at(img, addr, phys) && img->width == width && img->height == height && img->format == format && !img->depth &&
            img->levels <= 1 && img->layers == layers && better(img, best, addr))
            best = img;
    }
    if (best) best->last_use = r->use_seq;
    return best;
}

vk_image *vk_image_find(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format) {
    return vk_image_find_layers(r, addr, width, height, format, 1);
}

/* Any format: the one the GPU wrote last (a surface drawn in another format
 * than it was first seen in has several host images). */
vk_image *vk_image_find_any(renderer *r, guest_addr addr, uint32_t width, uint32_t height) {
    vk_image *best = NULL;
    uint64_t phys = vk_phys_of(r, addr);
    uint32_t count=r->image_count;
    const uint16_t *candidates=image_candidates(r,addr,phys,&count);
    for (uint32_t i = 0; i < count; i++) {
        vk_image *img = &r->images[candidates?candidates[i]:i];
        if (!at(img, addr, phys) || img->width != width || img->height != height || img->layers != 1 || img->depth ||
            img->levels > 1)
            continue;
        if (better(img, best, addr)) best = img;
    }
    if (best) best->last_use = r->use_seq;
    return best;
}

vk_image *vk_image_get(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format) {
    return vk_image_get_layers(r, addr, width, height, format, 1);
}

static bool create_objects(renderer *r, vk_image *img, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                           bool mutable_fmt,uint32_t first);

vk_image *vk_image_find_3d(renderer *r, guest_addr addr, uint32_t width, uint32_t height, uint32_t depth, VkFormat format) {
    for (uint32_t i = 0; i < r->image_count; i++) {
        vk_image *img = &r->images[i];
        if (img->addr == addr && img->width == width && img->height == height && img->depth == depth && img->format == format &&
            img->levels <= 1) {
            img->last_use = r->use_seq;
            return img;
        }
    }
    return NULL;
}

/* Whether a device-local heap is near the budget the OS gives this process
 * (VK_EXT_memory_budget). An integrated GPU's budget shrinks as system RAM fills;
 * past it, the Iris Xe driver crashed in vkCmdPipelineBarrier (Commandos: Origins). */
static bool memory_tight(renderer *r) {
    if (!r->memory_budget) return false;
    PFN_vkGetPhysicalDeviceMemoryProperties2 get = r->memory_properties2;
    if (!get) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
    get(r->phys, &p);
    for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount; h++)
        if ((p.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && b.heapBudget[h] &&
            b.heapUsage[h] > b.heapBudget[h] / 10 * 8) return true;
    return false;
}

static void free_feedback(renderer *r, vk_image *source);

/* Destroys an image's objects (the queue is idle) and forgets its views and index. */
static void image_release(renderer *r, vk_image *img) {
    if (!img->image) return; /* an empty slot (freed over the memory budget) */
    free_feedback(r, img); /* queue is idle: its private sampled views are no longer in use */
    vk_pipeline_forget_view(r, img->view);
    if (img->ds_view) vk_pipeline_forget_view(r, img->ds_view);
    if (img->stencil_view) vk_pipeline_forget_view(r, img->stencil_view);
    vk_pipeline_forget_view(r, img->array_view);
    for (uint32_t k = 0; k < img->sub_count; k++) vk_pipeline_forget_view(r, img->sub[k].view);
    vkDestroyImageView(r->device, img->view, NULL);
    vkDestroyImageView(r->device, img->array_view, NULL);
    if (img->ds_view) vkDestroyImageView(r->device, img->ds_view, NULL);
    if (img->stencil_view) vkDestroyImageView(r->device, img->stencil_view, NULL);
    for (uint32_t k = 0; k < img->sub_count; k++) vkDestroyImageView(r->device, img->sub[k].view, NULL);
    free(img->sub);
    vkDestroyImage(r->device, img->image, NULL);
    vkFreeMemory(r->device, img->memory, NULL);
    vk_pipeline_forget_image(r, (uint32_t)(img - r->images));
}

static vk_image *get_image(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format, uint32_t layers,
                           uint32_t depth, uint32_t levels,uint32_t first);

typedef struct vk_image_backing {
    struct vk_image_backing *next;
    guest_addr addr;
    uint32_t width, height, layers, depth;
    VkFormat format;
    uint64_t written_seq, bytes;
    uint8_t data[];
} vk_image_backing;

bool vk_image_backing_newer(renderer *r, guest_addr addr, uint32_t width, uint32_t height,
                            uint32_t layers, VkFormat format, uint64_t written_seq) {
    for (vk_image_backing *b = r->image_backing; b; b = b->next)
        if (b->addr == addr && b->width == width && b->height == height && b->layers == layers &&
            !b->depth && b->format == format && b->written_seq > written_seq) return true;
    return false;
}

/* The victim is unused by the open batch. Separate transfer commands complete
 * before its objects are destroyed or the restored image is used by that batch. */
static bool backing_transfer(renderer *r, vk_image *img, vk_buffer *buf, bool restore) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = r->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(r->device, &ai, &cmd) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check_work(r, vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer (backing)");
    vk_barrier(cmd, img, restore ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               restore ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = img->depth ? 1 : img->layers;
    copy.imageExtent = (VkExtent3D){img->width, img->height, img->depth ? img->depth : 1};
    if (restore) vkCmdCopyBufferToImage(cmd, buf->buffer, img->image, img->layout, 1, &copy);
    else vkCmdCopyImageToBuffer(cmd, img->image, img->layout, buf->buffer, 1, &copy);
    vk_check_work(r, vkEndCommandBuffer(cmd), "vkEndCommandBuffer (backing)");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vk_check_work(r, vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (backing)");
    vk_check_work(r, vkQueueWaitIdle(r->queue), "vkQueueWaitIdle (backing)");
    vkFreeCommandBuffers(r->device, r->pool, 1, &cmd);
    return true;
}

static bool backing_save(renderer *r, vk_image *img) {
    uint64_t bytes = (uint64_t)img->width * img->height * (img->depth ? img->depth : img->layers) * vk_format_bytes(img->format);
    if (!bytes || bytes > SIZE_MAX - sizeof(vk_image_backing) ||
        bytes > r->image_backing_limit || r->image_backing_bytes > r->image_backing_limit - bytes) return false;
    vk_image_backing *b = malloc(sizeof(*b) + (size_t)bytes);
    if (!b) return false;
    vk_buffer buffer = {0};
    bool ok = vk_buffer_create(r, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &buffer);
    if (ok) ok = backing_transfer(r, img, &buffer, false);
    if (ok) {
        memcpy(b->data, buffer.mapped, (size_t)bytes);
        b->addr = img->addr;
        b->width = img->width; b->height = img->height;
        b->layers = img->layers; b->depth = img->depth;
        b->format = img->format; b->written_seq = img->written_seq; b->bytes = bytes;
        b->next = r->image_backing; r->image_backing = b;
        r->image_backing_bytes += bytes;
        r->image_spills++;
    } else free(b);
    vk_buffer_destroy(r, &buffer);
    return ok;
}

/* Backing stores texel bytes, not values converted to the writer's number
 * representation. A later sampler can request an alias of those same bytes. */
static bool backing_format_compatible(VkFormat stored, VkFormat requested) {
    if (stored == requested)
        return true;
    uint32_t bytes = vk_format_bytes(stored);
    if (!bytes || bytes != vk_format_bytes(requested))
        return false;
    VkFormat aliases[VK_IMAGE_ALIAS_CAPACITY];
    uint32_t count = vk_alias_formats(stored, aliases);
    for (uint32_t i = 0; i < count; i++)
        if (aliases[i] == requested)
            return true;
    return false;
}

static bool backing_restore(renderer *r, vk_image *img) {
    vk_image_backing **best = NULL;
    for (vk_image_backing **p = &r->image_backing; *p; p = &(*p)->next) {
        vk_image_backing *b = *p;
        if (b->addr == img->addr && b->width == img->width && b->height == img->height &&
            b->layers == img->layers && b->depth == img->depth &&
            backing_format_compatible(b->format, img->format) &&
            (!best || b->written_seq > (*best)->written_seq))
            best = p;
    }
    if (!best)
        return true;
    vk_image_backing *b = *best;
    vk_buffer buffer = {0};
    bool ok = vk_buffer_create(r, b->bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &buffer);
    if (ok) {
        memcpy(buffer.mapped, b->data, (size_t)b->bytes);
        ok = backing_transfer(r, img, &buffer, true);
    }
    vk_buffer_destroy(r, &buffer);
    if (!ok)
        return false;
    img->gpu_written = true;
    img->written_seq = b->written_seq;
    *best = b->next;
    r->image_backing_bytes -= b->bytes;
    r->image_restores++;
    free(b);
    if (r->image_restores <= 8 || r->image_restores % 128 == 0)
        LOG_I(GPU, "image backing: %llu spills, %llu restores, %llu KiB in RAM",
              (unsigned long long)r->image_spills, (unsigned long long)r->image_restores,
              (unsigned long long)(r->image_backing_bytes / 1024));
    return true;
}

vk_image *vk_image_get_3d(renderer *r, guest_addr addr, uint32_t width, uint32_t height, uint32_t depth, VkFormat format) {
    vk_image *img = vk_image_find_3d(r, addr, width, height, depth, format);
    return img ? img : get_image(r, addr, width, height, format, 1, depth, 1, 0);
}

vk_image *vk_image_get_layers(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format,
                              uint32_t layers) {
    vk_image *img = vk_image_find_layers(r, addr, width, height, format, layers);
    if (img) return img;
    return get_image(r, addr, width, height, format, layers, 0, 1, 0);
}

static vk_image *get_levels(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format,
                              uint32_t layers, uint32_t levels,uint32_t first) {
    if (levels <= 1) return vk_image_get_layers(r, addr, width, height, format, layers);
    /* Same candidate list as per-level lookups; keep the exact-address
     * condition below, so physical aliases do not change chain identity. */
    uint32_t count=r->image_count;
    const uint16_t *candidates=image_candidates(r,addr,vk_phys_of(r,addr),&count);
    for (uint32_t i = 0; i < count; i++) {
        vk_image *img = &r->images[candidates?candidates[i]:i];
        if (img->addr == addr && img->width == width && img->height == height && img->format == format && !img->depth &&
            img->layers == layers && img->levels == levels && img->sparse_first<=first) {
            img->last_use = r->use_seq;
            return img;
        }
    }
    return get_image(r, addr, width, height, format, layers, 0, levels, first);
}

vk_image *vk_image_get_levels(renderer *r,guest_addr addr,uint32_t width,uint32_t height,VkFormat format,
                              uint32_t layers,uint32_t levels) {
    return get_levels(r,addr,width,height,format,layers,levels,0);
}
vk_image *vk_image_get_resident_levels(renderer *r,guest_addr addr,uint32_t width,uint32_t height,VkFormat format,
                                      uint32_t levels,uint32_t first) {
    return get_levels(r,addr,width,height,format,1,levels,first);
}

static vk_image *get_image(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format, uint32_t layers,
                           uint32_t depth, uint32_t levels,uint32_t first) {
    if (!vk_image_extent_valid(&r->props.limits, width, height, layers, depth)) {
        static uint32_t logged;
        if (logged++ < 8)
            LOG_W(GPU, "unsupported image extent %ux%ux%u, %u layers at 0x%llx: no allocation",
                  width, height, depth ? depth : 1, layers, (unsigned long long)addr);
        return NULL;
    }
    vk_image *img;
    bool reuse = false;
    uint64_t image_bytes = 0;
    if (r->image_bytes_limit)
        for (uint32_t i = 0; i < r->image_count; i++) image_bytes += r->images[i].allocated_bytes;
    /* Over the memory budget (integrated GPUs share system RAM), new images replace
     * old ones too: Commandos: Origins' 4K Unreal surfaces reached the 7.8 GiB the
     * Iris Xe reports, and its driver then crashed in vkCmdPipelineBarrier. */
    bool over_budget = (r->image_bytes_limit && image_bytes >= r->image_bytes_limit) || memory_tight(r);
    vk_image *lru = NULL;
    if (r->image_count == r->image_limit || over_budget) {
        /* full: replace the least recently used image this batch does not use */
        for (uint32_t i = 0; i < r->image_count; i++) {
            vk_image *c = &r->images[i];
            if (c->last_use >= r->use_seq) continue;
            if (r->image_backing_limit && c->gpu_written &&
                (c->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !vk_format_bytes(c->format) || c->levels > 1)) continue;
            if (!lru || (r->image_backing_limit && !c->gpu_written && lru->gpu_written) ||
                ((!r->image_backing_limit || c->gpu_written == lru->gpu_written) && c->last_use < lru->last_use)) lru = c;
        }
        if (!lru && r->image_count == r->image_limit) {
            static uint32_t logged;
            if (logged++ < 8) LOG_I(GPU, "image cache pressure: finish batch and retry resource binding");
            r->image_cache_pressure = true;
            return NULL;
        }
    }
    if (lru) { /* without one, an image over the budget takes a free slot (the batch uses every image) */
        static uint32_t budget_logs;
        if (over_budget && budget_logs++ < 4)
            LOG_I(GPU, "image cache: %u images, over the memory budget: replacing older ones", r->image_count);
        vk_check_work(r, vkQueueWaitIdle(r->queue), "vkQueueWaitIdle (transfer)"); /* earlier batches and presents may still read it */
        if (r->image_backing_limit && lru->gpu_written && !backing_save(r, lru)) {
            static uint32_t logged;
            if (logged++ < 8) LOG_W(GPU, "image backing unavailable or at RAM limit; keeping GPU contents");
            r->image_cache_pressure = true;
            return NULL;
        }
        image_release(r, lru);
        /* Over the budget, older images go until the new one fits: one for one, small
         * ones were replaced by 4K ones and the total kept growing. The slots stay empty. */
        uint64_t want = (uint64_t)width * height * layers * (depth ? depth : 1) * (vk_format_bytes(format) ? vk_format_bytes(format) : 1);
        image_bytes -= image_bytes >= lru->allocated_bytes ? lru->allocated_bytes : image_bytes;
        for (uint32_t freed = 0; over_budget && freed < 64 &&
             ((r->image_bytes_limit && image_bytes + want > r->image_bytes_limit) || memory_tight(r)); freed++) {
            vk_image *old = NULL;
            for (uint32_t i = 0; i < r->image_count; i++) {
                vk_image *c = &r->images[i];
                if (c == lru || !c->image || c->last_use >= r->use_seq || (r->image_backing_limit && c->gpu_written)) continue;
                if (!old || c->last_use < old->last_use) old = c;
            }
            if (!old) break;
            image_bytes -= image_bytes >= old->allocated_bytes ? old->allocated_bytes : image_bytes;
            image_release(r, old);
            memset(old, 0, sizeof(*old));
            old->phys = VK_NO_PHYS;
        }
        img = lru;
        reuse = true;
    } else {
        img = &r->images[r->image_count];
    }
    r->image_lookup_generation++;
    memset(img, 0, sizeof(*img));
    img->depth = depth;
    img->addr = addr;
    img->phys = vk_phys_of(r, addr);
    img->levels = levels ? levels : 1;
    if (!create_objects(r, img, format, width, height, layers, false, first)) return NULL;
    img->layers = layers;
    img->width = width;
    img->height = height;
    img->format = format;
    img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    img->last_use = r->use_seq;
    if (!reuse) r->image_count++;
    if (r->image_backing_limit && img->levels == 1 && !backing_restore(r, img)) {
        /* Retain the RAM copy for a retry; this empty slot cannot masquerade
         * as a valid texture and be filled from stale guest memory. */
        vkDestroyImageView(r->device, img->view, NULL);
        vkDestroyImageView(r->device, img->array_view, NULL);
        if (img->ds_view) vkDestroyImageView(r->device, img->ds_view, NULL);
        vkDestroyImage(r->device, img->image, NULL);
        vkFreeMemory(r->device, img->memory, NULL);
        memset(img, 0, sizeof(*img));
        r->image_cache_pressure = true;
        return NULL;
    }
    if (depth)
        LOG_I(GPU, "host image %ux%ux%u (3D) format %d for guest 0x%llx", width, height, depth, (int)format,
              (unsigned long long)addr);
    else if (layers > 1)
        LOG_I(GPU, "host image %ux%u x%u layers format %d for guest 0x%llx", width, height, layers, (int)format,
              (unsigned long long)addr);
    else
        LOG_I(GPU, "host image %ux%u format %d for guest 0x%llx", width, height, (int)format, (unsigned long long)addr);
    return img;
}

/* The Vulkan objects of an image: image, memory, the whole-image views. */
static bool create_objects(renderer *r, vk_image *img, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                           bool mutable_fmt,uint32_t first) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = img->depth ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = width;
    ci.extent.height = height;
    ci.extent.depth = img->depth ? img->depth : 1;
    ci.mipLevels = img->levels ? img->levels : 1;
    ci.arrayLayers = img->depth ? 1 : layers;
    if (img->depth) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; /* its slices are render targets */
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    bool depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM;
    bool compressed = format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* compute shaders may write any surface (image_store) where the format allows it */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r->phys, format, &fp);
    /* R64 atomic-only images need no sampling support. Do not request a
     * usage the format cannot provide, including for null atomic resources. */
    if(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)ci.usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!compressed && depth && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!compressed && !depth && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    img->color_attachment=(ci.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)!=0;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        img->storage = true;
    }
    /* color images may be viewed as same-size integer/float formats (the list keeps the
     * driver's compression where it can); storage through such a view is allowed even
     * when the image's own format is not writable (extended usage) */
    VkFormat aliases[VK_IMAGE_ALIAS_CAPACITY];
    VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (mutable_fmt && !depth && !compressed && vk_format_bytes(format)) {
        fl.viewFormatCount = vk_alias_formats(format, aliases);
        fl.pViewFormats = aliases;
        if (r->format_list) ci.pNext = &fl;
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        img->mutable_fmt = true;
    }
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageFormatProperties supported = {0};
    VkResult support = vkGetPhysicalDeviceImageFormatProperties(r->phys, ci.format, ci.imageType,
                                                                ci.tiling, ci.usage, ci.flags, &supported);
    if (!vk_image_format_valid(support, &ci, &supported)) {
        static uint32_t logged;
        if (logged++ < 16)
            LOG_W(GPU, "unsupported image request %ux%ux%u, %u layers, format %d, usage 0x%x, flags 0x%x"
                       " at 0x%llx (query %d): no creation",
                  ci.extent.width, ci.extent.height, ci.extent.depth, ci.arrayLayers, (int)ci.format,
                  (unsigned)ci.usage, (unsigned)ci.flags, (unsigned long long)img->addr, (int)support);
        return false;
    }
    img->sparse_first=0;img->allocated_bytes=0;
    if(vk_sparse_image_create(r,&ci,first,&img->image,&img->memory,&img->allocated_bytes)) {
        img->sparse_first=first;
    } else {
        if (!check(r, vkCreateImage(r->device, &ci, NULL, &img->image), "vkCreateImage")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(r->device, img->image, &req);
        if (!vk_alloc_memory(r, &req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &img->memory)) {
            vkDestroyImage(r->device, img->image, NULL);
            img->image = VK_NULL_HANDLE;
            return false;
        }
        if (!check(r, vkBindImageMemory(r->device, img->image, img->memory, 0), "vkBindImageMemory")) {
            vkDestroyImage(r->device, img->image, NULL);
            vkFreeMemory(r->device, img->memory, NULL);
            img->image = VK_NULL_HANDLE, img->memory = VK_NULL_HANDLE;
            return false;
        }
        img->allocated_bytes=req.size;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = img->depth ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    /* Extended image usage does not extend a view format's capabilities.
     * In particular, an sRGB sampled view must not inherit storage usage. */
    VkImageViewUsageCreateInfo view_usage={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    view_usage.usage=ci.usage;
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) view_usage.usage&=~VK_IMAGE_USAGE_STORAGE_BIT;
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) view_usage.usage&=~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) view_usage.usage&=~VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vi.pNext=&view_usage;
    vkCreateImageView(r->device, &vi, NULL, &img->view);
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.subresourceRange.layerCount = img->depth ? img->depth : layers; /* 3D: the slices, for rendering */
    if (img->depth) { /* such a view is an attachment only */
        static VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        vi.pNext = &usage;
    }
    vkCreateImageView(r->device, &vi, NULL, &img->array_view);
    img->aspect = vi.subresourceRange.aspectMask;
    if (vk_format_has_stencil(format)) {
        vi.pNext = NULL;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(r->device, &vi, NULL, &img->ds_view);
    }
    return true;
}

static void free_feedback(renderer *r, vk_image *source) {
    vk_image *copy = source->feedback;
    if (!copy) return;
    for (uint32_t k = 0; k < copy->sub_count; k++) vkDestroyImageView(r->device, copy->sub[k].view, NULL);
    free(copy->sub);
    if (copy->view) vkDestroyImageView(r->device, copy->view, NULL);
    if (copy->array_view) vkDestroyImageView(r->device, copy->array_view, NULL);
    if (copy->ds_view) vkDestroyImageView(r->device, copy->ds_view, NULL);
    if (copy->stencil_view) vkDestroyImageView(r->device, copy->stencil_view, NULL);
    if (copy->image) vkDestroyImage(r->device, copy->image, NULL);
    if (copy->memory) vkFreeMemory(r->device, copy->memory, NULL);
    free(copy);
    source->feedback = NULL;
}

static void discard_captures(renderer *r) {
    for(uint32_t i=0;i<r->capture_count;i++) {
        vk_image owner={0};owner.feedback=r->captures[i].image;
        free_feedback(r,&owner);
    }
    r->capture_count=0;
}

vk_image *vk_image_snapshot(renderer *r,vk_image *source) {
    if(!source)return NULL;
    vk_image *feedback=source->feedback;
    source->feedback=NULL;
    vk_image *snapshot=vk_image_feedback(r,source);
    source->feedback=feedback;
    return snapshot;
}

void vk_image_snapshot_destroy(renderer *r,vk_image *snapshot) {
    vk_image owner={0};owner.feedback=snapshot;
    free_feedback(r,&owner);
}

/* Snapshot ownership is independent of a later feedback refresh. No fence
 * or host readback here: copies stay ordered with the frame's GPU work. */
bool vk_capture_defer(renderer *r,vk_image *source,const char *path) {
    if(r->capture_count==PS5C_ARRAY_COUNT(r->captures))return false;
    vk_image *copy=vk_image_feedback(r,source);
    if(!copy)return false;
    source->feedback=NULL;
    uint32_t i=r->capture_count++;
    r->captures[i].image=copy;
    snprintf(r->captures[i].path,sizeof(r->captures[i].path),"%s",path);
    return true;
}

vk_image *vk_image_feedback(renderer *r, vk_image *source) {
    if (!source || source->depth || source->levels > 16) return NULL;
    vk_image *copy = source->feedback;
    if (!copy) {
        copy = calloc(1, sizeof(*copy));
        if (!copy) return NULL;
        source->feedback = copy;
        copy->addr = source->addr; /* diagnostic identity only; never inserted into r->images */
        copy->phys = VK_NO_PHYS;
        copy->width = source->width; copy->height = source->height;
        copy->layers = source->layers; copy->levels = source->levels;
        copy->format = source->format;
        copy->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (!create_objects(r, copy, copy->format, copy->width, copy->height, copy->layers, source->mutable_fmt, 0)) {
            free_feedback(r, source);
            return NULL;
        }
    }
    if (source->gpu_written && copy->gpu_written && copy->written_seq == source->written_seq) return copy;
    vk_work_end(r);
    vk_barrier(r->cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk_barrier(r->cmd, copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy regions[16] = {0};
    uint32_t levels = source->levels ? source->levels : 1;
    for (uint32_t level = 0; level < levels; level++) {
        VkImageCopy *region = &regions[level];
        VkImageAspectFlags aspect = vk_format_has_stencil(source->format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                                           : source->aspect;
        region->srcSubresource = region->dstSubresource = (VkImageSubresourceLayers){aspect, level, 0, source->layers};
        region->extent.width = source->width >> level ? source->width >> level : 1;
        region->extent.height = source->height >> level ? source->height >> level : 1;
        region->extent.depth = 1;
    }
    vkCmdCopyImage(r->cmd, source->image, source->layout, copy->image, copy->layout, levels, regions);
    copy->gpu_written = true;
    copy->written_seq = source->written_seq;
    return copy;
}

/* R8/RG8 sRGB are optional Vulkan formats. Expand their raw UNORM channels
 * before sampling, then let an RGBA8 sRGB view decode BEFORE filtering.
 * The source remains at its guest byte size, including GPU-written aliases. */
vk_image *vk_image_srgb(renderer *r, vk_image *source) {
    if(!source || (source->format!=VK_FORMAT_R8_UNORM && source->format!=VK_FORMAT_R8G8_UNORM) || source->levels>16) return NULL;
    VkFormatProperties sf,df;
    vkGetPhysicalDeviceFormatProperties(r->phys,source->format,&sf);
    vkGetPhysicalDeviceFormatProperties(r->phys,VK_FORMAT_R8G8B8A8_UNORM,&df);
    if(!(sf.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(df.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)) return NULL;
    vk_image *copy=source->feedback;
    if(!copy) {
        copy=calloc(1,sizeof(*copy));if(!copy) return NULL;
        source->feedback=copy;
        copy->addr=source->addr;copy->phys=VK_NO_PHYS;copy->width=source->width;copy->height=source->height;
        copy->layers=source->layers;copy->levels=source->levels;copy->depth=source->depth;
        copy->format=VK_FORMAT_R8G8B8A8_UNORM;copy->stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        copy->expanded_srgb=true;
        if(!create_objects(r,copy,copy->format,copy->width,copy->height,copy->layers,true,0)) {
            free_feedback(r,source);return NULL;
        }
    }
    if(!copy->expanded_srgb) return NULL;
    if(source->gpu_written && copy->gpu_written && copy->written_seq==source->written_seq) return copy;
    vk_work_end(r);
    vk_barrier(r->cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk_barrier(r->cmd,copy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit regions[16]={0};uint32_t levels=source->levels?source->levels:1;
    for(uint32_t k=0;k<levels;k++) {
        VkImageBlit *b=&regions[k];
        b->srcSubresource=b->dstSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,k,0,source->depth?1:source->layers};
        b->srcOffsets[1]=b->dstOffsets[1]=(VkOffset3D){source->width>>k?source->width>>k:1,
            source->height>>k?source->height>>k:1,source->depth?(source->depth>>k?source->depth>>k:1):1};
    }
    vkCmdBlitImage(r->cmd,source->image,source->layout,copy->image,copy->layout,levels,regions,VK_FILTER_NEAREST);
    copy->gpu_written=true;copy->written_seq=source->written_seq;
    return copy;
}

/* Objects of promoted images, destroyed once the batch that copied from them completed. */
#define MAX_RETIRED 64
static struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, array_view;
    struct vk_image_sub *sub;
    uint32_t sub_count;
} g_retired[MAX_RETIRED];
static uint32_t g_retired_count;

static void free_retired(renderer *r) {
    for (uint32_t i = 0; i < g_retired_count; i++) {
        vk_pipeline_forget_view(r, g_retired[i].view);
        vk_pipeline_forget_view(r, g_retired[i].array_view);
        for (uint32_t k = 0; k < g_retired[i].sub_count; k++) vk_pipeline_forget_view(r, g_retired[i].sub[k].view);
        vkDestroyImageView(r->device, g_retired[i].view, NULL);
        vkDestroyImageView(r->device, g_retired[i].array_view, NULL);
        for (uint32_t k = 0; k < g_retired[i].sub_count; k++) vkDestroyImageView(r->device, g_retired[i].sub[k].view, NULL);
        free(g_retired[i].sub);
        vkDestroyImage(r->device, g_retired[i].image, NULL);
        vkFreeMemory(r->device, g_retired[i].memory, NULL);
    }
    g_retired_count = 0;
}

bool vk_image_promote(renderer *r, vk_image *img) {
    if (img->mutable_fmt) return true;
    uint32_t levels = img->levels ? img->levels : 1;
    if (levels > 32) return false;
    if (img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !vk_format_bytes(img->format) || g_retired_count == MAX_RETIRED) return false;
    vk_image n = *img;
    n.sub = NULL, n.sub_count = n.sub_cap = 0;
    if (!create_objects(r, &n, img->format, img->width, img->height, img->layers, true, 0)) return false;
    n.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    n.access = 0;
    n.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkCommandBuffer cmd = vk_begin(r);
    if (img->layout != VK_IMAGE_LAYOUT_UNDEFINED) { /* what the image holds moves over */
        vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk_barrier(cmd, &n, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        /* A chain keeps its content/version metadata during promotion, so
         * all levels must move too. Copying only mip zero left valid lower
         * levels black when an integer upload was sampled through UNORM. */
        VkImageCopy copies[32] = {0};
        for (uint32_t level = 0; level < levels; level++) {
            VkImageCopy *c = &copies[level];
            c->srcSubresource = c->dstSubresource = (VkImageSubresourceLayers){img->aspect, level, 0, img->layers};
            c->extent.width = img->width >> level ? img->width >> level : 1;
            c->extent.height = img->height >> level ? img->height >> level : 1;
            c->extent.depth = img->depth >> level ? img->depth >> level : 1;
        }
        vkCmdCopyImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, n.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies);
    }
    uint32_t k = g_retired_count++;
    g_retired[k].image = img->image;
    g_retired[k].memory = img->memory;
    g_retired[k].view = img->view;
    g_retired[k].array_view = img->array_view;
    g_retired[k].sub = img->sub;
    g_retired[k].sub_count = img->sub_count;
    *img = n;
    LOG_I(GPU, "host image 0x%llx %ux%u format %d: views may reinterpret it now", (unsigned long long)img->addr, img->width,
          img->height, (int)img->format);
    return true;
}

/* ---- Frame recording ---- */

VkCommandBuffer vk_begin(renderer *r) {
    if (r->recording) return r->cmd;
    /* Each slot keeps its descriptors and CPU snapshots until its own fence.
     * Promotions retain old image views globally: finish their owning batch
     * before retiring them rather than switching slots in that case. */
    bool rotated=!g_retired_count && vk_work_can_overlap(r);
    if(rotated)r->work_index^=1;
    vk_stat_work_slot(rotated);
    r->cmd=r->work_cmds[r->work_index];
    r->work_fence=r->work_fences[r->work_index];
    /* the work command buffer is reused after its fence signals */
    uint64_t t0 = vk_stat_now();
    vk_check_work(r, vk_work_wait(r, UINT64_MAX), "vkWaitForFences (work)");
    vk_stat_gpu_wait(t0);
    if (g_retired_count) free_retired(r); /* the batch that copied from them is done */
    if (!r->work_timeline) vk_check_work(r, vkResetFences(r->device, 1, &r->work_fence), "vkResetFences (work)");
    vk_check_work(r, vkResetCommandBuffer(r->cmd, 0), "vkResetCommandBuffer (work)");
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check_work(r, vkBeginCommandBuffer(r->cmd, &bi), "vkBeginCommandBuffer (work)");
    r->recording = true;
    r->use_seq++;
    g_recording_seq = r->use_seq;
    r->image_cache_pressure = false;
    vk_work_begin(r);
    return r->cmd;
}

/* Waits for the current work slot's last submit (its fence, or its value on the timeline). */
VkResult vk_work_wait(renderer *r, uint64_t timeout_ns) {
    if (!r->work_timeline) return vkWaitForFences(r->device, 1, &r->work_fence, VK_TRUE, timeout_ns);
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &r->work_timeline, .pValues = &r->work_value[r->work_index]};
    return r->wait_semaphores(r->device, &wi, timeout_ns);
}

void vk_flush(renderer *r) {
    if (!r->recording) return;
    vk_work_end(r);
    vk_work_profile_end(r);
    vk_check_work(r, vkEndCommandBuffer(r->cmd), "vkEndCommandBuffer (work)");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &r->cmd;
    uint64_t value = r->work_submitted + 1;
    VkTimelineSemaphoreSubmitInfo ti = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value};
    if (r->work_timeline) {
        si.pNext = &ti;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &r->work_timeline;
    }
    VkResult submitted = vkQueueSubmit(r->queue, 1, &si, r->work_timeline ? VK_NULL_HANDLE : r->work_fence);
    vk_check_work(r, submitted, "vkQueueSubmit (work)");
    if (r->work_timeline && submitted == VK_SUCCESS) r->work_value[r->work_index] = r->work_submitted = value;
    r->recording = false;
}

/* ---- Presentation ---- */

static VkFormat surface_format(uint64_t pixel_format) {
    /* VideoOut: 0x80000000 A8R8G8B8 sRGB, 0x80002200 A8B8G8R8 sRGB, 0x88060000 A2R10G10B10;
     * the 64-bit forms of the PS5 SDK: 0x8000000000000000 B8G8R8A8, 0x8000000022000000 R8G8B8A8 */
    switch (pixel_format) {
    case 0x80002200ull:
    case 0x8000000022000000ull: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x88000000ull: /* A2R10G10B10 sRGB */
    case 0x88060000ull: /* A2R10G10B10 */
    case 0x88740000ull: /* A2R10G10B10 BT.2020 PQ (shown without the HDR curve) */
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

/* PS5C_SAVE_FRAMES=<n>: every n-th presented frame goes to user/frames/NNNNN.bmp;
 * PS5C_SAVE_TARGETS=<n>: at the n-th present, every GPU-written image goes to
 * user/frames/rt_<address>_<w>x<h>_f<format>.bmp. Large images at a quarter
 * of their size, HDR values clamped (debugging aids). */
static uint32_t env_u32(const char *name, int *cache) {
    if (*cache < 0) *cache = getenv(name) ? atoi(getenv(name)) : 0;
    return *cache > 0 ? (uint32_t)*cache : 0;
}

/* Optional live capture cadence for timing-sensitive graphics diagnostics.
 * Changing captures must not require restarting the guest or its scene. */
static uint32_t capture_interval(void) {
    static int every = -1;
    static bool checked;
    static const char *file;
    static uint64_t next_read;
    static uint32_t value;
    uint32_t fallback = env_u32("PS5C_SAVE_FRAMES", &every);
    if (!checked) {
        file = getenv("PS5C_SAVE_FRAMES_RATE_FILE");
        value = fallback;
        checked = true;
    }
    if (!file) return fallback;
    uint64_t now = pal_time_ns();
    if (now >= next_read) {
        next_read = now + 100000000ull;
        FILE *f = fopen(file, "r");
        unsigned n;
        if (f) {
            if (fscanf(f, "%u", &n) == 1 && n <= 10000 && n != value) {
                value = n;
                LOG_I(GPU, "diagnostic frame capture interval: %u", value);
            }
            fclose(f);
        }
    }
    return value;
}

static float small_float(uint32_t e, uint32_t m, uint32_t mbits) { /* 5-bit exponent floats */
    if (e == 0) return (float)m / (float)(1u << mbits) / 16384.0f;
    if (e == 31) return 65504.0f;
    return (1.0f + (float)m / (float)(1u << mbits)) * (float)(1u << e) / 32768.0f;
}

static uint8_t to8(float f) {
    if (!(f > 0.0f)) return 0;
    return f >= 1.0f ? 255 : (uint8_t)(f * 255.0f + 0.5f);
}

/* One texel as 8-bit RGB; false if the format is not handled. */
static bool texel_rgb(VkFormat fmt, const uint8_t *t, uint8_t *rgb) {
    uint32_t p;
    switch (fmt) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT: rgb[0] = t[0], rgb[1] = t[1], rgb[2] = t[2]; return true;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: rgb[0] = t[2], rgb[1] = t[1], rgb[2] = t[0]; return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        memcpy(&p, t, 4);
        rgb[0] = (uint8_t)(p >> 2), rgb[1] = (uint8_t)(p >> 12), rgb[2] = (uint8_t)(p >> 22);
        return true;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        memcpy(&p, t, 4);
        rgb[2] = (uint8_t)(p >> 2), rgb[1] = (uint8_t)(p >> 12), rgb[0] = (uint8_t)(p >> 22);
        return true;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        memcpy(&p, t, 4);
        rgb[0] = to8(small_float((p >> 6) & 0x1F, p & 0x3F, 6));
        rgb[1] = to8(small_float((p >> 17) & 0x1F, (p >> 11) & 0x3F, 6));
        rgb[2] = to8(small_float((p >> 27) & 0x1F, (p >> 22) & 0x1F, 5));
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16_SFLOAT: {
        uint16_t h[3] = {0, 0, 0};
        memcpy(h, t, fmt == VK_FORMAT_R16_SFLOAT ? 2 : fmt == VK_FORMAT_R16G16_SFLOAT ? 4 : 6);
        for (int k = 0; k < 3; k++) {
            float v = small_float((h[k] >> 10) & 0x1F, h[k] & 0x3FF, 10);
            rgb[k] = to8((h[k] & 0x8000) ? -v : v);
        }
        if (fmt == VK_FORMAT_R16_SFLOAT) rgb[1] = rgb[2] = rgb[0];
        return true;
    }
    case VK_FORMAT_R32_SFLOAT: {
        float f;
        memcpy(&f, t, 4);
        rgb[0] = rgb[1] = rgb[2] = to8(f);
        return true;
    }
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
        float f[3];
        memcpy(f, t, sizeof(f));
        for (int k = 0; k < 3; k++) rgb[k] = to8(f[k]);
        return true;
    }
    case VK_FORMAT_R8_UNORM: rgb[0] = rgb[1] = rgb[2] = t[0]; return true;
    case VK_FORMAT_R8G8_UNORM: rgb[0] = t[0], rgb[1] = t[1], rgb[2] = 0; return true;
    case VK_FORMAT_R16G16B16A16_UNORM: rgb[0] = t[1], rgb[1] = t[3], rgb[2] = t[5]; return true;
    case VK_FORMAT_R16G16_UNORM: rgb[0] = t[1], rgb[1] = t[3], rgb[2] = 0; return true;
    default: return false;
    }
}

static uint32_t format_bytes(VkFormat fmt) {
    switch (fmt) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_UINT: return 8;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R8_UNORM: return 1;
    default: return 4;
    }
}

/* Copies mip 0 / layer 0 of an image (one aspect) into a new host buffer and waits (debugging aids). */
static bool readback(renderer *r, vk_image *src, VkImageAspectFlags aspect, uint32_t layer, uint64_t size, vk_buffer *buf) {
    if (!vk_buffer_create(r, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf)) return false;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = r->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(r->device, &ai, &cmd) != VK_SUCCESS) {
        vk_buffer_destroy(r, buf);
        return false;
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vk_check_work(r, vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer (transfer)");
    vk_barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = aspect;
    c.imageSubresource.baseArrayLayer = layer;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = src->width;
    c.imageExtent.height = src->height;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf->buffer, 1, &c);
    vk_check_work(r, vkEndCommandBuffer(cmd), "vkEndCommandBuffer (transfer)");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vk_check_work(r, vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (transfer)");
    vk_check_work(r, vkQueueWaitIdle(r->queue), "vkQueueWaitIdle (transfer)");
    vkFreeCommandBuffers(r->device, r->pool, 1, &cmd);
    return true;
}

/* Texels of a color image that are not all zero (debugging aid: PS5C_DEPTH_STATS_FRAME), or -1. */
int64_t vk_color_coverage(renderer *r, vk_image *src) {
    uint32_t bpp = vk_format_bytes(src->format);
    if (src->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !bpp || bpp > 16) return -1;
    uint64_t n = (uint64_t)src->width * src->height;
    vk_buffer buf;
    if (!readback(r, src, VK_IMAGE_ASPECT_COLOR_BIT, 0, n * bpp, &buf)) return -1;
    static const uint8_t zero[16];
    int64_t nonzero = 0;
    for (uint64_t i = 0; i < n; i++) nonzero += memcmp((const uint8_t *)buf.mapped + i * bpp, zero, bpp) != 0;
    vk_buffer_destroy(r, &buf);
    return nonzero;
}

/* A D32 depth image, every layer: statistics in the log and a grey picture (min..max of each
 * layer, layers one below the other, every 4th texel) at `path` (debugging aid: PS5C_SAVE_TEXTURES). */
void vk_save_depth(renderer *r, vk_image *src, const char *path) {
    if (src->format != VK_FORMAT_D32_SFLOAT && src->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint32_t w = src->width / 4, h = src->height / 4, layers = src->layers ? src->layers : 1, row = (w * 3 + 3) & ~3u;
    uint8_t *bmp = w && h ? (uint8_t *)calloc(1, 54 + (size_t)row * h * layers) : NULL;
    if (!bmp) return;
    uint64_t n = (uint64_t)src->width * src->height;
    for (uint32_t l = 0; l < layers; l++) {
        vk_buffer buf;
        if (!readback(r, src, VK_IMAGE_ASPECT_DEPTH_BIT, l, n * 4, &buf)) break;
        const float *z = (const float *)buf.mapped;
        double sum = 0;
        float lo = 1e30f, hi = -1e30f;
        uint64_t zero = 0, one = 0;
        for (uint64_t i = 0; i < n; i++) {
            sum += z[i];
            if (z[i] < lo) lo = z[i];
            if (z[i] > hi) hi = z[i];
            zero += z[i] == 0.0f;
            one += z[i] == 1.0f;
        }
        LOG_I(GPU, "depth %s layer %u: min %g max %g mean %g, %llu at 0, %llu at 1 of %llu", path, l, lo, hi, sum / (double)n,
              (unsigned long long)zero, (unsigned long long)one, (unsigned long long)n);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                float f = z[(uint64_t)y * 4 * src->width + x * 4];
                uint8_t g = hi > lo ? (uint8_t)((f - lo) / (hi - lo) * 255.0f) : 0;
                uint8_t *px = bmp + 54 + (size_t)(h * layers - 1 - (l * h + y)) * row + x * 3; /* bottom-up rows */
                px[0] = px[1] = px[2] = g;
            }
        vk_buffer_destroy(r, &buf);
    }
    uint32_t file = 54 + row * h * layers, v, height = h * layers;
    bmp[0] = 'B';
    bmp[1] = 'M';
    memcpy(bmp + 2, &file, 4);
    v = 54;
    memcpy(bmp + 10, &v, 4);
    v = 40;
    memcpy(bmp + 14, &v, 4);
    memcpy(bmp + 18, &w, 4);
    memcpy(bmp + 22, &height, 4);
    uint16_t planes = 1, bits = 24;
    memcpy(bmp + 26, &planes, 2);
    memcpy(bmp + 28, &bits, 2);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(bmp, 1, file, f);
        fclose(f);
    }
    free(bmp);
}

/* Float statistics of a D32 depth image (debugging aid: PS5C_DEPTH_STATS_FRAME). */
void vk_depth_stats(renderer *r, vk_image *src, const char *what) {
    if (src->format != VK_FORMAT_D32_SFLOAT && src->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    vk_buffer buf;
    uint64_t n = (uint64_t)src->width * src->height;
    if (!readback(r, src, VK_IMAGE_ASPECT_DEPTH_BIT, 0, n * 4, &buf)) return;
    double sum = 0, hi = 0;
    uint64_t nonzero = 0;
    for (uint64_t i = 0; i < n; i++) {
        float f;
        memcpy(&f, (const uint8_t *)buf.mapped + i * 4, 4);
        sum += f;
        if (f > hi) hi = f;
        nonzero += f != 0.0f;
    }
    LOG_I(GPU, "depth stats %s 0x%llx: max %g mean %g, nonzero %llu of %llu", what, (unsigned long long)src->addr, hi,
          sum / (double)n, (unsigned long long)nonzero, (unsigned long long)n);
    vk_buffer_destroy(r, &buf);
}

void vk_save_image(renderer *r, vk_image *src, const char *path) {
    uint8_t probe[16] = {0}, rgb[3];
    bool have_rgb = texel_rgb(src->format, probe, rgb);
    bool raw_requested = getenv("PS5C_SAVE_RAW") ||
        (getenv("PS5C_SAVE_RAW_TARGETS") && strstr(path, "rt_")) ||
        (getenv("PS5C_SAVE_RAW_TEXTURES") && strstr(path, "tex_"));
    bool raw_integer = (src->format == VK_FORMAT_R32_UINT || src->format == VK_FORMAT_R32G32_UINT) && raw_requested;
    if (src->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (!have_rgb && !raw_integer)) return;
    uint32_t bpp = format_bytes(src->format), slices = src->depth > 1 ? src->depth : 1; /* 3D: the slices one below the other */
    vk_buffer buf;
    uint64_t size = (uint64_t)src->width * src->height * slices * bpp;
    if (!vk_buffer_create(r, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &buf)) return;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = r->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(r->device, &ai, &cmd) != VK_SUCCESS) {
        vk_buffer_destroy(r, &buf);
        return;
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vk_check_work(r, vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer (transfer)");
    vk_barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = src->width;
    c.imageExtent.height = src->height;
    c.imageExtent.depth = slices;
    vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &c);
    vk_check_work(r, vkEndCommandBuffer(cmd), "vkEndCommandBuffer (transfer)");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vk_check_work(r, vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (transfer)");
    vk_check_work(r, vkQueueWaitIdle(r->queue), "vkQueueWaitIdle (transfer)");
    vkFreeCommandBuffers(r->device, r->pool, 1, &cmd);

    if (raw_requested) {
        /* Every channel, as the image holds them: <path>.raw. The texture-only
         * option avoids full-resolution raw frame dumps when inspecting alpha. */
        char raw[300];
        snprintf(raw, sizeof(raw), "%s.raw", path);
        FILE *f = fopen(raw, "wb");
        if (f) {
            fwrite(buf.mapped, 1, (size_t)size, f);
            fclose(f);
        }
    }
    if (!have_rgb) { /* Integer material targets provide exact bytes only. */
        vk_buffer_destroy(r, &buf);
        return;
    }
    if (src->format == VK_FORMAT_R16G16B16A16_SFLOAT || src->format == VK_FORMAT_R32_SFLOAT ||
        src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || src->format == VK_FORMAT_R32G32B32A32_SFLOAT) {
        /* float statistics of channels R, G, B (debugging: values an 8-bit picture rounds to 0 or 255) */
        for (uint32_t ch = 0; ch < (src->format == VK_FORMAT_R32_SFLOAT ? 1u : src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 3u : 4u); ch++) {
        double sum = 0, lo = 1e30, hi = -1e30;
        uint64_t nonzero = 0, nans = 0, infs = 0, n = (uint64_t)src->width * src->height;
        for (uint64_t i = 0; i < n; i++) {
            float f;
            if (src->format == VK_FORMAT_R32_SFLOAT || src->format == VK_FORMAT_R32G32B32A32_SFLOAT) {
                memcpy(&f, (const uint8_t *)buf.mapped + i * bpp + ch * 4, 4);
            } else if (src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
                uint32_t packed, bits, mbits = ch == 2 ? 5 : 6;
                memcpy(&packed, (const uint8_t *)buf.mapped + i * 4, 4);
                uint32_t v = packed >> (ch == 0 ? 0 : ch == 1 ? 11 : 22);
                uint32_t e = (v >> mbits) & 31, m = v & ((1u << mbits) - 1);
                if (e == 0) f = (float)m / (float)(1u << (mbits + 14));
                else {
                    bits = ((e == 31 ? 255u : e + 112) << 23) | (m << (23 - mbits));
                    memcpy(&f, &bits, 4);
                }
            } else {
                uint16_t hv;
                memcpy(&hv, (const uint8_t *)buf.mapped + i * 8 + ch * 2, 2);
                uint32_t e = (hv >> 10) & 0x1F, m = hv & 0x3FF, bits;
                if (e == 0) f = (float)m / 16777216.0f;
                else if (e == 31) bits = ((uint32_t)(hv & 0x8000) << 16) | 0x7F800000u | (m << 13), memcpy(&f, &bits, 4);
                else bits = ((uint32_t)(hv & 0x8000) << 16) | ((e + 112) << 23) | (m << 13), memcpy(&f, &bits, 4);
                if (e == 0 && (hv & 0x8000)) f = -f;
            }
            nonzero += f != 0.0f;
            if (f != f) { nans++; continue; }
            if (!isfinite(f)) { infs++; continue; }
            sum += f;
            if (f < lo) lo = f;
            if (f > hi) hi = f;
        }
        LOG_I(GPU, "image stats %s: %c min %g max %g mean %g (finite), nonzero %llu, NaN %llu, inf %llu of %llu", path, "RGBA"[ch],
              lo, hi, sum / (double)(n - nans - infs ? n - nans - infs : 1), (unsigned long long)nonzero, (unsigned long long)nans,
              (unsigned long long)infs, (unsigned long long)n);
        }
    }
    uint32_t step = src->width >= 1024 && !getenv("PS5C_SAVE_FULL") ? 4 : 1;
    uint32_t w = src->width / step, h = src->height * slices / step, row = (w * 3 + 3) & ~3u;
    uint8_t *bmp = w && h ? (uint8_t *)calloc(1, 54 + (size_t)row * h) : NULL;
    if (bmp) {
        uint32_t file = 54 + row * h, v;
        bmp[0] = 'B';
        bmp[1] = 'M';
        memcpy(bmp + 2, &file, 4);
        v = 54;
        memcpy(bmp + 10, &v, 4);
        v = 40;
        memcpy(bmp + 14, &v, 4);
        memcpy(bmp + 18, &w, 4);
        memcpy(bmp + 22, &h, 4);
        bmp[26] = 1;
        bmp[28] = 24;
        const uint8_t *px = (const uint8_t *)buf.mapped;
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *o = bmp + 54 + (size_t)(h - 1 - y) * row;
            for (uint32_t x = 0; x < w; x++) {
                texel_rgb(src->format, px + ((size_t)y * step * src->width + (size_t)x * step) * bpp, rgb);
                o[3 * x] = rgb[2];
                o[3 * x + 1] = rgb[1];
                o[3 * x + 2] = rgb[0];
            }
        }
        pal_mkdir("user");
        pal_mkdir("user/frames");
        pal_file *f = NULL;
        if (pal_file_open(path, PAL_FILE_WRITE | PAL_FILE_CREATE | PAL_FILE_TRUNCATE, &f) == PS5C_OK) {
            pal_file_write(f, bmp, file, NULL);
            pal_file_close(f);
            LOG_I(GPU, "saved %s", path);
        }
        free(bmp);
    }
    vk_buffer_destroy(r, &buf);
}

static void save_images(renderer *r, uint32_t number, vk_image *src) {
    static int targets = -1;
    char path[96];
    if(r->capture_count) {
        /* The caller has submitted all game work for this frame. Preserve
         * its final image alongside the intermediate snapshots. */
        if(src && src->gpu_written) {
            snprintf(path,sizeof(path),"user/frames/deferred_final_%05u.bmp",number);
            vk_save_image(r,src,path);
        }
        for(uint32_t i=0;i<r->capture_count;i++) {
            vk_image *image=r->captures[i].image;
            if(image->aspect==VK_IMAGE_ASPECT_COLOR_BIT)vk_save_image(r,image,r->captures[i].path);
            else vk_save_depth(r,image,r->captures[i].path);
        }
        discard_captures(r);
    }
    uint32_t n = capture_interval();
    if (src && src->gpu_written && n && number % n == 0) {
        snprintf(path, sizeof(path), "user/frames/%05u.bmp", number);
        vk_save_image(r, src, path);
    }
    /* PS5C_SAVE_TARGETS=@<file>: at the first present after <file> appears (then removed) */
    const char *st = getenv("PS5C_SAVE_TARGETS");
    pal_path_info trig;
    bool by_file = st && st[0] == '@' && pal_path_stat(st + 1, &trig) == PS5C_OK;
    if (by_file) pal_file_remove(st + 1);
    if (by_file || (!(st && st[0] == '@') && env_u32("PS5C_SAVE_TARGETS", &targets) == number)) {
        bool textures = getenv("PS5C_SAVE_TEXTURES") != NULL;
        /* PS5C_SAVE_TARGETS_FORMAT=<VkFormat>: only images of that format (with PS5C_SAVE_FULL, at full size) */
        int only = getenv("PS5C_SAVE_TARGETS_FORMAT") ? atoi(getenv("PS5C_SAVE_TARGETS_FORMAT")) : 0;
        /* Optional bounded target list, read only when the capture is armed.
         * Missing/invalid lists save nothing, rather than dumping every image.
         * Hex guest addresses, inline or in @file; no rendering state changes. */
        const char *selection = getenv("PS5C_SAVE_TARGETS_ADDRS");
        guest_addr selected[16];uint32_t selected_count = 0;
        if (selection) {
            char values[512] = {0};
            if (*selection == '@') {
                FILE *file = fopen(selection + 1, "rb");
                if (file) { size_t bytes_read = fread(values, 1, sizeof(values)-1, file); values[bytes_read] = 0; fclose(file); }
            } else snprintf(values, sizeof(values), "%s", selection);
            for (char *next = values; *next && selected_count < PS5C_ARRAY_COUNT(selected);) {
                while (*next == ' ' || *next == '\t' || *next == '\r' || *next == '\n' || *next == ',') next++;
                if (!*next) break;
                char *end; unsigned long long address = strtoull(next, &end, 16);
                if (end == next) break;
                selected[selected_count++] = (guest_addr)address; next = end;
            }
        }
        for (uint32_t i = 0; i < r->image_count; i++) {
            vk_image *img = &r->images[i];
            if ((!img->gpu_written && !textures) || !img->image || (only && (int)img->format != only)) continue;
            if (selection) {
                bool match = false;
                for (uint32_t j = 0; j < selected_count; j++) match |= img->addr == selected[j];
                if (!match) continue;
            }
            snprintf(path, sizeof(path), "user/frames/%s_%010llx_%ux%u_f%d.bmp", img->gpu_written ? "rt" : "tex",
                     (unsigned long long)img->addr, img->width, img->height, (int)img->format);
            if (img->format == VK_FORMAT_D32_SFLOAT || img->format == VK_FORMAT_D32_SFLOAT_S8_UINT) vk_save_depth(r, img, path);
            else vk_save_image(r, img, path);
        }
    }
}

bool renderer_set_splash(renderer *r, const void *bc7, size_t size, uint32_t width, uint32_t height, bool srgb) {
    VkFormat fmt = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r->phys, fmt, &fp);
    size_t need = (size_t)((width + 3) / 4) * ((height + 3) / 4) * 16;
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || size < need) return false;
    pal_mutex_lock(r->lock);
    vk_image *img = &r->splash;
    memset(img, 0, sizeof(*img));
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = width;
    ci.extent.height = height;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = vkCreateImage(r->device, &ci, NULL, &img->image) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(r->device, img->image, &req);
        ok = vk_alloc_memory(r, &req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &img->memory) &&
             vkBindImageMemory(r->device, img->image, img->memory, 0) == VK_SUCCESS;
    }
    ok = ok && vk_buffer_create(r, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &r->splash_staging);
    if (ok) {
        memcpy(r->splash_staging.mapped, bc7, need);
        img->width = width;
        img->height = height;
        img->format = fmt;
        img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        img->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        r->splash_on = true;
        r->splash_uploaded = false;
    } else {
        if (img->image) vkDestroyImage(r->device, img->image, NULL);
        if (img->memory) vkFreeMemory(r->device, img->memory, NULL);
        memset(img, 0, sizeof(*img));
    }
    pal_mutex_unlock(r->lock);
    return ok;
}

void renderer_hide_splash(renderer *r) {
    pal_mutex_lock(r->lock);
    r->splash_on = false; /* the image goes with the renderer: a present may still read it */
    pal_mutex_unlock(r->lock);
}

bool renderer_show_picture(renderer *r, const void *bgra, uint32_t width, uint32_t height) {
    if (!width || !height) return false;
    pal_mutex_lock(r->lock);
    vk_image *img = &r->picture;
    bool ok = true;
    if (!img->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_B8G8R8A8_UNORM;
        ci.extent.width = width;
        ci.extent.height = height;
        ci.extent.depth = 1;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ok = vkCreateImage(r->device, &ci, NULL, &img->image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(r->device, img->image, &req);
            ok = vk_alloc_memory(r, &req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &img->memory) &&
                 vkBindImageMemory(r->device, img->image, img->memory, 0) == VK_SUCCESS;
        }
        ok = ok && vk_buffer_create(r, (VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                                    &r->picture_staging);
        if (ok) {
            img->width = width;
            img->height = height;
            img->format = VK_FORMAT_B8G8R8A8_UNORM;
            img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            img->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        } else {
            if (img->image) vkDestroyImage(r->device, img->image, NULL);
            if (img->memory) vkFreeMemory(r->device, img->memory, NULL);
            vk_buffer_destroy(r, &r->picture_staging);
            memset(img, 0, sizeof(*img));
        }
    }
    ok = ok && img->width == width && img->height == height;
    if (ok) {
        /* the copy of the previous picture may still read the staging buffer */
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
            vk_check_work(r, vkWaitForFences(r->device, 1, &r->frames[i].fence, VK_TRUE, UINT64_MAX),
                          "vkWaitForFences (picture)");
        memcpy(r->picture_staging.mapped, bgra, (size_t)width * height * 4);
        r->picture_on = true;
        r->picture_dirty = true;
    }
    pal_mutex_unlock(r->lock);
    if (ok) renderer_present(r, NULL);
    return ok;
}

void renderer_hide_picture(renderer *r) {
    pal_mutex_lock(r->lock);
    r->picture_on = false;
    pal_mutex_unlock(r->lock);
}

/* An integer frame (the game's last pass stores it through a UINT descriptor;
 * the display reads the bytes as UNORM) is copied bit for bit into a UNORM
 * image the blit can scale. NULL when that image cannot be made. */
static vk_image *present_as_unorm(renderer *r, VkCommandBuffer cmd, vk_image *src) {
    vk_image *t = &r->present_tmp;
    VkFormat fmt = present_unorm_of(src->format);
    if (t->image && (t->width != src->width || t->height != src->height || t->format != fmt)) {
        vk_check_work(r, vkDeviceWaitIdle(r->device), "vkDeviceWaitIdle (transfer)");
        vkDestroyImage(r->device, t->image, NULL);
        vkFreeMemory(r->device, t->memory, NULL);
        memset(t, 0, sizeof(*t));
    }
    if (!t->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent.width = src->width;
        ci.extent.height = src->height;
        ci.extent.depth = 1;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkMemoryRequirements req;
        bool ok = vkCreateImage(r->device, &ci, NULL, &t->image) == VK_SUCCESS;
        if (ok) vkGetImageMemoryRequirements(r->device, t->image, &req);
        ok = ok && vk_alloc_memory(r, &req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->memory) &&
             vkBindImageMemory(r->device, t->image, t->memory, 0) == VK_SUCCESS;
        if (!ok) {
            if (t->image) vkDestroyImage(r->device, t->image, NULL);
            if (t->memory) vkFreeMemory(r->device, t->memory, NULL);
            memset(t, 0, sizeof(*t));
            return NULL;
        }
        t->width = src->width;
        t->height = src->height;
        t->format = fmt;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        t->stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    vk_barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vk_barrier(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy = {0};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = src->width;
    copy.extent.height = src->height;
    copy.extent.depth = 1;
    vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &copy);
    return t;
}

/* Host/GPU counters are sampled at 1 Hz only when visible, before the renderer
 * lock. Presentation timing remains independent of that sampling interval. */
#define PANEL_LINES 10
#define PANEL_COLS 64
static struct {
    bool on, init, host_init;
    uint64_t last_ns, window_ns, frames, draws_at, disp_at;
    uint64_t hardware_ns;
    double ms_sum, ms_max;
    pal_host_info host;
    pal_host_stats previous;
    char text[PANEL_LINES][PANEL_COLS + 1];
} g_panel;
static vk_buffer g_panel_buf[2];

static void panel_memory_line(char *out, const char *name, bool valid, uint64_t used, uint64_t total,
                              bool process_valid, uint64_t process) {
    const double gib = 1073741824.0;
    char app[24] = "N/A";
    if (process_valid) snprintf(app, sizeof(app), "%.2f GiB", (double)process / gib);
    if (valid)
        snprintf(out, PANEL_COLS + 1, "%s %.2f/%.2f GiB | ZERA %s", name, (double)used / gib, (double)total / gib, app);
    else
        snprintf(out, PANEL_COLS + 1, "%s N/A | ZERA %s", name, app);
}

static void panel_sample(renderer *r) {
    if (!g_panel.init) {
        g_panel.init = true;
        g_panel.on = settings_get_int("geral", "painel", 0) != 0;
        snprintf(g_panel.text[0], PANEL_COLS + 1, "ZeraPS5 %s B%d%s | FPS N/A", PS5C_VERSION_STRING, PS5C_BUILD_NUMBER,
                 PS5C_GIT_DIRTY ? "-DIRTY" : "");
    }
    if (pal_input_panel_toggled()) {
        g_panel.on = !g_panel.on;
        g_panel.hardware_ns = 0;
        g_panel.previous.valid = 0;
    }
    if (!g_panel.on) return;
    if (!g_panel.host_init) {
        pal_host_get_info(&g_panel.host);
        g_panel.host_init = true;
        snprintf(g_panel.text[2], PANEL_COLS + 1, "CPU %.*s", PANEL_COLS - 4, g_panel.host.cpu);
        snprintf(g_panel.text[4], PANEL_COLS + 1, "GPU %.*s", PANEL_COLS - 4, r->props.deviceName);
        snprintf(g_panel.text[9], PANEL_COLS + 1, "OS %.*s", PANEL_COLS - 3, g_panel.host.os);
    }
    uint64_t now = pal_time_ns();
    if (g_panel.hardware_ns && now - g_panel.hardware_ns < 1000000000ull) return;
    pal_host_stats stats;
    pal_host_get_stats(&stats);
    const pal_host_stats *prev = &g_panel.previous;
    char process_cpu[16] = "N/A", system_cpu[16] = "N/A", cores[24];
    if (g_panel.hardware_ns && (stats.valid & prev->valid & PAL_HOST_PROCESS_CPU) &&
        stats.process_cpu_ns >= prev->process_cpu_ns && now > g_panel.hardware_ns && g_panel.host.logical_processors) {
        double percent = 100.0 * (double)(stats.process_cpu_ns - prev->process_cpu_ns) /
                         (double)(now - g_panel.hardware_ns) / g_panel.host.logical_processors;
        snprintf(process_cpu, sizeof(process_cpu), "%.1f%%", fmin(percent, 100.0));
    }
    if ((stats.valid & prev->valid & PAL_HOST_SYSTEM_CPU) && stats.system_cpu_ns > prev->system_cpu_ns &&
        stats.system_idle_ns >= prev->system_idle_ns) {
        uint64_t total = stats.system_cpu_ns - prev->system_cpu_ns, idle = stats.system_idle_ns - prev->system_idle_ns;
        if (idle <= total) snprintf(system_cpu, sizeof(system_cpu), "%.1f%%", 100.0 * (double)(total - idle) / (double)total);
    }
    if (g_panel.host.physical_cores)
        snprintf(cores, sizeof(cores), "%uC/%uT", g_panel.host.physical_cores, g_panel.host.logical_processors);
    else
        snprintf(cores, sizeof(cores), "%uT", g_panel.host.logical_processors);
    snprintf(g_panel.text[3], PANEL_COLS + 1, "CPU %s | ZERA %s | %s", system_cpu, process_cpu, cores);
    panel_memory_line(g_panel.text[7], "RAM", (stats.valid & PAL_HOST_RAM) && stats.ram_available <= stats.ram_total,
                      stats.ram_available <= stats.ram_total ? stats.ram_total - stats.ram_available : 0, stats.ram_total,
                      (stats.valid & PAL_HOST_PROCESS_RAM) != 0, stats.process_ram);
    panel_memory_line(g_panel.text[8], "COMMIT", (stats.valid & PAL_HOST_COMMIT) != 0, stats.commit_total, stats.commit_limit,
                      (stats.valid & PAL_HOST_PROCESS_COMMIT) != 0, stats.process_commit);
    g_panel.previous = stats;
    g_panel.hardware_ns = now;

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                   .pNext = &budget};
    if (r->memory_budget) r->memory_properties2(r->phys, &properties);
    uint64_t total = 0, used = 0, limit = 0;
    for (uint32_t i = 0; i < r->mem_props.memoryHeapCount; i++) {
        if (!(r->mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        total += r->mem_props.memoryHeaps[i].size;
        used += budget.heapUsage[i];
        limit += budget.heapBudget[i];
    }
    const double gib = 1073741824.0;
    const char *kind = r->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "GPU RAM (UMA)" : "VRAM";
    char usage[24] = "N/A", capacity[24] = "N/A", available[24] = "N/A";
    if (total) snprintf(capacity, sizeof(capacity), "%.2f GiB", (double)total / gib);
    /* heapUsage is the driver's estimate for this process, not whole-PC VRAM
     * use. Integrated GPU heaps share RAM: do not advertise dedicated VRAM. */
    if (r->memory_budget && limit) {
        snprintf(usage, sizeof(usage), "%.2f GiB EST", (double)used / gib);
        snprintf(available, sizeof(available), "%.2f GiB", (double)limit / gib);
    }
    snprintf(g_panel.text[5], PANEL_COLS + 1, "%s ZERA %s | TOTAL %s", kind, usage, capacity);
    snprintf(g_panel.text[6], PANEL_COLS + 1, "BUDGET %s | VK SUP %u.%u.%u | OUT %ux%u", available,
             VK_VERSION_MAJOR(r->props.apiVersion), VK_VERSION_MINOR(r->props.apiVersion),
             VK_VERSION_PATCH(r->props.apiVersion), r->sc_extent.width, r->sc_extent.height);
}

static void panel_tick(renderer *r) {
    uint64_t now = pal_time_ns();
    if (!g_panel.last_ns) vk_pipeline_counts(r, &g_panel.draws_at, &g_panel.disp_at);
    if (g_panel.last_ns) {
        double ms = (double)(now - g_panel.last_ns) / 1e6;
        g_panel.ms_sum += ms;
        if (ms > g_panel.ms_max) g_panel.ms_max = ms;
        g_panel.frames++;
    }
    g_panel.last_ns = now;
    if (!g_panel.window_ns) g_panel.window_ns = now;
    if (now - g_panel.window_ns >= 500000000ull && g_panel.frames) {
        uint64_t draws, disp;
        vk_pipeline_counts(r, &draws, &disp);
        double secs = (double)(now - g_panel.window_ns) / 1e9;
        snprintf(g_panel.text[0], PANEL_COLS + 1, "ZeraPS5 %s B%d%s | %.0f FPS %.1f MS MAX %.1f", PS5C_VERSION_STRING,
                 PS5C_BUILD_NUMBER, PS5C_GIT_DIRTY ? "-DIRTY" : "", (double)g_panel.frames / secs,
                 g_panel.ms_sum / (double)g_panel.frames, g_panel.ms_max);
        snprintf(g_panel.text[1], PANEL_COLS + 1, "%llu DRAWS  %llu DISPATCHES /FRAME",
                 (unsigned long long)((draws - g_panel.draws_at) / g_panel.frames),
                 (unsigned long long)((disp - g_panel.disp_at) / g_panel.frames));
        g_panel.draws_at = draws;
        g_panel.disp_at = disp;
        g_panel.frames = 0;
        g_panel.ms_sum = g_panel.ms_max = 0;
        g_panel.window_ns = now;
    }
}

/* Notifications (trophies): one line in the top-right corner until it expires */
#define TOAST_COLS 30
static struct {
    char text[TOAST_COLS + 1];
    uint64_t until_ns;
} g_toast;
static vk_buffer g_toast_buf[2];

void renderer_notify(renderer *r, const char *text, uint32_t ms) {
    pal_mutex_lock(r->lock);
    snprintf(g_toast.text, sizeof(g_toast.text), "%s", text);
    g_toast.until_ns = pal_time_ns() + (uint64_t)ms * 1000000u;
    pal_mutex_unlock(r->lock);
}

/* renderer lock held */
static void toast_draw(renderer *r, VkCommandBuffer cmd, VkImage sc, uint32_t frame) {
    if (!g_toast.until_ns || pal_time_ns() >= g_toast.until_ns) return;
    uint32_t w, h;
    vk_overlay_size(TOAST_COLS, 1, &w, &h);
    if (w + 16 > r->sc_extent.width || h + 16 > r->sc_extent.height) return;
    vk_buffer *b = &g_toast_buf[frame & 1];
    if (!b->buffer && !vk_buffer_create(r, (uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, b)) return;
    const char *line = g_toast.text;
    bool bgra = r->sc_format == VK_FORMAT_B8G8R8A8_UNORM || r->sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    vk_overlay_render((uint8_t *)b->mapped, w, h, &line, 1, bgra);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = (int32_t)(r->sc_extent.width - w - 8);
    copy.imageOffset.y = 8;
    copy.imageExtent.width = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, b->buffer, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

/* Shader compilation in progress: one line in the bottom-left corner while the
 * earlier sessions' pipelines are built (usually over the splash screen), or
 * once draws have waited for theirs for a second (their draws are skipped).
 * PS5C_NO_SHADER_PROGRESS=1 hides it. */
#define STATUS_COLS 34
static vk_buffer g_status_buf[2];

static void status_draw(renderer *r, VkCommandBuffer cmd, VkImage sc, uint32_t frame) {
    static int off = -1;
    static uint64_t draws_since;
    if (off < 0) off = getenv("PS5C_NO_SHADER_PROGRESS") != NULL;
    uint32_t done, total, draws;
    if (off || !vk_pipeline_async_progress(&done, &total, &draws)) {
        draws_since = 0;
        return;
    }
    char text[64];
    if (done < total) {
        snprintf(text, sizeof(text), "COMPILANDO SHADERS %u/%u", done, total);
    } else {
        uint64_t now = pal_time_ns();
        if (!draws_since) draws_since = now;
        if (now - draws_since < 1000000000ull) return;
        snprintf(text, sizeof(text), "COMPILANDO SHADERS (%u)", draws);
    }
    uint32_t w, h;
    vk_overlay_size(STATUS_COLS, 1, &w, &h);
    if (w + 16 > r->sc_extent.width || h + 16 > r->sc_extent.height) return;
    vk_buffer *b = &g_status_buf[frame & 1];
    if (!b->buffer && !vk_buffer_create(r, (uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, b)) return;
    const char *line = text;
    bool bgra = r->sc_format == VK_FORMAT_B8G8R8A8_UNORM || r->sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    vk_overlay_render((uint8_t *)b->mapped, w, h, &line, 1, bgra);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = 8;
    copy.imageOffset.y = (int32_t)(r->sc_extent.height - h - 8);
    copy.imageExtent.width = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, b->buffer, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

/* Copies the panel onto the swapchain image (in TRANSFER_DST layout) */
static void panel_draw(renderer *r, VkCommandBuffer cmd, VkImage sc, uint32_t frame) {
    if (!g_panel.on) return;
    uint32_t w, h, scale = 3;
    vk_overlay_size_scaled(PANEL_COLS, PANEL_LINES, scale, &w, &h);
    if (w + 16 > r->sc_extent.width) scale = 2;
    if (r->sc_extent.width <= 16 + 2 * VK_OVERLAY_PAD || r->sc_extent.height <= 16 + 2 * VK_OVERLAY_PAD) return;
    uint32_t cols = (r->sc_extent.width - 16 - 2 * VK_OVERLAY_PAD) / (3 * scale);
    uint32_t count = (r->sc_extent.height - 16 - 2 * VK_OVERLAY_PAD) / ((9 * scale + 1) / 2);
    if (cols > PANEL_COLS) cols = PANEL_COLS;
    if (count > PANEL_LINES) count = PANEL_LINES;
    if (!cols || !count) return;
    vk_overlay_size_scaled(cols, count, scale, &w, &h);
    vk_buffer *b = &g_panel_buf[frame & 1];
    if (b->buffer && b->size < (uint64_t)w * h * 4) vk_buffer_destroy(r, b);
    if (!b->buffer && !vk_buffer_create(r, (uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, b)) return;
    const char *lines[PANEL_LINES];
    for (uint32_t i = 0; i < count; i++) lines[i] = g_panel.text[i];
    bool bgra = r->sc_format == VK_FORMAT_B8G8R8A8_UNORM || r->sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    vk_overlay_render_scaled((uint8_t *)b->mapped, w, h, lines, count, bgra, scale);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = 8;
    copy.imageOffset.y = 8;
    copy.imageExtent.width = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth = 1;
    if (w + 8 > r->sc_extent.width || h + 8 > r->sc_extent.height) copy.imageOffset.x = copy.imageOffset.y = 0;
    vkCmdCopyBufferToImage(cmd, b->buffer, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

/* The surface the GPU wrote last at the display buffer: its size, else (no such image) one a few rows
 * taller, as a PS4 color target's tile-padded height (1080 rows in a 1088-row target). */
static vk_image *displayed_image(renderer *r, const renderer_surface *s) {
    vk_image *last = NULL;
    for (int pass = 0; pass < 2 && !last; pass++) {
        for (uint32_t i = 0; i < r->image_count; i++) {
            vk_image *img = &r->images[i];
            bool size = pass ? img->height > s->height && img->height <= s->height + 64 : img->height == s->height;
            if (img->addr != s->addr || img->width != s->width || !size || !img->gpu_written) continue;
            if (!last || img->written_seq > last->written_seq) last = img; /* the most recent frame */
        }
    }
    return last;
}

void renderer_present(renderer *r, const renderer_surface *s) {
    panel_sample(r); /* OS/driver queries never hold up the command processor lock. */
    vk_shader_cache_tick(); /* not under the renderer lock: a save may take a while */
    vk_pipeline_async_frame();
    /* The present frame's fence is only this thread's: wait for it before taking
     * the lock the command processor needs for every draw. */
    vk_present_frame *f = &r->frames[r->frame_index];
    vk_check_work(r, vkWaitForFences(r->device, 1, &f->fence, VK_TRUE, UINT64_MAX), "vkWaitForFences (present)");
    pal_mutex_lock(r->lock);
    /* the frame's work goes first in the queue. Its results already reached
     * guest memory: the command processor completes the writebacks before a
     * flip packet (and a CPU flip comes after the game waited for its labels);
     * waiting here again only stalled the next frame's work behind this lock. */
    vk_flush(r);
    { /* debugging captures, also while the window is minimized */
        vk_image *last = s ? displayed_image(r, s) : NULL;
        static uint32_t presents;
        save_images(r, ++presents, last);
        uint32_t n = capture_interval();
        if (!last && s && n && presents % n == 0) {
            /* a frame from guest memory: how much of it is not zero (debugging aid) */
            uint64_t nz = 0, total = 0;
            for (uint64_t i = 0; i < (uint64_t)s->pitch * s->height; i += 97) {
                uint32_t pixel;
                if (!gmem_read(r->mem, s->addr + i * 4, &pixel, sizeof(pixel))) break;
                nz += pixel != 0;
                total++;
            }
            LOG_I(GPU, "present %u from guest memory 0x%llx: %llu of %llu samples not zero, format 0x%llx", presents,
                  (unsigned long long)s->addr, (unsigned long long)nz, (unsigned long long)total,
                  (unsigned long long)s->pixel_format);
        }
    }
    if (!r->swapchain && !create_swapchain(r)) {
        pal_mutex_unlock(r->lock);
        return;
    }
    uint32_t sc_index = 0;
    VkResult res = vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &sc_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_check_work(r, vkDeviceWaitIdle(r->device), "vkDeviceWaitIdle (recreate)");
        create_swapchain(r);
        pal_mutex_unlock(r->lock);
        return;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        vk_check_work(r, res, "vkAcquireNextImageKHR");
    }
    vk_check_work(r, vkResetFences(r->device, 1, &f->fence), "vkResetFences (present)");
    vk_check_work(r, vkResetCommandBuffer(f->cmd, 0), "vkResetCommandBuffer (present)");
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check_work(r, vkBeginCommandBuffer(f->cmd, &bi), "vkBeginCommandBuffer (present)");

    /* the frame: the surface the GPU wrote last at this address (in whatever
     * color format it was drawn, the blit converts), else the guest memory */
    vk_image *src = NULL;
    if (s) {
        VkFormat want = surface_format(s->pixel_format);
        src = displayed_image(r, s);
        bool rendered = src != NULL;
        if (!src) src = vk_image_get(r, s->addr, s->width, s->height, want);
        static int logged = -1;
        if (logged < 0) logged = getenv("PS5C_LOG_PRESENT") ? 0 : 1000;
        if (logged < 600) {
            logged++;
            LOG_I(GPU, "present 0x%llx %ux%u: %s (format %d)", (unsigned long long)s->addr, s->width, s->height,
                  rendered ? "GPU image" : "guest memory", src ? (int)src->format : -1);
        }
    }
    if (src && !src->gpu_written) vk_upload_linear(r, f->cmd, &f->staging, src, s->addr, s->pitch * 4);
    renderer_surface splash_surface;
    if (r->picture_on) { /* the intro covers everything */
        if (r->picture_dirty) {
            vk_barrier(f->cmd, &r->picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {0};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent.width = r->picture.width;
            copy.imageExtent.height = r->picture.height;
            copy.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(f->cmd, r->picture_staging.buffer, r->picture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy);
            r->picture_dirty = false;
        }
        src = &r->picture;
        memset(&splash_surface, 0, sizeof(splash_surface));
        splash_surface.width = r->picture.width;
        splash_surface.height = r->picture.height;
        s = &splash_surface;
    } else if (r->splash_on) { /* the system splash screen covers the game's frames */
        if (!r->splash_uploaded) {
            vk_barrier(f->cmd, &r->splash, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy = {0};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent.width = r->splash.width;
            copy.imageExtent.height = r->splash.height;
            copy.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(f->cmd, r->splash_staging.buffer, r->splash.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy);
            r->splash_uploaded = true;
        }
        src = &r->splash;
        memset(&splash_surface, 0, sizeof(splash_surface));
        splash_surface.width = r->splash.width;
        splash_surface.height = r->splash.height;
        s = &splash_surface;
    }

    /* swapchain image: clear to black, then blit the frame letterboxed */
    vk_image sc = {0};
    sc.image = r->sc_images[sc_index];
    sc.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    /* The acquire semaphore is waited at TRANSFER. The source scope of the
     * first layout transition must intersect that wait before touching the
     * acquired image, even when discarding its old contents. */
    sc.stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    vk_barrier(f->cmd, &sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(f->cmd, sc.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (src && present_unorm_of(src->format) != VK_FORMAT_UNDEFINED) src = present_as_unorm(r, f->cmd, src);
    if (src) {
        vk_barrier(f->cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
                             0, NULL);
        double sx = (double)r->sc_extent.width / s->width, sy = (double)r->sc_extent.height / s->height;
        double scale = sx < sy ? sx : sy;
        int32_t w = (int32_t)(s->width * scale), h = (int32_t)(s->height * scale);
        int32_t x = ((int32_t)r->sc_extent.width - w) / 2, y = ((int32_t)r->sc_extent.height - h) / 2;
        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)s->width;
        blit.srcOffsets[1].y = (int32_t)s->height;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0].x = x;
        blit.dstOffsets[0].y = y;
        blit.dstOffsets[1].x = x + w;
        blit.dstOffsets[1].y = y + h;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(f->cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    panel_tick(r);
    panel_draw(r, f->cmd, sc.image, r->frame_index);
    toast_draw(r, f->cmd, sc.image, r->frame_index);
    status_draw(r, f->cmd, sc.image, r->frame_index);
    vk_barrier(f->cmd, &sc, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vk_check_work(r, vkEndCommandBuffer(f->cmd), "vkEndCommandBuffer (present)");

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f->acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &r->sc_rendered[sc_index];
    VkResult submitted = vkQueueSubmit(r->queue, 1, &si, f->fence);
    vk_check_work(r, submitted, "vkQueueSubmit (present)");

    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &r->sc_rendered[sc_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &r->swapchain;
    pi.pImageIndices = &sc_index;
    res = vkQueuePresentKHR(r->queue, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        vk_check_work(r, vkDeviceWaitIdle(r->device), "vkDeviceWaitIdle (recreate)");
        create_swapchain(r);
    } else vk_check_work(r, res, "vkQueuePresentKHR");
    r->frame_index = (r->frame_index + 1) % FRAMES_IN_FLIGHT;
    pal_mutex_unlock(r->lock);
}

/* ---- Lifetime ---- */

static bool create_frames(renderer *r) {
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = r->queue_family;
    if (!check(r, vkCreateCommandPool(r->device, &pci, NULL, &r->pool), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = r->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vk_present_frame *f = &r->frames[i];
        if (vkAllocateCommandBuffers(r->device, &ai, &f->cmd) != VK_SUCCESS ||
            vkCreateFence(r->device, &fci, NULL, &f->fence) != VK_SUCCESS ||
            vkCreateSemaphore(r->device, &sci, NULL, &f->acquired) != VK_SUCCESS) {
            return false;
        }
    }
    for(uint32_t i=0;i<2;i++) {
        if(vkAllocateCommandBuffers(r->device,&ai,&r->work_cmds[i])!=VK_SUCCESS ||
           vkCreateFence(r->device,&fci,NULL,&r->work_fences[i])!=VK_SUCCESS)return false;
    }
    r->cmd=r->work_cmds[0];r->work_fence=r->work_fences[0];
    return true;
}

/* The window goes to a display the chosen GPU drives (the log lists them) */
static bool place_window(renderer *r) {
    char list[2048];
    if (pal_display_list(list, sizeof(list))) {
        for (char *line = list, *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            if (!end) break;
            *end = 0;
            LOG_I(GPU, "display %s", line);
        }
    }
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    vkGetPhysicalDeviceProperties2(r->phys, &p2);
    if (!id.deviceLUIDValid) return true;
    int moved = pal_window_move_to_gpu(r->window, id.deviceLUID);
    if (moved > 0) LOG_I(GPU, "window moved to a display %s drives", p2.properties.deviceName);
    else if (moved < 0) LOG_W(GPU, "%s drives no display: presenting goes through another GPU (slower)", p2.properties.deviceName);
    return true;
}

renderer *renderer_create(pal_window *window, gmem *mem) {
    if (getenv("PS5C_NO_VULKAN") || !window || !vk_api_load()) return NULL;
    if (vk_pipeline_async_on()) {
        LOG_E(GPU, "renderer unavailable: previous pipeline workers still own the device; restart the process");
        return NULL;
    }
    renderer *r = (renderer *)calloc(1, sizeof(renderer));
    if (!r) return NULL;
    char title[64]={0};
    r->ngg_culling_profile=settings_param_string("titleId",title,sizeof(title)) && !strcmp(title,"PPSA99319");
    if(r->ngg_culling_profile)LOG_I(GPU,"Quake III homebrew: RADV NGG culling profile enabled");
    memset(&g_panel, 0, sizeof(g_panel));
    const char *lod=getenv("PS5C_LOD_STATS_BINDINGS");
    r->lod_stats_enabled=lod && *lod && strcmp(lod,"0");
    if(r->lod_stats_enabled)LOG_I(GPU,"LOD statistics: conservative binding feedback (experimental, actual sampled LOD unavailable)");
    r->cpu_prepare_setting=getenv("PS5C_CPU_PREPARE_CACHE");
    vk_cpu_prepare(r,0);
    r->image_limit = 512; /* raised below on a GPU with its own large memory */
    const char *limit = getenv("PS5C_IMAGE_CACHE_LIMIT");
    if (limit) {
        unsigned long value = strtoul(limit, NULL, 10);
        if (value >= 64 && value <= VK_MAX_IMAGES) r->image_limit = (uint32_t)value;
        LOG_I(GPU, "image cache: %u active slots (PS5C_IMAGE_CACHE_LIMIT)", r->image_limit);
    }
    const char *backing = getenv("PS5C_GPU_IMAGE_BACKING");
    if (backing && atoi(backing) != 0) {
        uint64_t mb = 1024;
        const char *budget = getenv("PS5C_GPU_IMAGE_BACKING_MB");
        if (budget) {
            unsigned long long value = strtoull(budget, NULL, 10);
            if (value >= 1 && value <= 8192) mb = value;
        }
        r->image_backing_limit = mb * 1024 * 1024;
        LOG_I(GPU, "GPU image backing: RAM budget %llu MiB (experimental)", (unsigned long long)mb);
    }
    r->window = window;
    r->mem = mem;
    uint64_t t0 = pal_time_ns();
    bool ok = pal_mutex_create(&r->lock) == PS5C_OK && create_instance(r) && vk_surface_create(r) && pick_device(r) &&
              place_window(r) && create_device(r) && create_frames(r) && vk_pipeline_init(r);
    if (ok && !limit) {
        /* No Man's Sky keeps more than 512 surfaces alive: each new one evicted an
         * older one (with a queue wait), its 4K targets every frame, and they came
         * back from guest memory (0.38 -> 0.79 frames/s on the RTX with 2048).
         * A GPU with 8 GiB of its own memory or more takes every slot. */
        VkDeviceSize local = 0;
        for (uint32_t i = 0; i < r->mem_props.memoryHeapCount; i++)
            if ((r->mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && r->mem_props.memoryHeaps[i].size > local)
                local = r->mem_props.memoryHeaps[i].size;
        if (local >= (8ull << 30) && r->props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) r->image_limit = VK_MAX_IMAGES;
        LOG_I(GPU, "image cache: %u active slots (%.1f GiB of device memory)", r->image_limit, local / 1073741824.0);
        /* An integrated GPU's heap is system RAM the guest's own memory also takes:
         * images keep to half of it. PS5C_IMAGE_MEMORY_MB=<n> sets the budget, 0 none. */
        if (r->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) r->image_bytes_limit = local / 2;
    }
    const char *image_mb = getenv("PS5C_IMAGE_MEMORY_MB");
    if (ok && image_mb) r->image_bytes_limit = strtoull(image_mb, NULL, 10) << 20;
    if (ok && r->image_bytes_limit)
        LOG_I(GPU, "image cache: %.1f GiB memory budget", r->image_bytes_limit / 1073741824.0);
    if (ok) {
        vk_shader_cache_open(r);
        vk_pipeline_async_start(r);
    }
    uint64_t t1 = pal_time_ns();
    if (!ok) {
        LOG_W(GPU, "Vulkan renderer unavailable: using the software presenter");
        renderer_destroy(r);
        return NULL;
    }
    create_swapchain(r); /* may fail while minimized: retried at present */
    /* a slow swapchain: the window on a display another GPU drives (testers' reports) */
    LOG_I(GPU, "renderer ready: device %.0f ms, swapchain %.0f ms", (double)(t1 - t0) / 1e6, (double)(pal_time_ns() - t1) / 1e6);
    pal_window_set_external_present(window, true);
    return r;
}

void renderer_destroy(renderer *r) {
    if (!r) return;
    if (r->device) {
        if (!vk_pipeline_async_stop(r)) {
            /* Imports borrow guest pages even though the compiler only owns
             * copied SPIR-V/recipes. Retire those imports before the caller
             * tears down guest memory; retain the device and compiler state. */
            renderer_wait_idle(r);
            /* A CPU driver compiler can outlive the bounded stop. Device-idle
             * does not join it. Retain its renderer and cache until process
             * exit rather than freeing handles/state still used by the worker. */
            LOG_W(GPU, "renderer retained until process exit: pipeline compiler still running");
            return;
        }
        vk_check_work(r, vkDeviceWaitIdle(r->device), "vkDeviceWaitIdle (transfer)");
        discard_captures(r);
        /* Retired views belong to this device; do not carry them into the
         * next renderer after a format promotion in the final batch. */
        free_retired(r);
        vk_pipeline_shutdown(r);
        vk_shader_cache_close(r);
        for (uint32_t i = 0; i < r->image_count; i++) {
            free_feedback(r, &r->images[i]);
            vkDestroyImageView(r->device, r->images[i].view, NULL);
            vkDestroyImageView(r->device, r->images[i].array_view, NULL);
            if (r->images[i].ds_view) vkDestroyImageView(r->device, r->images[i].ds_view, NULL);
            if (r->images[i].stencil_view) vkDestroyImageView(r->device, r->images[i].stencil_view, NULL);
            for (uint32_t k = 0; k < r->images[i].sub_count; k++) vkDestroyImageView(r->device, r->images[i].sub[k].view, NULL);
            free(r->images[i].sub);
            vkDestroyImage(r->device, r->images[i].image, NULL);
            vkFreeMemory(r->device, r->images[i].memory, NULL);
        }
        if (r->splash.image) vkDestroyImage(r->device, r->splash.image, NULL);
        if (r->splash.memory) vkFreeMemory(r->device, r->splash.memory, NULL);
        if (r->present_tmp.image) vkDestroyImage(r->device, r->present_tmp.image, NULL);
        if (r->present_tmp.memory) vkFreeMemory(r->device, r->present_tmp.memory, NULL);
        vk_buffer_destroy(r, &r->splash_staging);
        if (r->picture.image) vkDestroyImage(r->device, r->picture.image, NULL);
        if (r->picture.memory) vkFreeMemory(r->device, r->picture.memory, NULL);
        vk_buffer_destroy(r, &r->picture_staging);
        for (int i = 0; i < 2; i++) vk_buffer_destroy(r, &g_panel_buf[i]), vk_buffer_destroy(r, &g_toast_buf[i]), vk_buffer_destroy(r, &g_status_buf[i]);
        for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
            vk_buffer_destroy(r, &r->frames[i].staging);
            if (r->frames[i].fence) vkDestroyFence(r->device, r->frames[i].fence, NULL);
            if (r->frames[i].acquired) vkDestroySemaphore(r->device, r->frames[i].acquired, NULL);
        }
        for(uint32_t i=0;i<2;i++)
            if(r->work_fences[i])vkDestroyFence(r->device,r->work_fences[i],NULL);
            if(r->work_timeline)vkDestroySemaphore(r->device,r->work_timeline,NULL);
        if (r->pool) vkDestroyCommandPool(r->device, r->pool, NULL);
        destroy_swapchain(r);
        vkDestroyDevice(r->device, NULL);
    }
    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->instance) vkDestroyInstance(r->instance, NULL);
    if (r->lock) pal_mutex_destroy(r->lock);
    if (r->window) pal_window_set_external_present(r->window, false);
    while (r->image_backing) {
        vk_image_backing *b = r->image_backing;
        r->image_backing = b->next;
        free(b);
    }
    free(r->checkpoints);
    free(r);
}

/* CPU observations are recorded in command order under the renderer lock.
 * No GPU sample counts are claimed; this conservative mode stays opt-in. */
bool renderer_lod_stats(renderer *r,void *out,uint32_t bytes,uint32_t flags) {
    if(!r || !r->lod_stats_enabled)return false;
    pal_mutex_lock(r->lock);
    static unsigned logged;
    if(bytes && logged++<16) {
        unsigned used=0;for(unsigned k=0;k<4;k++)for(uint64_t b=r->lod_stats.bound[k];b;b&=b-1)used++;
        LOG_I(GPU,"LOD binding report: %u bytes, %u counters used, flags %08x (conservative)",bytes,used,flags);
    }
    bool ok=gpu_lod_report(&r->lod_stats,out,bytes,flags);
    pal_mutex_unlock(r->lock);
    return ok;
}
