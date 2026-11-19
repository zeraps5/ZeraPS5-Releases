/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/vk/vk_api.h"

#include "base/log.h"
#include "pal/pal.h"

#define VK_DEFINE(name) PFN_##name name;
VK_GLOBAL_FUNCS(VK_DEFINE)
VK_INSTANCE_FUNCS(VK_DEFINE)
VK_DEVICE_FUNCS(VK_DEFINE)
#undef VK_DEFINE

static PFN_vkGetInstanceProcAddr g_get_instance_proc;

bool vk_api_load(void) {
    if (g_get_instance_proc) return true;
#ifdef _WIN32
    const char *lib_name = "vulkan-1.dll";
#else
    const char *lib_name = "libvulkan.so.1";
#endif
    pal_library *lib = pal_library_open(lib_name);
    if (!lib) {
        LOG_W(GPU, "Vulkan loader %s not found", lib_name);
        return false;
    }
    g_get_instance_proc = (PFN_vkGetInstanceProcAddr)pal_library_symbol(lib, "vkGetInstanceProcAddr");
    if (!g_get_instance_proc) return false;
    bool ok = true;
#define VK_LOAD(name)                                                                                                  \
    name = (PFN_##name)g_get_instance_proc(NULL, #name);                                                               \
    if (!name) ok = false;
    VK_GLOBAL_FUNCS(VK_LOAD)
#undef VK_LOAD
    return ok;
}

uint32_t vk_api_instance_version(void) {
    PFN_vkEnumerateInstanceVersion get =
        g_get_instance_proc ? (PFN_vkEnumerateInstanceVersion)g_get_instance_proc(NULL, "vkEnumerateInstanceVersion") : NULL;
    uint32_t v = VK_API_VERSION_1_0;
    if (!get || get(&v) != VK_SUCCESS) return VK_API_VERSION_1_0;
    return v;
}

PFN_vkVoidFunction vk_api_instance_proc(VkInstance instance, const char *name) {
    return g_get_instance_proc ? g_get_instance_proc(instance, name) : NULL;
}

bool vk_api_load_instance(VkInstance instance) {
    bool ok = true;
#define VK_LOAD(name)                                                                                                  \
    name = (PFN_##name)g_get_instance_proc(instance, #name);                                                           \
    if (!name) {                                                                                                       \
        LOG_E(GPU, "Vulkan: %s missing", #name);                                                                     \
        ok = false;                                                                                                    \
    }
    VK_INSTANCE_FUNCS(VK_LOAD)
#undef VK_LOAD
    return ok;
}

bool vk_api_load_device(VkDevice device) {
    bool ok = true;
#define VK_LOAD(name)                                                                                                  \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name);                                                             \
    if (!name) {                                                                                                       \
        LOG_E(GPU, "Vulkan: %s missing", #name);                                                                     \
        ok = false;                                                                                                    \
    }
    VK_DEVICE_FUNCS(VK_LOAD)
#undef VK_LOAD
    return ok;
}
