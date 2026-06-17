/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"
#include "hw/xbox/nv2a/pgraph/glsl/ubershader.h" /* struct types */
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h" /* VshUniformValues */
#include "hw/xbox/nv2a/pgraph/glsl/psh.h" /* PshUniformValues */

#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <volk.h>
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>

#include "debug.h"
#include "constants.h"
#include "glsl.h"

#define HAVE_EXTERNAL_MEMORY 1

typedef struct QueueFamilyIndices {
    int queue_family;
} QueueFamilyIndices;

typedef struct MemorySyncRequirement {
    hwaddr addr, size;
} MemorySyncRequirement;

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
} RenderPassState;

typedef struct RenderPass {
    RenderPassState state;
    VkRenderPass render_pass;
} RenderPass;

typedef struct PipelineKey {
    bool clear;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    uint32_t regs[19];
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

#define UBER_B_SLOTS 2048

typedef struct PipelineBinding {
    LruNode node;
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkRenderPass render_pass;
    unsigned int draw_time;
    bool has_dynamic_line_width;
} PipelineBinding;

enum Buffer {
    BUFFER_STAGING_DST,
    BUFFER_STAGING_SRC,
    BUFFER_COMPUTE_DST,
    BUFFER_COMPUTE_SRC,
    BUFFER_INDEX,
    BUFFER_INDEX_STAGING,
    BUFFER_VERTEX_RAM,
    BUFFER_VERTEX_INLINE,
    BUFFER_VERTEX_INLINE_STAGING,
    BUFFER_UNIFORM,
    BUFFER_UNIFORM_STAGING,
    BUFFER_COUNT
};

typedef struct StorageBuffer {
    VkBuffer buffer;
    VkBufferUsageFlags usage;
    VmaAllocationCreateInfo alloc_info;
    VmaAllocation allocation;
    VkMemoryPropertyFlags properties;
    size_t buffer_offset;
    /* bytes already copied by an earlier chunk submit this
     * window (delta-copy watermark; reset with buffer_offset post-wait). */
    size_t synced_offset;
    size_t buffer_size;
    uint8_t *mapped;
} StorageBuffer;

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;

    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    VkImage image;
    VkImageView image_view;
    VmaAllocation allocation;

    // Used for scaling
    VkImage image_scratch;
    VkImageLayout image_scratch_current_layout;
    VmaAllocation allocation_scratch;

    bool initialized;
} SurfaceBinding;

typedef struct ShaderModuleInfo {
    int refcnt;
    char *glsl;
    GByteArray *spirv;
    VkShaderModule module;
    SpvReflectShaderModule reflect_module;
    SpvReflectDescriptorSet **descriptor_sets;
    ShaderUniformLayout uniforms;
    ShaderUniformLayout push_constants;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
        /* uber VSH upload locs cached once per module (mirrors the
         * geom cache) — kills 15 uniform_index strcmp scans/draw. */
        ShaderModuleInfo *uber_locs_for;
        int uber_locs[15];
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
        /* uniform locs lazily resolved at first upload for the
         * current module (uber_locs_for tracks which); idempotent re-resolve. */
        ShaderModuleInfo *uber_locs_for;
        int uber_locs[6];
    } geom;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
        /* uber PSH upload locs cached once per module (mirrors the
         * geom cache) — kills 20 uniform_index strcmp scans/draw. */
        ShaderModuleInfo *uber_locs_for;
        int uber_locs[21]; /* +u_tex_size */
    } psh;
} ShaderBinding;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    VkImage image;
    VkImageLayout current_layout;
    VkImageView image_view;
    VmaAllocation allocation;
    VkSampler sampler;
    bool possibly_dirty;
    uint64_t hash;
    unsigned int draw_time;
    uint32_t submit_time;
    /* the created image's level-0 extent (post any
     * surface-to-texture scaling) — carried into the uber PSH UBO so the
     * shader's RECT normalize reads a uniform instead of doing a
     * per-fragment textureSize() query (same integers, bit-exact). */
    uint32_t uber_dim_w, uber_dim_h;
    /* assigned slots in the batched sampler arrays
     * (-1 = unassigned). Identity is the binding itself (sampler params
     * are part of the cache key). Released into the deferred-recycle
     * list when the binding's resources are destroyed. */
    int16_t uber_slot;
    int16_t uber_slot_uint;
} TextureBinding;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
} QueryReport;

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

