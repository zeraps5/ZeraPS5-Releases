/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone capability/correctness probe. Uses scratch memory only.
 * Windows (MSYS2 UCRT64, from the repository): gcc -std=c11 -Wall -Wextra
 *   -Werror -Ithird_party/vulkan/include tools/vulkan_host_import_probe.c
 *   C:/Windows/System32/vulkan-1.dll -o build/host_import_probe.exe
 * Linux: cc -std=c11 -Wall -Wextra -Werror tools/vulkan_host_import_probe.c
 *   -lvulkan -o build/host_import_probe
 * This tests transfer commands, not shader BDA, guest aliases or performance. */
#define _POSIX_C_SOURCE 200112L
#ifdef _WIN32
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t memory_type(VkPhysicalDevice p, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(p, &props);
    const VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & needed) == needed) return i;
    return UINT32_MAX;
}

static int test_import(VkPhysicalDevice physical, uint32_t family, VkDeviceSize alignment, int shared) {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer imported = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory imported_memory = VK_NULL_HANDLE, readback_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *host = NULL, *mapped = NULL;
#ifdef _WIN32
    HANDLE mapping = NULL;
#endif
    VkResult result = VK_SUCCESS;
    int status = 1;
    const char *step = "vkCreateDevice";
#define TRY(call) do { step = #call; result = (call); if (result != VK_SUCCESS) goto done; } while (0)
#define REQUIRE(cond, what) do { if (!(cond)) { step = what; result = VK_ERROR_UNKNOWN; goto done; } } while (0)
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *extension = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    TRY(vkCreateDevice(physical, &di, NULL, &device));
    PFN_vkGetMemoryHostPointerPropertiesEXT host_properties =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
    REQUIRE(host_properties, "host pointer entry point missing");
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkDeviceSize bytes = 65536;
    REQUIRE(alignment && !(alignment & (alignment - 1)) && alignment <= 16777216,
            "unsupported import alignment");
    bytes = (bytes + alignment - 1) & ~(alignment - 1);
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device, &bi, NULL, &imported));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, imported, &req);
    VkDeviceSize allocation_bytes = (req.size + alignment - 1) & ~(alignment - 1);
