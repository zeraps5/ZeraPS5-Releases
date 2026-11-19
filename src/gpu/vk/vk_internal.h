/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Vulkan renderer internals shared by its source files. */
#ifndef PS5C_VK_INTERNAL_H
#define PS5C_VK_INTERNAL_H

#include "gpu/renderer.h"
#include "gpu/lod_stats.h"
#include "gpu/vk/vk_api.h"
#include "mem/gmem_access.h"

bool vk_guest_access_begin(struct renderer *r);
void vk_guest_access_end(struct renderer *r);
/* Returns only on success. Failed work must not reach writeback/label
 * publication or reuse an unsignaled fence. Logs and exits the guest process. */
void vk_check_work(struct renderer *r, VkResult result, const char *what);
/* Resource creation may recover from allocation/format failures, but a lost
 * device cannot be treated as an unsupported shader and signal completion. */
VkResult vk_check_device(struct renderer *r, VkResult result, const char *what);

typedef struct vk_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mapped;
} vk_buffer;

/* Host image standing for a guest surface */
#define VK_MAX_IMAGES 2048

/* vk_pipeline.c: drops per-image caches when a cache slot gets a new image */
struct renderer;
void vk_pipeline_forget_image(struct renderer *r, uint32_t index);
/* Remove framebuffers referring to a view, after its commands have completed. */
void vk_pipeline_forget_view(struct renderer *r, VkImageView view);

typedef struct vk_image {
    guest_addr addr;
    uint32_t width, height;
    VkFormat format;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;       /* 2D: layer 0 */
    VkImageView array_view; /* 2D array: every layer (cube faces, array slices) */
    uint32_t layers;
    uint32_t levels; /* mip levels: 1, or a sampled chain assembled from per-level images (vk_image_get_levels) */
    uint64_t chain_seq; /* chains: the newest write of the level images copied in */
    uint64_t chain_hash; /* chains from guest memory: the base level contents they were built from */
    uint32_t depth; /* 3D images: their slices (rendered through array_view, a 2D array of them); 0 for 2D */
    struct vk_image_sub {
        uint16_t base, count;
        bool array;
        VkFormat format; /* the view's (a reinterpretation of the image's bits, or the image's own) */
        VkImageUsageFlags usage; /* restricted reinterpretation views: sampled and storage are distinct */
        uint16_t min_lod; /* view-relative U4.8 residency clamp; dimensions stay unchanged */
        VkImageView view;
    } *sub; /* views of some layers (descriptors naming a slice range; one per
             * texture of an array: Minecraft's block textures), malloc'ed */
    uint32_t sub_count, sub_cap;
    VkImageLayout layout;
    VkAccessFlags access;
    VkPipelineStageFlags stage;
    VkImageAspectFlags aspect;
    bool storage;     /* created with VK_IMAGE_USAGE_STORAGE_BIT, its own format writable */
    bool color_attachment; /* format and allocation support color attachment use */
    bool mutable_fmt; /* views may reinterpret the bits as the same-size formats of vk_alias_formats */
    bool gpu_written; /* rendered by the GPU: guest memory is stale */
    uint64_t written_seq; /* order of the last GPU write (renderer write_seq) */
    uint64_t last_use;    /* renderer use_seq of the last batch that used it (eviction) */
    uint64_t phys;        /* offset in direct memory when addr is a view of it (VK_NO_PHYS: none): another
                           * view of the same memory is the same image (Sonic samples its depth there) */
    struct vk_image *feedback; /* private sampled copy, outside the guest surface cache */
    bool expanded_srgb; /* private RGBA8 copy of an R/RG UNORM source, sampled through sRGB */
    VkImageView ds_view; /* depth+stencil formats: the attachment view (view samples the depth only) */
    VkImageView stencil_view; /* its stencil read as an 8-bit unsigned texture (made on first use) */
    guest_addr stencil_addr;  /* DB_STENCIL_*_BASE of the stencil drawn with it (0: none) */
    VkDeviceSize allocated_bytes; /* physical allocation, including only resident mips for sparse images */
    uint32_t sparse_first; /* 0=dense; otherwise first physically bound mip (tail always bound) */
} vk_image;

/* Depth formats with a stencil aspect: attachments and layout changes cover
 * both aspects, while copies and sampling use the depth one (img->aspect). */