typedef struct PGRAPHVkDisplayState {
    ShaderModuleInfo *display_frag;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_set;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkRenderPass render_pass;
    VkFramebuffer framebuffer;

    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkSampler sampler;

    struct {
        PvideoState state;
        int width, height;
        VkImage image;
        VkImageView image_view;
        VmaAllocation allocation;
        VkSampler sampler;
    } pvideo;

    int width, height;
    int draw_time;
    /* the surface the display image last blitted (with
     * draw_time above + the pending flags, proves the blit would re-produce
     * identical bytes — skip it). Reset on display-image recreation. */
    hwaddr last_vram_addr;

    // OpenGL Interop
#ifdef WIN32
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_memory_obj;
    GLuint gl_texture_id;
} PGRAPHVkDisplayState;

typedef struct ComputePipelineKey {
    VkFormat host_fmt;
    bool pack;
    int workgroup_size;
} ComputePipelineKey;

typedef struct ComputePipeline {
    LruNode node;
    ComputePipelineKey key;
    VkPipeline pipeline;
} ComputePipeline;

typedef struct PGRAPHVkComputeState {
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1024];
    int descriptor_set_index;
    VkPipelineLayout pipeline_layout;
    Lru pipeline_cache;
    ComputePipeline *pipeline_cache_entries;
} PGRAPHVkComputeState;