#ifdef _WIN32
    if (shared) {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            (DWORD)(allocation_bytes >> 32), (DWORD)allocation_bytes, NULL);
        if (mapping) host = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)allocation_bytes);
    } else host = VirtualAlloc(NULL, (SIZE_T)allocation_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    (void)shared;
    if (posix_memalign(&host, (size_t)(alignment < sizeof(void *) ? sizeof(void *) : alignment),
                       (size_t)allocation_bytes) != 0) host = NULL;
#endif
    REQUIRE(host && ((uintptr_t)host % alignment) == 0, "scratch allocation/alignment");
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    TRY(host_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp));
    uint32_t type = memory_type(physical, req.memoryTypeBits & hp.memoryTypeBits);
    REQUIRE(type != UINT32_MAX, "no compatible host coherent memory type");
    VkImportMemoryHostPointerInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = allocation_bytes, .memoryTypeIndex = type};
    TRY(vkAllocateMemory(device, &ai, NULL, &imported_memory));
    TRY(vkBindBufferMemory(device, imported, imported_memory, 0));
    bi.pNext = NULL;
    TRY(vkCreateBuffer(device, &bi, NULL, &readback));
    vkGetBufferMemoryRequirements(device, readback, &req);
    type = memory_type(physical, req.memoryTypeBits);
    REQUIRE(type != UINT32_MAX, "readback memory type");
    ai.pNext = NULL; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    TRY(vkAllocateMemory(device, &ai, NULL, &readback_memory));
    TRY(vkBindBufferMemory(device, readback, readback_memory, 0));
    TRY(vkMapMemory(device, readback_memory, 0, bytes, 0, &mapped));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    TRY(vkCreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    TRY(vkAllocateCommandBuffers(device, &ca, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device, &fi, NULL, &fence));
    uint32_t *input = host;
    size_t words = (size_t)(bytes / 4);
    for (size_t i = 0; i < words; i++) input[i] = 0xa5a50000u ^ (uint32_t)(i * 17);
    memset(mapped, 0, (size_t)bytes);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    TRY(vkBeginCommandBuffer(command, &begin));
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    VkBufferCopy copy = {.size = bytes};
    vkCmdCopyBuffer(command, imported, readback, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    vkCmdFillBuffer(command, imported, 0, bytes, 0x13579bdfu);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, NULL, 0, NULL);
    TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    TRY(vkQueueSubmit(queue, 1, &submit, fence));
    TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
    for (size_t i = 0; i < words; i++) {
        REQUIRE(((uint32_t *)mapped)[i] == (0xa5a50000u ^ (uint32_t)(i * 17)), "GPU read mismatched CPU seed");
        REQUIRE(input[i] == 0x13579bdfu, "CPU read mismatched GPU fill");
    }
    printf("  %s: PASS, %llu bytes, CPU -> GPU copy and GPU -> CPU fill verified\n",
        shared ? "shared mapping" : "private allocation", (unsigned long long)bytes);
    status = 0;
done:
    if (status) printf("  %s: FAIL at %s (VkResult %d)\n",
        shared ? "shared mapping" : "private allocation", step, (int)result);
    if (device) vkDeviceWaitIdle(device);
    if (mapped) vkUnmapMemory(device, readback_memory);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (readback) vkDestroyBuffer(device, readback, NULL);
    if (imported) vkDestroyBuffer(device, imported, NULL);
    if (readback_memory) vkFreeMemory(device, readback_memory, NULL);
    if (imported_memory) vkFreeMemory(device, imported_memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
#ifdef _WIN32
    if (host && shared) UnmapViewOfFile(host);
    if (host && !shared) VirtualFree(host, 0, MEM_RELEASE);
    if (mapping) CloseHandle(mapping);
#else
    free(host);
#endif
    return status;
#undef REQUIRE
#undef TRY
}

int main(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    if (vkCreateInstance(&ci, NULL, &instance) != VK_SUCCESS) return 2;
    uint32_t count = 0;
    int failures = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) { vkDestroyInstance(instance, NULL); return 2; }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS) {
        free(devices); vkDestroyInstance(instance, NULL); return 2;
    }
    for (uint32_t d = 0; d < count; d++) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host};
        vkGetPhysicalDeviceProperties2(devices[d], &props);
        printf("%s (driver 0x%x)\n", props.properties.deviceName, props.properties.driverVersion);
        uint32_t n = 0;
        if (vkEnumerateDeviceExtensionProperties(devices[d], NULL, &n, NULL) != VK_SUCCESS) { failures++; continue; }
        VkExtensionProperties *extensions = calloc(n ? n : 1, sizeof(*extensions));
        if (!extensions || vkEnumerateDeviceExtensionProperties(devices[d], NULL, &n, extensions) != VK_SUCCESS) { free(extensions); failures++; continue; }
        int supported = 0;
        for (uint32_t i = 0; i < n; i++) supported |= strcmp(extensions[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0;
        free(extensions);
        if (!supported) { puts("  VK_EXT_external_memory_host: unsupported (copy fallback required)"); continue; }
        printf("  VK_EXT_external_memory_host: supported, alignment %llu\n", (unsigned long long)host.minImportedHostPointerAlignment);
        VkPhysicalDeviceExternalBufferInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
        VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(devices[d], &info, &external);
        if (!(external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            puts("  host allocation transfer buffer: not importable"); continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &n, NULL);
        VkQueueFamilyProperties *families = calloc(n ? n : 1, sizeof(*families));
        if (!families) { failures++; continue; }
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &n, families);
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < n; i++) if (families[i].queueCount && (families[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) { family = i; break; }
        free(families);
        if (family == UINT32_MAX) { failures++; continue; }
        failures += test_import(devices[d], family, host.minImportedHostPointerAlignment, 0);
#ifdef _WIN32
        failures += test_import(devices[d], family, host.minImportedHostPointerAlignment, 1);
#endif
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    return failures ? 1 : 0;
}