static inline bool vk_format_has_stencil(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

#define VK_NO_PHYS UINT64_MAX

typedef struct vk_present_frame {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    vk_buffer staging; /* CPU-drawn frame uploads */
} vk_present_frame;

struct vk_pipelines; /* vk_pipeline.c */

struct renderer {
    struct vk_checkpoint_state *checkpoints; /* opt-in immutable NVIDIA diagnostic markers */
    pal_window *window;
    gmem *mem;
    gmem_access guest_access; /* CPU access scopes, serialized by lock */
    pal_mutex *lock; /* all queue access and recording */

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    /* Vulkan version in use: what the instance asked for (1.4 when the loader has it,
     * PS5C_VK13=0 keeps 1.1) capped by the device's; caps lists the newer features found.
     * Detection only: nothing below is enabled yet (docs/plano-vulkan.md, step 1). */
    uint32_t instance_api, api;
    struct {
        bool dynamic_rendering, synchronization2, maintenance4, maintenance5;
        bool extended_dynamic_state, extended_dynamic_state2, extended_dynamic_state3;
        bool push_descriptor, pipeline_library, timeline_semaphore, buffer_device_address;
    } caps;
    bool format_list; /* VK_KHR_image_format_list: mutable images name their view formats */
    bool memory_budget;
    /* VK_EXT_descriptor_indexing: layouts past the ordinary per-stage limits can still be
     * built as update-after-bind ones, within these limits (copied from props.limits) */
    bool descriptor_uab;
    VkPhysicalDeviceLimits uab_limits;
    bool vs_layer;    /* VK_EXT_shader_viewport_index_layer: vertex shaders pick the target layer */
    bool resident_mips; /* opt-in CPU mip chains with VK_EXT_image_view_min_lod */
    bool sparse_mips; /* opt-in sparse BC chains; requires resident_mips and queue sparse binding */
    bool demote;      /* shaderDemoteToHelperInvocation enabled on this device */
    /* Step 2 (docs/plano-vulkan.md): cull, front face, depth test/write/compare, depth bias on,
     * stencil test and ops are dynamic state (Vulkan 1.3 core), not part of the pipeline key.
     * PS5C_VK_DYNSTATE=0 keeps them in the pipelines. */
    bool dyn_state;
    /* Vulkan 1.3 dynamic rendering (PS5C_VK_DYNRENDER=0: render passes and framebuffers) */
    bool dyn_render;
    /* VK_KHR_push_descriptor (PS5C_VK_PUSH=0: a descriptor set allocated and updated per draw) */
    uint32_t push_max; /* 0: off; else the most descriptors a pushed set may hold */
    PFN_vkCmdPushDescriptorSetKHR push_descriptor_set;
    PFN_vkCmdBeginRendering begin_rendering;
    PFN_vkCmdEndRendering end_rendering;
    PFN_vkCmdSetCullMode set_cull_mode;
    PFN_vkCmdSetFrontFace set_front_face;
    PFN_vkCmdSetDepthTestEnable set_depth_test;
    PFN_vkCmdSetDepthWriteEnable set_depth_write;
    PFN_vkCmdSetDepthCompareOp set_depth_compare;
    PFN_vkCmdSetDepthBiasEnable set_depth_bias_enable;
    PFN_vkCmdSetStencilTestEnable set_stencil_test;
    PFN_vkCmdSetStencilOp set_stencil_op;
    bool sampler_minmax; /* VK_EXT_sampler_filter_minmax: S# FILTER_MODE min/max reductions */
    float max_aniso;     /* samplerAnisotropy enabled: the device's maxSamplerAnisotropy (0: off) */
    bool pixel_interlock; /* opt-in native ordering for guest POPS shaders */
    bool clip_distance; /* shaderClipDistance enabled: user clip planes */
    bool logic_op;      /* logicOp enabled: CB_COLOR_CONTROL ROP3 */
    bool stencil;     /* D32_SFLOAT_S8_UINT depth/stencil attachments are supported (guest stencil) */
    uint32_t cs_subgroup; /* VK_EXT_subgroup_size_control: the subgroup size compute pipelines require (0: driver's choice) */
    VkPhysicalDeviceMemoryProperties mem_props;
    PFN_vkGetPhysicalDeviceMemoryProperties2 memory_properties2;
    VkPhysicalDeviceFeatures features;
    bool bvh_address; /* opt-in checked guest VA -> Vulkan device address map */
    PFN_vkGetBufferDeviceAddressKHR buffer_address;
    bool ps_vertex;
    bool lod_stats_enabled;
    gpu_lod_stats lod_stats;
    bool mesh_shader; /* opt-in native NGG backend */
    bool ngg_culling_profile; /* measured RADV no-GS culling compatibility */
    bool mesh_native32; /* fixed32 subgroups: opt-in two-wave software rendezvous */
    VkShaderStageFlags ballot32_stages; /* subgroup BASIC+BALLOT supported stages */
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks;
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_limits;
    VkDeviceSize host_import_align; /* opt-in external host allocations; zero when unavailable */
    PFN_vkGetMemoryHostPointerPropertiesEXT host_pointer_properties;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;

    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkSemaphore sc_rendered[8]; /* present completion is tied to the acquired image */

    VkCommandPool pool;
    vk_present_frame frames[2];
    uint32_t frame_index;

    /* Active aliases; optional second slot owns separate immutable snapshots. */
    VkCommandBuffer cmd;
    VkFence work_fence;
    VkCommandBuffer work_cmds[2];
    VkFence work_fences[2];
    /* Vulkan 1.2 timeline semaphore in place of the work fences (PS5C_VK_TIMELINE=0: fences):
     * every work submit signals the next value; a slot is done when the timeline reaches its value */
    VkSemaphore work_timeline;
    uint64_t work_value[2], work_submitted;
    PFN_vkWaitSemaphores wait_semaphores;
    uint32_t work_index;
    bool recording;

    vk_image images[VK_MAX_IMAGES];
    uint32_t image_count;
    uint32_t image_limit; /* active slots; a larger limit can be tested without reserving GPU memory */
    uint64_t image_bytes_limit; /* device memory the images may hold before new ones replace old ones (0: none) */
    /* Candidate indices only: selection still reads live formats, dimensions
     * and write sequences. Never cache an image's contents or a winner. */
    struct {
        guest_addr addr;
        uint64_t phys, generation;
        uint16_t indices[16];
        uint32_t count;
    } image_lookup[256];
    uint64_t image_lookup_generation;
    const char *cpu_prepare_setting;
    uint64_t cpu_prepare_flip;
    bool cpu_prepare_seen, cpu_prepare_enabled;
    bool image_cache_pressure; /* no slot safe to reuse until the current batch completes */
    struct vk_image_backing *image_backing; /* opt-in RAM copies of evicted GPU-written color images */
    uint64_t image_backing_bytes, image_backing_limit;
    uint64_t image_spills, image_restores;
    uint64_t write_seq; /* counts GPU writes to images */
    struct {
        vk_image *image;
        char path[128];
    } captures[16]; /* opt-in pass snapshots, read only after the whole frame */
    uint32_t capture_count;
    struct { guest_addr start, end; uint64_t phys; } phys_cache[128]; /* view VMAs, cleared each batch */
    uint32_t phys_cached;
    uint64_t phys_cache_seq;
    uint64_t use_seq;   /* counts recorded batches */

    struct vk_pipelines *pipes;

    /* system splash screen (renderer_set_splash) */
    vk_image splash;
    vk_buffer splash_staging; /* its texels until the first present uploads them */
    vk_image present_tmp; /* an integer frame copied as UNORM for the blit (present_as_unorm) */
    bool splash_on, splash_uploaded;
    /* a picture over everything (renderer_show_picture) */
    vk_image picture;
    vk_buffer picture_staging;
    bool picture_on, picture_dirty;
};

const char *vk_surface_extension_name(void);
bool vk_surface_create(renderer *r);

bool vk_alloc_memory(renderer *r, const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkDeviceMemory *out);
bool vk_sparse_image_create(renderer *r,const VkImageCreateInfo *info,uint32_t first,
                            VkImage *image,VkDeviceMemory *memory,VkDeviceSize *bytes);
bool vk_buffer_create(renderer *r, VkDeviceSize size, VkBufferUsageFlags usage, bool host, vk_buffer *out);
bool vk_buffer_create_local_host(renderer *r, VkDeviceSize size, VkBufferUsageFlags usage, vk_buffer *out);
/* Borrow existing host pages as a storage/transfer buffer. Caller keeps the
 * allocation alive (guest mappings pinned) until all work completes and the
 * buffer is destroyed. Never sets mapped: Vulkan does not own/map these pages. */
bool vk_buffer_import_host(renderer *r, void *host, VkDeviceSize bytes, vk_buffer *out);
/* Writes a color image to a BMP (debugging captures; waits for the queue). */
void vk_save_image(renderer *r, vk_image *src, const char *path);
void vk_depth_stats(renderer *r, vk_image *src, const char *what);
int64_t vk_color_coverage(renderer *r, vk_image *src);
void vk_save_depth(renderer *r, vk_image *src, const char *path);
/* Recreates a color image so views may reinterpret its bits (vk_alias_formats), its
 * contents copied in the command buffer being recorded; the old objects go once that
 * batch completed. Images start without it: mutable formats cost the driver's
 * compression (KAGE lost a third of its frame rate on Iris Xe). */
bool vk_image_promote(renderer *r, vk_image *img);
void vk_pipeline_counts(renderer *r, uint64_t *draws, uint64_t *dispatches);
/* Internal diagnostics: retained allocation bytes and successful snapshot bindings. */
void vk_readonly_cache_counts(renderer *r, uint64_t *bytes, uint64_t *uses);
void vk_buffer_destroy(renderer *r, vk_buffer *b);
/* The pre-draw contents of a depth attachment that the draw also samples. */
vk_image *vk_image_feedback(renderer *r, vk_image *source);
/* Independent GPU snapshot. Destroy only after its last command completed. */
vk_image *vk_image_snapshot(renderer *r, vk_image *source);
void vk_image_snapshot_destroy(renderer *r, vk_image *snapshot);
bool vk_capture_defer(renderer *r, vk_image *source, const char *path);
vk_image *vk_image_srgb(renderer *r, vk_image *source);
void vk_barrier(VkCommandBuffer cmd, vk_image *img, VkImageLayout to, VkAccessFlags dst_access,
                VkPipelineStageFlags dst_stage);

/* Waits for the submitted batch and copies its buffer results to guest memory. */
void vk_writebacks_complete(renderer *r);
vk_image *vk_image_find(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format);
/* Any host image of that address and size, whatever its format. */
vk_image *vk_image_find_any(renderer *r, guest_addr addr, uint32_t width, uint32_t height);
uint64_t vk_phys_of(renderer *r, guest_addr addr);
void vk_cpu_prepare(renderer *r, uint64_t flip);
/* An image with `levels` mip levels (lookups for single-level images never return it) */
vk_image *vk_image_get_levels(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format,
                              uint32_t layers, uint32_t levels);
/* CPU-streamed chain; never return a partial chain to ordinary get_levels(). */
vk_image *vk_image_get_resident_levels(renderer *r,guest_addr addr,uint32_t width,uint32_t height,VkFormat format,
                                      uint32_t levels,uint32_t first);
/* Whether an evicted image has newer GPU contents than this sequence. */
bool vk_image_backing_newer(renderer *r, guest_addr addr, uint32_t width, uint32_t height,
                            uint32_t layers, VkFormat format, uint64_t written_seq);
/* Finds or creates the host image for a guest surface. */
vk_image *vk_image_get(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format);
/* Bytes per texel of an uncompressed color format (0: depth, compressed or unknown). */
uint32_t vk_format_bytes(VkFormat f);
/* The 3D image (width x height x depth) at a guest address, created when missing. */
vk_image *vk_image_get_3d(renderer *r, guest_addr addr, uint32_t width, uint32_t height, uint32_t depth, VkFormat format);
vk_image *vk_image_find_3d(renderer *r, guest_addr addr, uint32_t width, uint32_t height, uint32_t depth, VkFormat format);
/* The formats a color image of this size may also be viewed as (its bits reinterpreted:
 * Unreal writes packed values through R32_UINT views of RGB10A2 and RGBA8 surfaces). */
uint32_t vk_alias_formats(VkFormat f, VkFormat *out);
vk_image *vk_image_find_array(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format);
vk_image *vk_image_find_layers(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format,
                               uint32_t layers);
vk_image *vk_image_get_layers(renderer *r, guest_addr addr, uint32_t width, uint32_t height, VkFormat format,
                              uint32_t layers);
/* Copies a linear guest surface into `img` (recorded into `cmd`). */
void vk_upload_linear(renderer *r, VkCommandBuffer cmd, vk_buffer *staging, vk_image *img, guest_addr addr,
                      uint32_t pitch_bytes);

/* Work command buffer: vk_begin starts recording if needed, vk_flush submits. */
VkCommandBuffer vk_begin(renderer *r);
void vk_flush(renderer *r);
VkResult vk_work_wait(renderer *r, uint64_t timeout_ns);
void vk_diagnostic_checkpoint(renderer *r, uint32_t kind, uint64_t a, uint64_t b, uint64_t ha, uint64_t hb);
/* vk_pipeline.c hooks: recording starts (the previous batch is complete, so
 * per-batch resources can be reused) and ends (open render passes close). */
void vk_work_begin(renderer *r);
bool vk_work_can_overlap(renderer *r);
bool vk_pipeline_test_draw_keys(void);
void vk_stat_work_slot(bool rotated);
void vk_work_end(renderer *r);
void vk_work_profile_end(renderer *r);

/* Draw/dispatch translation (vk_pipeline.c) */
bool vk_pipeline_init(renderer *r);
void vk_pipeline_shutdown(renderer *r);

/* PS5C_GPU_STATS helpers (vk_pipeline.c) */
uint64_t vk_stat_now(void);
void vk_stat_gpu_wait(uint64_t t0);
/* Native regression/diagnostic counts: fresh retained pools and reused draws. */
void vk_large_import_cache_counts(renderer *r,uint64_t *builds,uint64_t *reuses);

#endif