typedef struct PGRAPHVkState {
    uint32_t vk_api_version;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;
    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;
    bool vertex_input_dynamic_state_enabled;
    bool extended_dynamic_state_enabled;
    bool multi_draw_enabled;
    bool extended_dynamic_state3_enabled;
    bool eds3_polygon_mode;
    bool descriptor_indexing_enabled; /* PARTIALLY_BOUND */
    /* device validates std430 layout on UNIFORM buffers
     * (uniformBufferStandardLayout and/or scalarBlockLayout, core 1.2) —
     * the uber blocks emit std430 when set, std140 fallback otherwise. */
    bool uber_ubo_std430;

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

    VkQueue queue;
    VkCommandPool command_pool;
    /* chunked submission — pairs of (main, aux) command
     * buffers cycle within a finish-window so the GPU starts mid-window;
     * the real finish's fence covers all (same-queue signal order). */
#define VK_CB_NPAIRS 8
    VkCommandBuffer command_buffers[2 * VK_CB_NPAIRS];
    VkSemaphore pair_semaphores[VK_CB_NPAIRS];
    int cb_pair_index;
    /* these were file-scope statics, on the assumption that uber_active is
     * fixed per renderer instance. Switching a live renderer from uber to spec
     * breaks that: a spec re-init inherited the uber session's tiny-key flag and
     * hashed/compared full spec keys over the 36-byte uber spans, colliding keys
     * and selecting the wrong pipelines. They are per-instance now (g_malloc0
     * zeroes them; init/prebake assign them explicitly). */
    bool uber_compact_pipeline_key;
    bool uber_tiny_pipeline_key;
    uint32_t chunk_count;        /* draws per command-buffer chunk; 0 = off */
    bool vstream_enabled;     /* vertex-stream path active */
    /* a pgraph_vk_finish between an attr's stream and the draw's
     * vkCmdBindVertexBuffers kills the ring epoch (copyback ran, wp reset) —
     * the bound ring offset would be overwritten by later streams before the
     * GPU reads it. Per-attr finish-generation stamps let the consume site
     * retarget exactly the dead entries to the 1:1 mirror (the copyback put
     * those bytes there). */
    uint32_t vstream_finish_gen;
    uint32_t vstream_gen[NV2A_VERTEXSHADER_ATTRIBUTES];
    hwaddr vstream_mirror_offset[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint16_t vstream_mask;
    uint32_t chunk_flushes_since;

    VkCommandBuffer command_buffer;
    VkSemaphore command_buffer_semaphore;
    VkFence command_buffer_fence;
    unsigned int command_buffer_start_time;
    bool in_command_buffer;
    uint32_t submit_count;

    VkCommandBuffer aux_command_buffer;
    bool in_aux_command_buffer;

    VkFramebuffer framebuffers[256];
    int framebuffer_index;
    bool framebuffer_dirty;

    VkRenderPass render_pass;
    GArray *render_passes; // RenderPass
    bool in_render_pass;
    bool in_draw;

    Lru pipeline_cache;
    /* persistent once-zeroed key template — under the
     * fully-dynamic uber config every consumed field is rewritten per draw
     * and nothing else is ever written, so the per-draw ~3KB memset dies. */
    /* cross-draw batching state. caps = device support
     * (set at device creation); enabled = caps && multi_draw && the
     * fully-dynamic config (decided at uber init). Slot manager: slot
     * identity = (image view, sampler) pair; freed slots recycle only
     * after the GPU passes the frame that last referenced them. */
    bool uber_batched_caps;
    bool uber_batched;
    /* set 2 = the batched machinery. One persistent set
     * (UPDATE_AFTER_BIND; fixed-count PARTIALLY_BOUND arrays — variable
     * count is legal only on the last binding, so fixed 2048 per type).
     * Record ring: host-visible, reset at finish (GPU idle); per-range
     * uvec4 records (packed 2D|uint<<16 slot per stage). Freed slots
     * recycle only after a finish that follows the free. */
    VkDescriptorSetLayout uber_b_set_layout;
    VkDescriptorPool uber_b_pool;
    VkDescriptorSet uber_b_set;
    VkBuffer uber_b_rec_buffer;
    VmaAllocation uber_b_rec_alloc;
    uint8_t *uber_b_rec_mapped;
    uint32_t uber_b_rec_wp;
    uint32_t uber_b_rec_capacity;

    /* the cross-flush merge accumulator. Draw-arrays
     * flushes append ranges here instead of encoding; any real state encode
     * (hooked at the encode sites) flushes the pending batch first, so
     * correctness holds by construction. */
#define UBER_M_CAP 4096
    VkMultiDrawInfoEXT uber_m_infos[UBER_M_CAP];
    uint32_t uber_m_count;
    uint32_t uber_m_base;       /* first record index of the batch */
    uint32_t uber_m_nflushes;   /* guest flushes folded into the batch */
    uint32_t uber_m_cap;        /* cross-draw merge cap */
    /* last-EMITTED dynamic-state values — a dirty EDS block
     * re-emits only the values that moved (reset with the block caches at
     * begin_render_pass; dynamic state is per-render-pass-validated here). */
    struct {
        bool valid;
        bool dt, dw;
        VkCompareOp dop;
        bool st_en;
        uint32_t st_ops_key;
        uint32_t cmask, wmask, ref;
        VkCullModeFlags cull;
        VkFrontFace ff;
        uint32_t bc_packed;
    } eds1_emit;
    struct {
        bool valid;
        VkColorComponentFlags wm;
        VkBool32 be;
        uint32_t eq_key;
        uint32_t bc_packed;
        int pm;
    } eds3_emit;
    bool uber_m_bds_valid;      /* bind_descriptor_sets shadow */
    VkDescriptorSet uber_m_bds_sets[3];
    bool uber_m_vb_valid;       /* bind_vertex_buffer shadow */
    uint32_t uber_m_vb_n;
    VkBuffer uber_m_vb_buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize uber_m_vb_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint16_t uber_b_free[4][2048];
    int uber_b_free_top[4];
    struct {
        uint16_t slot;
        uint8_t type;
        uint32_t seq;
    } uber_b_pending[4 * UBER_B_SLOTS];
    int uber_b_npending;
    uint32_t uber_b_finish_seq;
    PipelineKey uber_key_template;
    bool uber_key_template_init;
    VkPipelineCache vk_pipeline_cache;
    PipelineBinding *pipeline_cache_entries;
    PipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;

    /* 4x-larger descriptor pools (2048 -> 8192) so pgraph_vk_finish, a full GPU
     * sync, fires about 4x less often on draw-heavy scenes. The three
     * set-index-keyed arrays below (descriptor_sets, uber_tex_descriptor_sets,
     * uber_tex_set_pattern) must be the same size, or a mismatch is an
     * out-of-bounds heap write. Paired with a 4x BUFFER_UNIFORM (buffer.c) since
     * the uniform staging exhausts too. */
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[8192];
    int descriptor_set_index;

    /* uber splits textures into their own descriptor set
     * (set=1) so per-draw UBO churn rewrites 3 descriptors instead of 19;
     * the texture set is rewritten only when texture bindings change.
     * Unused (VK_NULL_HANDLE / 0) when !uber_active. */
    VkDescriptorSetLayout uber_tex_descriptor_set_layout;
    VkDescriptorSet uber_tex_descriptor_sets[8192];
    int uber_tex_descriptor_set_index;
    /* per-set last-written family pattern (2 bits/stage:
     * is_cube|is_3d). Dummy-slot descriptors are eternal objects, so when a
     * recycled set's pattern matches, the up-to-8 dummy writes are skipped
     * (real-texture slots are always rewritten). 0xFF = unwritten/invalid. */
    uint8_t uber_tex_set_pattern[8192];

    StorageBuffer storage_buffers[BUFFER_COUNT];

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    unsigned long *uploaded_bitmap;
    size_t bitmap_size;

    VkVertexInputAttributeDescription vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;

    VkVertexInputBindingDescription vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_binding_descriptions;
    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    /* vertex-RAM streaming (uber-only). When an attribute's
     * vertex bytes are still in-flight (a prior draw reads them) and this draw
     * needs them, copy them into a 32MB ring at the TOP of BUFFER_VERTEX_RAM and
     * point the attribute's binding offset straight at the ring copy (exact, no
     * routing ambiguity) — instead of a full GPU sync. The 1:1 mirror keeps the
     * OLD bytes for the in-flight draws and is refreshed from the ring at the
     * next finish (post-drain). Overflow falls back to the sync. */
    size_t vertex_stream_base;
    size_t vertex_stream_size;
    size_t vertex_stream_wp;
    struct { hwaddr region_start; hwaddr size; size_t stream_pos; }
        vertex_stream_remap[2048];
    int vertex_stream_remap_count;

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    TextureBinding dummy_texture;
    TextureBinding dummy_texture_cube;
    TextureBinding dummy_texture_3d;
    bool texture_bindings_changed;
    VkFormatProperties *texture_format_properties;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    ShaderModuleInfo *quad_vert_module, *solid_frag_module;
    bool shader_bindings_changed;
    bool use_push_constants_for_uniform_attrs;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    bool uber_active;
    bool uber_state_changed;
    /* uber_program_data_dirty removed — it was computed every
     * state-dirty draw and never read (write-only vestige of a removed gate). */
    /* the three uber uniform structs depend
     * only on shader state + texture bindings + surface dims (the gate),
     * so they — and their hash contribution — are recomputed only when
     * uber_structs_dirty; value-only draws reuse these cached copies. */
    UberPshUniforms uber_psh_cached;
    UberVshUniforms uber_vsh_cached;
    UberGeomUniforms uber_geom_cached;
    uint64_t uber_structs_subhash;
    /* the per-draw uniform VALUE structs, cached and recomputed
     * only when a covering signal fires (vsh: state/gen/ltctx/regs/dims — c[]
     * excluded, texture bindings verified-unread; psh: state/tex/gen/
     * regs/dims — ltctx verified-unread). Subhashes ride the recomputes.
     * The 3KB c[] hash contribution, reused while the
     * constants' dirty flags are clean. */
    VshUniformValues uber_vsh_values_cached;
    PshUniformValues uber_psh_values_cached;
    uint64_t uber_vsh_values_subhash;
    uint64_t uber_psh_values_subhash;
    uint64_t uber_c_subhash;
    /* vsh_values reads pgraph_get_inline_values for
     * the uniform_attrs, but inline vertex-attr writes (SET_VERTEX_DATA*) bump
     * no covering signal (unlike material/light/point which bump uniform_state_
     * gen) — so the value-cache went stale on inline-value changes, breaking
     * fog/material/spotlight/vsh-round/shadow/blend. Hash the consumed inline
     * values to gate the vsh-values recompute. */
    uint64_t uber_inline_values_hash;
    bool uber_values_valid;
    bool uber_c_hash_valid;
    uint32_t uber_tex_enable;
    uint32_t uber_tex_modes;
    int32_t uber_depth_format;

    uint64_t last_uniform_state_gen;
    /* last-consumed writer generations (see pgraph.h). */
    uint64_t uber_last_c_wgen;
    uint64_t uber_last_ltctx_wgen;
    uint32_t last_surface_width, last_surface_height;
    uint32_t last_alpha_ref, last_z_persp;
    uint32_t last_setupraster_uniform, last_zoffsetbias, last_zoffsetfactor;

    struct {
        uint32_t control_0, control_1, control_2, setupraster, blendcolor;
        bool valid;
    } eds1_cache;
    /* EDS3 change-cache, mirroring eds1_cache (invalidated
     * per render pass alongside it). pm = the resolved VkPolygonMode. */
    struct {
        uint32_t control_0, blend, blendcolor;
        int pm;
        bool valid;
    } eds3_cache;
    /* vertex-input dynamic-state change cache (per render
     * pass, like the EDS caches). */
    bool vids_cache_valid;
    /* FNV hash of the active vertex-input descriptions the
     * vkCmdSetVertexInputEXT consumes, so the per-draw rebuild only forces a
     * re-SET when the format actually changes (not every draw). */
    uint64_t vids_format_hash;
    bool uber_prebaked;
    ShaderModuleInfo *uber_vsh_module;
    ShaderModuleInfo *uber_geom_module;
    ShaderModuleInfo *uber_geom_quad_module;
    ShaderModuleInfo *uber_geom_quad_wireframe_module;
    ShaderModuleInfo *uber_geom_line_module;
    ShaderModuleInfo *uber_geom_points_module;
    /* NV passthrough GS for smooth tri-class draws */
    ShaderModuleInfo *uber_geom_tri_pass_module;
    bool geom_passthrough_enabled;
    ShaderModuleInfo *uber_psh_module;
    ShaderBinding uber_shader_binding;
    ShaderBinding uber_shader_binding_quad;
    ShaderBinding uber_shader_binding_quad_wireframe;
    ShaderBinding uber_shader_binding_line;
    ShaderBinding uber_shader_binding_points;
    ShaderBinding uber_shader_binding_tri_pass;

    // FIXME: Merge these into a structure
    uint64_t uniform_buffer_hashes[3];
    size_t uniform_buffer_offsets[3];
    bool uniforms_changed;
    /* adaptive content-hash gate. credit tracks the recent UBO
     * skip rate; hash while it pays, else probe 1/64 to re-arm (so
     * rapidly-varying content pays almost no hash cost). valid=false marks
     * the baseline stale after a non-hashed draw so the next hash re-stages. */
    int uber_hash_credit;
    uint32_t uber_hash_probe;
    bool uber_hash_valid;
    /* skip the per-draw uber-struct std140 upload-expansion when
     * the structs are byte-identical to last draw (gated on the credit). */
    uint64_t uber_struct_hash;

    VkQueryPool query_pool;
    int max_queries_in_flight; // FIXME: Move out to constant
    int num_queries_in_flight;
    bool new_query_needed;
    bool query_in_flight;
    uint32_t zpass_pixel_count_result;
    QSIMPLEQ_HEAD(, QueryReport) report_queue; // FIXME: Statically allocate

    SurfaceFormatInfo kelvin_surface_zeta_vk_map[3];

    uint32_t clear_parameter;

    PGRAPHVkDisplayState display;
    PGRAPHVkComputeState compute;
} PGRAPHVkState;

void pgraph_vk_uber_b_release_slots(PGRAPHVkState *r, TextureBinding *tb);

// renderer.c
void pgraph_vk_check_memory_budget(PGRAPHState *pg);

// debug.c
#define RGBA_RED     (float[4]){1,0,0,1}
#define RGBA_YELLOW  (float[4]){1,1,0,1}
#define RGBA_GREEN   (float[4]){0,1,0,1}
#define RGBA_BLUE    (float[4]){0,0,1,1}
#define RGBA_PINK    (float[4]){1,0,1,1}
#define RGBA_DEFAULT (float[4]){0,0,0,0}

void pgraph_vk_debug_init(void);
void pgraph_vk_insert_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                   float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_end_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd);

