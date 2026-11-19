/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Window surface creation, the only platform-specific part of the renderer. */
#include "base/log.h"
#include "gpu/vk/vk_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_win32.h>

const char *vk_surface_extension_name(void) { return "VK_KHR_win32_surface"; }

bool vk_surface_create(renderer *r) {
    PFN_vkCreateWin32SurfaceKHR create =
        (PFN_vkCreateWin32SurfaceKHR)vk_api_instance_proc(r->instance, "vkCreateWin32SurfaceKHR");
    void *inst = NULL, *hwnd = NULL;
    pal_window_native(r->window, &inst, &hwnd);
    if (!create || !hwnd) return false;
    VkWin32SurfaceCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = (HINSTANCE)inst;
    ci.hwnd = (HWND)hwnd;
    if (create(r->instance, &ci, NULL, &r->surface) != VK_SUCCESS) {
        LOG_E(GPU, "Vulkan: cannot create the window surface");
        return false;
    }
    return true;
}
#else
const char *vk_surface_extension_name(void) { return "VK_KHR_xlib_surface"; }

bool vk_surface_create(renderer *r) {
    PS5C_UNUSED(r);
    return false; /* no window system integration outside Windows yet */
}
#endif