// instance.c
void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_vk_finalize_instance(PGRAPHState *pg);
QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device);
uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties);

// glsl.c
void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);
GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source);
VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r,
                                                       GByteArray *spv);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_spirv(
    PGRAPHVkState *r, const uint8_t *spirv_data, size_t spirv_size);
void pgraph_vk_ref_shader_module(ShaderModuleInfo *info);
void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);
void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);

// buffer.c
void pgraph_vk_init_buffers(NV2AState *d);
void pgraph_vk_finalize_buffers(NV2AState *d);
bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment);
VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment);

// command.c
void pgraph_vk_init_command_buffers(PGRAPHState *pg);
void pgraph_vk_finalize_command_buffers(PGRAPHState *pg);
VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg);
void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// image.c
void pgraph_vk_transition_image_layout(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkImage image, VkFormat format,
                                       VkImageLayout oldLayout,
                                       VkImageLayout newLayout);

// vertex.c
void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element);
void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset, void *data,
                                    VkDeviceSize size);
VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count);

// surface.c
void pgraph_vk_init_surfaces(PGRAPHState *pg);
void pgraph_vk_finalize_surfaces(PGRAPHState *pg);
void pgraph_vk_surface_flush(NV2AState *d);
void pgraph_vk_process_pending_downloads(NV2AState *d);
void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_vk_wait_for_surface_download(SurfaceBinding *e);
void pgraph_vk_download_dirty_surfaces(NV2AState *d);
void pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start, hwaddr size);
void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force);
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write);
SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr);
void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d);
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg);

// surface-compute.c
void pgraph_vk_init_compute(PGRAPHState *pg);
bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r);
void pgraph_vk_compute_finish_complete(PGRAPHVkState *r);
void pgraph_vk_finalize_compute(PGRAPHState *pg);
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale);
void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst);

// display.c
void pgraph_vk_init_display(PGRAPHState *pg);
void pgraph_vk_finalize_display(PGRAPHState *pg);
void pgraph_vk_render_display(PGRAPHState *pg);

// texture.c
void pgraph_vk_init_textures(PGRAPHState *pg);
void pgraph_vk_finalize_textures(PGRAPHState *pg);
void pgraph_vk_bind_textures(NV2AState *d);
void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                            hwaddr size);
void pgraph_vk_trim_texture_cache(PGRAPHState *pg);

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
void pgraph_vk_bind_shaders(PGRAPHState *pg);

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_process_pending_reports_internal(NV2AState *d);

typedef enum FinishReason {
    VK_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    VK_FINISH_REASON_SURFACE_CREATE,
    VK_FINISH_REASON_SURFACE_DOWN,
    VK_FINISH_REASON_NEED_BUFFER_SPACE,
    VK_FINISH_REASON_FRAMEBUFFER_DIRTY,
    VK_FINISH_REASON_PRESENTING,
    VK_FINISH_REASON_FLIP_STALL,
    VK_FINISH_REASON_FLUSH,
    VK_FINISH_REASON_STALLED,
} FinishReason;

// draw.c
void pgraph_vk_init_pipelines(PGRAPHState *pg);
void pgraph_vk_finalize_pipelines(PGRAPHState *pg);
void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_vk_draw_begin(NV2AState *d);
void pgraph_vk_draw_end(NV2AState *d);
void pgraph_vk_finish(PGRAPHState *pg, FinishReason why);
void pgraph_vk_flush_draw(NV2AState *d);
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

// gpuprops.c
void pgraph_vk_determine_gpu_properties(NV2AState *d);
GPUProperties *pgraph_vk_get_gpu_properties(void);

#endif
