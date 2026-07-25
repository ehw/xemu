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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "renderer.h"
#include "ui/xemu-settings.h"
#include "xemu-version.h"
#include <math.h>



void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int polygon_mode = r->shader_binding->state.geom.polygon_front_mode;
    int primitive_mode = r->shader_binding->state.geom.primitive_mode;

    // FIXME: Replace with LUT
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINE_LOOP:
        // FIXME: line strips, except that the first and last vertices are also used as a line
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME
        } else if (polygon_mode == POLY_MODE_FILL) {
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

/* uber pipeline keys never populate vsh.program_data (the
 * key is memset and sparsely filled - see init_pipeline_key), so the 2176B
 * window is always zero. Skip it in the hash and the LRU compare.
 * The flags live on PGRAPHVkState rather than in file-scope statics, which
 * would survive a live renderer switch and poison a subsequent spec instance
 * (the compare callback derives r via container_of). */

#define PIPELINE_KEY_DEAD_OFFS \
    offsetof(PipelineKey, shader_state.vsh.programmable.program_data)
#define PIPELINE_KEY_DEAD_SIZE \
    sizeof(((PipelineKey *)0)->shader_state.vsh.programmable.program_data)

/* under the fully-dynamic uber config (the prebake gate:
 * EDS+EDS3+polymode+vids) the live non-clear key bytes reduce to exactly
 * [0, shader_state) (clear flag + render-pass formats) and the GeomState
 * window (primitive/polygon modes + the selector in smooth_shading;
 * its remaining fields are never written = zero on both sides). Hash and
 * compare only those ~36 bytes instead of the ~1.2KB spans. Clear keys
 * carry clear_parameter in regs[] and
 * keep the spans; a clear/non-clear pair always differs inside span1
 * (the clear byte), so cross-class compares stay correct. */

#define PIPELINE_KEY_HEAD_SIZE offsetof(PipelineKey, shader_state)
#define PIPELINE_KEY_GEOM_OFFS \
    (offsetof(PipelineKey, shader_state) + offsetof(ShaderState, geom))
#define PIPELINE_KEY_GEOM_SIZE sizeof(((PipelineKey *)0)->shader_state.geom)

static uint64_t pipeline_key_hash(const PGRAPHVkState *r,
                                  const PipelineKey *key)
{
    if (r->uber_tiny_pipeline_key && !key->clear) {
        const uint8_t *p = (const uint8_t *)key;
        return fast_hash(p, PIPELINE_KEY_HEAD_SIZE) ^
               (fast_hash(p + PIPELINE_KEY_GEOM_OFFS,
                          PIPELINE_KEY_GEOM_SIZE) * 2654435761u);
    }
    if (!r->uber_compact_pipeline_key) {
        return fast_hash((const void *)key, sizeof(*key));
    }
    const uint8_t *p = (const uint8_t *)key;
    size_t a = PIPELINE_KEY_DEAD_OFFS;
    size_t b = a + PIPELINE_KEY_DEAD_SIZE;
    return fast_hash(p, a) ^ (fast_hash(p + b, sizeof(*key) - b) * 2654435761u);
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    const PGRAPHVkState *r =
        container_of(lru, const PGRAPHVkState, pipeline_cache);
    const PipelineKey *k = (const PipelineKey *)key;
    if (r->uber_tiny_pipeline_key && !k->clear && !snode->key.clear) {
        const uint8_t *x = (const uint8_t *)&snode->key;
        const uint8_t *y = (const uint8_t *)key;
        return memcmp(x, y, PIPELINE_KEY_HEAD_SIZE) ||
               memcmp(x + PIPELINE_KEY_GEOM_OFFS, y + PIPELINE_KEY_GEOM_OFFS,
                      PIPELINE_KEY_GEOM_SIZE);
    }
    if (!r->uber_compact_pipeline_key) {
        return memcmp(&snode->key, key, sizeof(PipelineKey));
    }
    const uint8_t *x = (const uint8_t *)&snode->key;
    const uint8_t *y = (const uint8_t *)key;
    size_t a = PIPELINE_KEY_DEAD_OFFS;
    size_t b = a + PIPELINE_KEY_DEAD_SIZE;
    return memcmp(x, y, a) ||
           memcmp(x + b, y + b, sizeof(PipelineKey) - b);
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* In-memory driver pipeline cache only. The ubershader prebakes every
     * pipeline at startup and creates none during gameplay, so there is
     * nothing worth persisting — the on-disk pipeline cache was removed. */
    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = 0,
        .pInitialData = NULL,
        .pNext = NULL,
    };
    VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                   &r->vk_pipeline_cache));

    const size_t pipeline_cache_size = 2048;
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->uber_compact_pipeline_key = r->uber_active;
    r->uber_tiny_pipeline_key = false; /* the prebake gate re-arms it */
    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                               &r->command_buffer_semaphore));
    for (int i = 0; i < VK_CB_NPAIRS; i++) {
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &r->pair_semaphores[i]));
    }
    /* The ubershader uses the vertex-stream path (which avoids the per-draw
     * vertex-buffer sync) and submits its command buffers in chunks of a fixed
     * number of draws so that CPU encoding overlaps GPU execution. */
    r->vstream_enabled = true;
    r->chunk_count = r->uber_active ? 1024 : 0;
    r->chunk_flushes_since = 0;

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(
        vkCreateFence(r->device, &fence_info, NULL, &r->command_buffer_fence));
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    vkDestroyFence(r->device, r->command_buffer_fence, NULL);
    vkDestroySemaphore(r->device, r->command_buffer_semaphore, NULL);
    for (int i = 0; i < VK_CB_NPAIRS; i++) {
        vkDestroySemaphore(r->device, r->pair_semaphores[i], NULL);
    }
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
}

static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = pipeline_key_hash(r, &key);
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->pipeline_binding || r->shader_bindings_changed ||
        r->texture_bindings_changed ||
        check_render_pass_dirty(pg)) {
        return true;
    }

    if (r->uber_active &&
        r->shader_binding->state.geom.primitive_mode !=
            r->pipeline_binding->key.shader_state.geom.primitive_mode) {
        return true;
    }

    /* with EDS live, the dirty check matches exactly
     * what init_pipeline_key keys on - BLEND/CONTROL_0 under EDS1-only,
     * nothing under EDS3 (those regs are dynamic state now). Was disabled
     * with the rest of the EDS code. */
    if (r->uber_active && r->extended_dynamic_state_enabled) {
        if (!r->extended_dynamic_state3_enabled) {
            const unsigned int regs[] = {
                NV_PGRAPH_BLEND, NV_PGRAPH_CONTROL_0,
            };
            for (int i = 0; i < ARRAY_SIZE(regs); i++) {
                if (pgraph_is_reg_dirty(pg, regs[i])) {
                    return true;
                }
            }
        }
    } else {
        const unsigned int regs[] = {
            NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
            NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
            NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
        };
        for (int i = 0; i < ARRAY_SIZE(regs); i++) {
            if (pgraph_is_reg_dirty(pg, regs[i])) {
                return true;
            }
        }
    }

    // FIXME: Use dirty bits instead
    /* with vertex-input dynamic state the descs are not
     * pipeline state - they ride vkCmdSetVertexInputEXT instead. */
    if (!(r->uber_active && r->vertex_input_dynamic_state_enabled) &&
        (memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0])))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* the memset moved to the caller — under the tiny-key
     * config the caller reuses a once-zeroed persistent template (every
     * field written below fully overwrites its previous value; nothing
     * else is ever written). */
    init_render_pass_state(pg, &key->render_pass_state);
    if (r->uber_active) {
        /* is_fixed_function and combiner_control were keyed
         * "in case state matters per draw" - verified consumed by NOTHING in
         * pipeline creation (the uber shader stages are the fixed 6 programs;
         * combiners/FF are uniform-driven). Keying them multiplied
         * bit-identical pipelines. */
        key->shader_state.geom.primitive_mode =
            r->shader_binding->state.geom.primitive_mode;
        /* the quad-wireframe FILL-forcing collided
         * QUADS+FILL (solid-quad GS) with QUADS+LINE (wireframe GS) into ONE
         * key - first-created won, so wireframe quads rendered with the
         * wrong GS (a Vulkan-only bug; GL dispatches programs directly and was
         * unaffected). The forcing was vestigial: the rasterizer polygonMode
         * is dynamic (EDS3) and begin_draw already forces FILL at set-time
         * for the quad-wf binding. Key the real modes; the keys un-collide. */
        key->shader_state.geom.polygon_front_mode =
            r->shader_binding->state.geom.polygon_front_mode;
        key->shader_state.geom.polygon_back_mode =
            r->shader_binding->state.geom.polygon_back_mode;
        /* the only remaining state pipeline creation consumes
         * is WHICH tri GS module the binding selected (passthrough vs
         * classic) - key exactly that bit, in the (otherwise-unconsumed)
         * smooth_shading slot, so the two module choices cannot collide
         * into one key. */
        key->shader_state.geom.smooth_shading =
            (r->shader_binding == &r->uber_shader_binding_tri_pass);
    } else {
        memcpy(&key->shader_state, &r->shader_binding->state,
               sizeof(ShaderState));
    }
    if (!(r->uber_active && r->vertex_input_dynamic_state_enabled)) {
        /* descs are dynamic state, not key material */
        memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
               sizeof(key->binding_descriptions[0]) *
                   r->num_active_vertex_binding_descriptions);
        memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
               sizeof(key->attribute_descriptions[0]) *
                   r->num_active_vertex_attribute_descriptions);
    }

    if (r->uber_active && r->extended_dynamic_state_enabled) {
        if (!r->extended_dynamic_state3_enabled) {
            key->regs[0] = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
            key->regs[1] = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0)
                & (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE
                   | NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE
                   | NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE
                   | NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
        }
    } else {
        const int regs[] = {
            NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
            NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
            NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
        };
        assert(ARRAY_SIZE(regs) <= ARRAY_SIZE(key->regs));
        for (int i = 0; i < ARRAY_SIZE(regs); i++) {
            key->regs[i] = pgraph_reg_r(pg, regs[i]);
        }
    }
}


/* BEGIN: pre-create the full uber pipeline space.
 * The uber pipeline key reduces to
 * (primitive_mode, polygon modes) x render-pass formats - statically
 * enumerable. Creating all of them up front (seeded into the LRU with keys
 * byte-identical to init_pipeline_key's runtime output) makes runtime
 * pipeline creation structurally zero: every lookup hits. Only the
 * fully-dynamic config matches these keys, so lesser hardware simply skips
 * the prebake and keeps the on-demand path. Clear pipelines are not
 * enumerated (their key carries the raw clear_parameter). */
static void uber_prebake_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->extended_dynamic_state_enabled ||
        !r->extended_dynamic_state3_enabled || !r->eds3_polygon_mode ||
        !r->vertex_input_dynamic_state_enabled) {
        return;
    }

    /* fully-dynamic config confirmed — the tiny key spans
     * are exact from here on (set before seeding so prebaked keys hash
     * identically to runtime probes). */
    r->uber_tiny_pipeline_key = true;


    enum { GM_TRI, GM_TRI_WF, GM_QUAD, GM_QUAD_WF, GM_LINE, GM_POINTS,
           GM_TRI_PASS };
    /* VK has no tri-wireframe GS module - triangle-class wireframe uses the
     * normal tri GS with dynamic polygonMode (only quad/polygon wireframe
     * are GS-emulated). GM_TRI_WF aliases the tri module so the key rows
     * (front_mode=LINE) still enumerate. */
    ShaderModuleInfo *gmods[7] = {
        r->uber_geom_module,        r->uber_geom_module,
        r->uber_geom_quad_module,   r->uber_geom_quad_wireframe_module,
        r->uber_geom_line_module,   r->uber_geom_points_module,
        r->uber_geom_tri_pass_module,
    };
    /* `pass` mirrors the key's smooth_shading slot (set only
     * when the passthrough module is the one the key selects). Tri-class
     * rows are enumerated in both flavors so runtime lookups hit either
     * way; pass rows are skipped when the extension is unavailable. */
    static const struct {
        uint8_t prim, front_mode, gm, pass;
        VkPrimitiveTopology topo;
    } combos[] = {
        { PRIM_TYPE_POINTS,         POLY_MODE_FILL, GM_POINTS,  0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST },
        { PRIM_TYPE_LINES,          POLY_MODE_FILL, GM_LINE,    0, VK_PRIMITIVE_TOPOLOGY_LINE_LIST },
        { PRIM_TYPE_LINE_LOOP,      POLY_MODE_FILL, GM_LINE,    0, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP },
        { PRIM_TYPE_LINE_STRIP,     POLY_MODE_FILL, GM_LINE,    0, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP },
        { PRIM_TYPE_TRIANGLES,      POLY_MODE_FILL, GM_TRI,     0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
        { PRIM_TYPE_TRIANGLE_STRIP, POLY_MODE_FILL, GM_TRI,     0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP },
        { PRIM_TYPE_TRIANGLE_FAN,   POLY_MODE_FILL, GM_TRI,     0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },
        { PRIM_TYPE_QUADS,          POLY_MODE_FILL, GM_QUAD,    0, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY },
        { PRIM_TYPE_QUAD_STRIP,     POLY_MODE_FILL, GM_QUAD,    0, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY },
        { PRIM_TYPE_POLYGON,        POLY_MODE_FILL, GM_TRI,     0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },
        { PRIM_TYPE_TRIANGLES,      POLY_MODE_LINE, GM_TRI_WF,  0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
        { PRIM_TYPE_TRIANGLE_STRIP, POLY_MODE_LINE, GM_TRI_WF,  0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP },
        { PRIM_TYPE_TRIANGLE_FAN,   POLY_MODE_LINE, GM_TRI_WF,  0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },
        { PRIM_TYPE_QUADS,          POLY_MODE_LINE, GM_QUAD_WF, 0, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY },
        { PRIM_TYPE_QUAD_STRIP,     POLY_MODE_LINE, GM_QUAD_WF, 0, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY },
        { PRIM_TYPE_POLYGON,        POLY_MODE_LINE, GM_LINE,    0, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP },
        /* passthrough flavors of every tri-class row */
        { PRIM_TYPE_TRIANGLES,      POLY_MODE_FILL, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
        { PRIM_TYPE_TRIANGLE_STRIP, POLY_MODE_FILL, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP },
        { PRIM_TYPE_TRIANGLE_FAN,   POLY_MODE_FILL, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },
        { PRIM_TYPE_POLYGON,        POLY_MODE_FILL, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },
        { PRIM_TYPE_TRIANGLES,      POLY_MODE_LINE, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
        { PRIM_TYPE_TRIANGLE_STRIP, POLY_MODE_LINE, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP },
        { PRIM_TYPE_TRIANGLE_FAN,   POLY_MODE_LINE, GM_TRI_PASS, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN },
    };
    static const VkFormat colors[] = {
        VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8_UNORM,
        VK_FORMAT_A1R5G5B5_UNORM_PACK16,
        VK_FORMAT_A4R4G4B4_UNORM_PACK16,
        VK_FORMAT_R5G6B5_UNORM_PACK16,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
    };
    static const VkFormat zetas[] = {
        VK_FORMAT_UNDEFINED,
        VK_FORMAT_D16_UNORM,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
    };

    for (int ci = 0; ci < ARRAY_SIZE(colors); ci++) {
        for (int zi = 0; zi < ARRAY_SIZE(zetas); zi++) {
            if (colors[ci] == VK_FORMAT_UNDEFINED &&
                zetas[zi] == VK_FORMAT_UNDEFINED) {
                continue;
            }
            RenderPassState rps = {
                .color_format = colors[ci],
                .zeta_format = zetas[zi],
            };
            VkRenderPass rpass = get_render_pass(r, &rps);

            for (int k = 0; k < ARRAY_SIZE(combos); k++) {
                if (combos[k].pass && !r->geom_passthrough_enabled) {
                    continue;
                }
                PipelineKey key;
                memset(&key, 0, sizeof(key));
                key.render_pass_state = rps;
                key.shader_state.geom.primitive_mode = combos[k].prim;
                key.shader_state.geom.polygon_front_mode =
                    combos[k].front_mode;
                key.shader_state.geom.polygon_back_mode =
                    combos[k].front_mode;
                /* keys carry the real modes now - the
                 * quad-wf rows keep LINE and no longer collide with FILL */
                key.shader_state.geom.smooth_shading =
                    combos[k].pass; /* the key's module bit */

                uint64_t hash = pipeline_key_hash(r, &key);
                LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
                PipelineBinding *snode =
                    container_of(node, PipelineBinding, node);
                if (snode->pipeline != VK_NULL_HANDLE) {
                    continue;
                }
                memcpy(&snode->key, &key, sizeof(key));

                int nstages = 0;
                VkPipelineShaderStageCreateInfo stages[3];
                stages[nstages++] = (VkPipelineShaderStageCreateInfo){
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = r->uber_vsh_module->module,
                    .pName = "main",
                };
                stages[nstages++] = (VkPipelineShaderStageCreateInfo){
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                    .module = gmods[combos[k].gm]->module,
                    .pName = "main",
                };
                stages[nstages++] = (VkPipelineShaderStageCreateInfo){
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = r->uber_psh_module->module,
                    .pName = "main",
                };

                VkPipelineInputAssemblyStateCreateInfo input_assembly = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                    .topology = combos[k].topo,
                    .primitiveRestartEnable = VK_FALSE,
                };
                VkPipelineViewportStateCreateInfo viewport_state = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                    .viewportCount = 1,
                    .scissorCount = 1,
                };
                VkPipelineRasterizationStateCreateInfo rasterizer = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                    .depthClampEnable = VK_TRUE,
                    .rasterizerDiscardEnable = VK_FALSE,
                    .polygonMode = VK_POLYGON_MODE_FILL,
                    .lineWidth = 1.0f,
                    .frontFace = VK_FRONT_FACE_CLOCKWISE,
                    .cullMode = VK_CULL_MODE_NONE,
                };
                VkPipelineMultisampleStateCreateInfo multisampling = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                    .sampleShadingEnable = VK_FALSE,
                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                };
                VkPipelineDepthStencilStateCreateInfo depth_stencil = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                };
                VkPipelineColorBlendAttachmentState cb_att = {
                    .colorWriteMask = 0,
                };
                VkPipelineColorBlendStateCreateInfo color_blending = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                    .logicOpEnable = VK_FALSE,
                    .logicOp = VK_LOGIC_OP_COPY,
                    .attachmentCount =
                        rps.color_format != VK_FORMAT_UNDEFINED ? 1 : 0,
                    .pAttachments =
                        rps.color_format != VK_FORMAT_UNDEFINED ? &cb_att :
                                                                  NULL,
                };

                VkDynamicState dyn[21] = { VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR };
                int ndyn = 2;
                dyn[ndyn++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
                bool line_ish =
                    (r->enabled_physical_device_features.wideLines ==
                     VK_TRUE) &&
                    (combos[k].front_mode == POLY_MODE_LINE ||
                     combos[k].prim == PRIM_TYPE_LINES ||
                     combos[k].prim == PRIM_TYPE_LINE_LOOP ||
                     combos[k].prim == PRIM_TYPE_LINE_STRIP);
                if (line_ish) {
                    dyn[ndyn++] = VK_DYNAMIC_STATE_LINE_WIDTH;
                }
                dyn[ndyn++] = VK_DYNAMIC_STATE_CULL_MODE_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_FRONT_FACE_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_STENCIL_OP_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
                dyn[ndyn++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
                dyn[ndyn++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
                dyn[ndyn++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
                dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
                dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
                VkPipelineDynamicStateCreateInfo dynamic_state = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                    .dynamicStateCount = ndyn,
                    .pDynamicStates = dyn,
                };

                VkDescriptorSetLayout set_layouts[3] = {
                    r->descriptor_set_layout,
                    r->uber_tex_descriptor_set_layout,
                    r->uber_b_set_layout,
                };
                VkPushConstantRange push_range = {
                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                    .offset = 0,
                    .size = sizeof(uint32_t),
                };
                VkPipelineLayoutCreateInfo layout_info = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                    .setLayoutCount = r->uber_batched ? 3 : 2,
                    .pSetLayouts = set_layouts,
                    .pushConstantRangeCount = r->uber_batched ? 1 : 0,
                    .pPushConstantRanges = &push_range,
                };
                VkPipelineLayout layout;
                VK_CHECK(vkCreatePipelineLayout(r->device, &layout_info,
                                                NULL, &layout));

                VkGraphicsPipelineCreateInfo info = {
                    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                    .stageCount = nstages,
                    .pStages = stages,
                    .pVertexInputState = NULL,
                    .pInputAssemblyState = &input_assembly,
                    .pViewportState = &viewport_state,
                    .pRasterizationState = &rasterizer,
                    .pMultisampleState = &multisampling,
                    .pDepthStencilState =
                        rps.zeta_format != VK_FORMAT_UNDEFINED ?
                            &depth_stencil : NULL,
                    .pColorBlendState = &color_blending,
                    .pDynamicState = &dynamic_state,
                    .layout = layout,
                    .renderPass = rpass,
                    .subpass = 0,
                };
                VkPipeline pipeline;
                VK_CHECK(vkCreateGraphicsPipelines(r->device,
                                                   r->vk_pipeline_cache, 1,
                                                   &info, NULL, &pipeline));
                snode->layout = layout;
                snode->pipeline = pipeline;
                snode->render_pass = rpass;
                snode->draw_time = 0;
                snode->has_dynamic_line_width = line_ish;
            }
        }
    }
}

static void create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_bind_textures(d);
    pgraph_vk_bind_shaders(pg);

    /* one-shot, after the uber modules exist */
    if (r->uber_active && !r->uber_prebaked) {
        r->uber_prebaked = true;
        uber_prebake_pipelines(pg);
    }

    // FIXME: If nothing was dirty, don't even try creating the key or hashing.
    //        Just use the same pipeline.
    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    // FIXME: We could clear less

    if (r->pipeline_binding && !pipeline_dirty) {
        NV2A_VK_DPRINTF("Cache hit");
        NV2A_VK_DGROUP_END();
        return;
    }

    PipelineKey key_storage;
    PipelineKey *keyp;
    if (r->uber_tiny_pipeline_key) {
        if (!r->uber_key_template_init) {
            memset(&r->uber_key_template, 0, sizeof(r->uber_key_template));
            r->uber_key_template_init = true;
        }
        keyp = &r->uber_key_template;
    } else {
        memset(&key_storage, 0, sizeof(key_storage));
        keyp = &key_storage;
    }
    init_pipeline_key(pg, keyp);
    uint64_t hash = pipeline_key_hash(r, keyp);

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, keyp);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    memcpy(&snode->key, keyp, sizeof(*keyp));

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vsh.module_info->module,
            .pName = "main",
            .pSpecializationInfo = NULL,
        };
    if (r->shader_binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->psh.module_info->module,
            .pName = "main",
            .pSpecializationInfo = NULL,
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = get_primitive_topology(pg),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    bool uber_eds = r->uber_active && r->extended_dynamic_state_enabled;
    bool uber_eds3 = r->uber_active && r->extended_dynamic_state3_enabled;

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = (uber_eds3 && r->eds3_polygon_mode) ? VK_POLYGON_MODE_FILL :
            pgraph_polygon_mode_vk_map[r->shader_binding->state
                                          .geom.polygon_front_mode],
        .lineWidth = 1.0f,
        .frontFace = uber_eds ? VK_FRONT_FACE_CLOCKWISE :
            ((pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
              NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                 VK_FRONT_FACE_COUNTER_CLOCKWISE :
                 VK_FRONT_FACE_CLOCKWISE),
        .cullMode = VK_CULL_MODE_NONE,
        .depthBiasEnable = VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

    bool uber_line_mode = r->uber_active &&
        r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE;
    if (!uber_eds) {
        if (!uber_line_mode &&
            (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
            uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                          NV_PGRAPH_SETUPRASTER_CULLCTRL);
            assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
            rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
        }
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    if (!uber_eds) {
        depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;

        if (depth_test) {
            depth_stencil.depthTestEnable = VK_TRUE;
            uint32_t depth_func =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), NV_PGRAPH_CONTROL_0_ZFUNC);
            assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
            depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
        }

        if (stencil_test) {
            depth_stencil.stencilTestEnable = VK_TRUE;
            uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                             NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
            uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                            NV_PGRAPH_CONTROL_1_STENCIL_REF);
            uint32_t mask_read = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                          NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t mask_write = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
            uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                        NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

            assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
            assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

            depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
            depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
            depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
            depth_stencil.front.compareOp =
                pgraph_stencil_func_vk_map[stencil_func];
            depth_stencil.front.compareMask = mask_read;
            depth_stencil.front.writeMask = mask_write;
            depth_stencil.front.reference = stencil_ref;
            depth_stencil.back = depth_stencil.front;
        }
    }

    VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };
    float blend_constant[4] = { 0, 0, 0, 0 };

    if (!uber_eds3) {
        write_mask = 0;
        if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_R_BIT;
        if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_G_BIT;
        if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_B_BIT;
        if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_A_BIT;
        color_blend_attachment.colorWriteMask = write_mask;

        if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
            color_blend_attachment.blendEnable = VK_TRUE;

            uint32_t sfactor =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_SFACTOR);
            uint32_t dfactor =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_DFACTOR);
            assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
            assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
            color_blend_attachment.srcColorBlendFactor =
                pgraph_blend_factor_vk_map[sfactor];
            color_blend_attachment.dstColorBlendFactor =
                pgraph_blend_factor_vk_map[dfactor];
            color_blend_attachment.srcAlphaBlendFactor =
                pgraph_blend_factor_vk_map[sfactor];
            color_blend_attachment.dstAlphaBlendFactor =
                pgraph_blend_factor_vk_map[dfactor];

            uint32_t equation =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_EQN);
            assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

            color_blend_attachment.colorBlendOp =
                pgraph_blend_equation_vk_map[equation];
            color_blend_attachment.alphaBlendOp =
                pgraph_blend_equation_vk_map[equation];

            if (!uber_eds) {
                uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
                pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
            }
        }
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
        .blendConstants[0] = blend_constant[0],
        .blendConstants[1] = blend_constant[1],
        .blendConstants[2] = blend_constant[2],
        .blendConstants[3] = blend_constant[3],
    };

    VkDynamicState dynamic_states[21] = { VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR };
    int num_dynamic_states = 2;
    bool uber_vids = r->uber_active && r->vertex_input_dynamic_state_enabled;
    if (uber_vids) {
        dynamic_states[num_dynamic_states++] =
            VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    }

    snode->has_dynamic_line_width =
        (r->enabled_physical_device_features.wideLines == VK_TRUE) &&
        (r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINES ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_LOOP ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_STRIP);
    if (snode->has_dynamic_line_width) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }

    if (uber_eds) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_CULL_MODE_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_FRONT_FACE_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_OP_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    }

    if (uber_eds3) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
        if (!uber_eds) {
            dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
        }
        if (r->eds3_polygon_mode) {
            dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
        }
    }

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = num_dynamic_states,
        .pDynamicStates = dynamic_states,
    };

    // FIXME: Dither
    // if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    /* uber uses two sets (0 = UBOs, 1 = textures).
     * Batched mode adds set 2 + the record-base push constant. */
    VkDescriptorSetLayout set_layouts[3] = {
        r->descriptor_set_layout,
        r->uber_tex_descriptor_set_layout,
        r->uber_b_set_layout,
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = r->uber_active ? (r->uber_batched ? 3 : 2) : 1,
        .pSetLayouts = set_layouts,
    };
    /* the record base rides a single push uint (VSH adds
     * gl_DrawID); the PSH reads the record SSBO. */
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(uint32_t),
    };
    if (r->uber_batched) {
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
    }

    VkPushConstantRange push_constant_range;
    /* the uber VSH never declares a push block (inline values
     * arrive via the inlineValue UBO member) - the range only varied the
     * layout per draw (and uniform_attrs is not in the uber key: a latent
     * range-mismatch). Uber layouts now carry no push range = constant. */
    if (r->use_push_constants_for_uniform_attrs && !r->uber_active) {
        int num_uniform_attributes =
            __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs);
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = uber_vids ? NULL : &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &keyp->render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->use_push_constants_for_uniform_attrs || r->uber_active) {
        /* uber consumes inline values via the UBO - the
         * per-draw get_inline_values + vkCmdPushConstants were dead weight
         * (pushed bytes the shader never read). */
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

/* slot manager. Slots live on the TextureBinding (identity
 * = the binding; sampler params are part of its cache key). Freed slots
 * recycle only at finish (GPU fully idle), so a pending submit can never
 * see a slot rewritten under it. */
static int uber_b_type_of(const TextureBinding *tb)
{
    if (tb->key.state.cubemap) {
        return 1;
    }
    if (tb->key.state.dimensionality == 3) {
        return 2;
    }
    return 0;
}

static int uber_b_slot_assign(PGRAPHVkState *r, TextureBinding *tb,
                              bool uint_slot)
{
    int16_t *slotp = uint_slot ? &tb->uber_slot_uint : &tb->uber_slot;
    if (*slotp >= 0) {
        return *slotp;
    }
    int type = uint_slot ? 3 : uber_b_type_of(tb);
    assert(r->uber_b_free_top[type] > 0);
    uint16_t slot = r->uber_b_free[type][--r->uber_b_free_top[type]];

    VkDescriptorImageInfo img = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = tb->image_view,
        .sampler = tb->sampler,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = r->uber_b_set,
        .dstBinding = 1 + type,
        .dstArrayElement = slot,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &img,
    };
    vkUpdateDescriptorSets(r->device, 1, &w, 0, NULL);
    *slotp = (int16_t)slot;
    return slot;
}

void pgraph_vk_uber_b_release_slots(PGRAPHVkState *r, TextureBinding *tb)
{
    if (!r->uber_batched) {
        return;
    }
    if (tb->uber_slot >= 0) {
        assert(r->uber_b_npending < (int)ARRAY_SIZE(r->uber_b_pending));
        r->uber_b_pending[r->uber_b_npending++] = (typeof(r->uber_b_pending[0])){
            .slot = (uint16_t)tb->uber_slot,
            .type = (uint8_t)uber_b_type_of(tb),
            .seq = r->uber_b_finish_seq,
        };
        tb->uber_slot = -1;
    }
    if (tb->uber_slot_uint >= 0) {
        assert(r->uber_b_npending < (int)ARRAY_SIZE(r->uber_b_pending));
        r->uber_b_pending[r->uber_b_npending++] = (typeof(r->uber_b_pending[0])){
            .slot = (uint16_t)tb->uber_slot_uint,
            .type = 3,
            .seq = r->uber_b_finish_seq,
        };
        tb->uber_slot_uint = -1;
    }
}

/* Build this flush's packed record and append one copy per range.
 * Returns the base record index for the push constant. */
static uint32_t uber_b_append_records(PGRAPHState *pg, int num_ranges)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint32_t need = (uint32_t)num_ranges * 16u;
    /* Capacity is reserved up-front in pgraph_vk_flush_draw (a finish here
     * would fire inside in_draw). */
    assert(r->uber_b_rec_wp + need <= r->uber_b_rec_capacity);
    uint32_t rec[4];
    for (int i = 0; i < 4; i++) {
        TextureBinding *tb = r->texture_bindings[i];
        uint32_t s = (uint32_t)uber_b_slot_assign(r, tb, false);
        bool is_x8y24 =
            tb->key.state.color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
            tb->key.state.color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT;
        uint32_t su = is_x8y24 ?
            (uint32_t)uber_b_slot_assign(r, tb, true) : 0u;
        rec[i] = (s & 0xFFFFu) | (su << 16);
    }
    uint32_t base = r->uber_b_rec_wp / 16u;
    uint8_t *dst = r->uber_b_rec_mapped + r->uber_b_rec_wp;
    for (int n = 0; n < num_ranges; n++) {
        memcpy(dst + (size_t)n * 16u, rec, 16u);
    }
    r->uber_b_rec_wp += need;
    return base;
}

/* every uber draw-encode path must append+push its record —
 * a draw without one reads a stale uber_rec_base (the inline_elements
 * lesson: world geometry sampled garbage slots while the draw_arrays-only
 * synthetic gate stayed green). */
static void uber_b_push_records(PGRAPHState *pg, int num_ranges)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r->uber_batched) {
        return;
    }
    uint32_t base = uber_b_append_records(pg, num_ranges);
    vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(base), &base);
}

/* encode the pending merged batch — one push (the
 * batch's first record index) + one vkCmdDrawMultiEXT. Called from every
 * encode site that would disturb the batch (correct by construction: a
 * state command between accumulated draws would otherwise apply to ALL of
 * them). Must run inside the render pass the draws were accumulated in. */
static void uber_m_flush(PGRAPHVkState *r)
{
    if (!r->uber_m_count) {
        return;
    }
    assert(r->in_render_pass);
    assert(r->pipeline_binding);
    vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(r->uber_m_base),
                       &r->uber_m_base);
    vkCmdDrawMultiEXT(r->command_buffer, r->uber_m_count, r->uber_m_infos, 1,
                      0, sizeof(VkMultiDrawInfoEXT));
    r->uber_m_count = 0;
    r->uber_m_nflushes = 0;
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= 1);

    if (r->uber_active) {
        /* set 0 = UBOs, set 1 = textures.
         * Static descriptor offsets. */
        assert(r->uber_tex_descriptor_set_index >= 1);
        VkDescriptorSet sets[3] = {
            r->descriptor_sets[r->descriptor_set_index - 1],
            r->uber_tex_descriptor_sets[r->uber_tex_descriptor_set_index - 1],
            r->uber_b_set,
        };
        if (r->uber_batched) {
            /* identical handles (static offsets; set handles
             * move only when descriptor writes happened) = the bind is a
             * no-op — skip it so clean flushes encode nothing. */
            if (r->uber_m_bds_valid &&
                memcmp(sets, r->uber_m_bds_sets, sizeof(sets)) == 0) {
                return;
            }
            uber_m_flush(r);
            memcpy(r->uber_m_bds_sets, sets, sizeof(sets));
            r->uber_m_bds_valid = true;
        }
        vkCmdBindDescriptorSets(r->command_buffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                r->pipeline_binding->layout, 0,
                                r->uber_batched ? 3 : 2, sets, 0, NULL);
        return;
    }

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            &r->descriptor_sets[r->descriptor_set_index - 1], 0,
                            NULL);
}

static void begin_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    assert(r->num_queries_in_flight < r->max_queries_in_flight);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        r->num_queries_in_flight, 1);
    vkCmdBeginQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight,
                    VK_QUERY_CONTROL_PRECISE_BIT);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(r->query_in_flight);

    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    /* delta form — copy only bytes staged since the last
     * submit this window. Offsets append until the post-wait reset, so an
     * in-flight chunk's source/destination ranges are never touched.
     * Degenerates to the classic [0, offset) copy when no chunks fired. */
    if (b_src->buffer_offset <= b_src->synced_offset) {
        return;
    }

    VkBufferCopy copy_region = {
        .srcOffset = b_src->synced_offset,
        .dstOffset = b_src->synced_offset,
        .size = b_src->buffer_offset - b_src->synced_offset,
    };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .offset = copy_region.dstOffset,
        .size = copy_region.size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);

    /* The offset reset moves to the post-wait tail of pgraph_vk_finish
     * (chunked submission); within the window both only grow. */
    b_src->synced_offset = b_src->buffer_offset;
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_CHECK(vmaFlushAllocation(
        r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation, 0,
        VK_WHOLE_SIZE));

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    assert(r->framebuffer_index > 0);

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;
    r->eds1_cache.valid = false;
    r->eds3_cache.valid = false;
    r->vids_cache_valid = false;
    r->eds1_emit.valid = false;
    r->eds3_emit.valid = false;
}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        /* the chokepoint — queries/clears/surfaces/nondraw/finish all
         * route through here */
        uber_m_flush(r);
        vkCmdEndRenderPass(r->command_buffer);
        r->in_render_pass = false;
    }
}

/* chunked submission — submit the open (main, aux) pair
 * WITHOUT waiting and continue recording on the next pair, so the GPU
 * executes mid-window work while the CPU keeps building. The real finish's
 * fence covers every chunk (same-queue signal order). Query brackets split
 * at the boundary exactly like the render-pass dance (results are summed —
 * reports.c — so splitting is result-identical). Staging offsets append
 * across chunks (delta copies); the uploaded_bitmap is not cleared (the
 * guest-overwrite guard must span the whole window). */
static void chunk_soft_submit(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_draw);
    if (r->in_aux_command_buffer || r->cb_pair_index >= VK_CB_NPAIRS - 1) {
        return; /* mid-upload or out of pairs: ride to the finish */
    }

    end_render_pass(r);           /* the hook drains the batch */
    assert(r->uber_m_count == 0);
    if (r->query_in_flight) {
        end_query(r);
    }
    VK_CHECK(vkEndCommandBuffer(r->command_buffer));

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
    sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                        BUFFER_VERTEX_INLINE);
    sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM);
    flush_memory_buffer(pg, cmd);
    VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
    r->in_aux_command_buffer = false;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore pair_sem = r->pair_semaphores[r->cb_pair_index];
    VkSubmitInfo submit_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &r->aux_command_buffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &pair_sem,
        },
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &r->command_buffer,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pair_sem,
            .pWaitDstStageMask = &wait_stage,
        }
    };
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
    VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                           VK_NULL_HANDLE));
    r->submit_count += 1;

    r->cb_pair_index++;
    r->command_buffer = r->command_buffers[2 * r->cb_pair_index];
    r->aux_command_buffer = r->command_buffers[2 * r->cb_pair_index + 1];
    r->in_command_buffer = false;
    /* command_buffer_start_time gates "was this surface/pipeline drawn since
     * the last FENCE WAIT" (download/present/evict safety). A chunk submit
     * is not a wait — keep the window start across chunks or those checks
     * would read surfaces the GPU is still writing (the N=1024 parity
     * break: stale render-target downloads). */
    int64_t chunk_window_start = r->command_buffer_start_time;
    pgraph_vk_begin_command_buffer(pg); /* invalidates the bind shadows */
    r->command_buffer_start_time = chunk_window_start;
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
};

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool was_batched = r->uber_batched;

    assert(!r->in_draw);
    assert(r->debug_depth == 0);

    if (r->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        if (r->in_render_pass) {
            end_render_pass(r);
        }
        assert(r->uber_m_count == 0);
        if (r->query_in_flight) {
            end_query(r);
        }
        VK_CHECK(vkEndCommandBuffer(r->command_buffer));

        VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg); // FIXME: Cleanup
        sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
        sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                                BUFFER_VERTEX_INLINE);
        sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM);
        bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
        flush_memory_buffer(pg, cmd);
        VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));
        r->in_aux_command_buffer = false;

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSemaphore pair_sem = r->pair_semaphores[r->cb_pair_index];
        VkSubmitInfo submit_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->aux_command_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &pair_sem,
            },
            {

                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->command_buffer,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &pair_sem,
                .pWaitDstStageMask = &wait_stage,
            }
        };
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        vkResetFences(r->device, 1, &r->command_buffer_fence);
        VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                               r->command_buffer_fence));
        r->submit_count += 1;

        bool check_budget = false;

        // Periodically check memory budget
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {

            // VMA queries budget via vmaSetCurrentFrameIndex
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            check_budget = true;
        }

        VK_CHECK(vkWaitForFences(r->device, 1, &r->command_buffer_fence,
                                 VK_TRUE, UINT64_MAX));

        /* GPU is now drained, so the in-flight draws are done
         * reading the 1:1 mirror — copy the streamed (new) bytes back into it so
         * post-finish draws see the latest, then reset the ring for the next
         * epoch. (Every finish path routes through here.) */
        /* The old finish-time copyback (ring->mirror) is REMOVED.
         * The 1:1 mirror is now kept current by the per-stream update_memory_buffer
         * (vertex.c); the copyback re-wrote stale stream-time bytes over that
         * refreshed mirror and corrupted Stencil_REPLACE_ST. Just clear the remap
         * list (the ring is reset below for the next epoch). */
        r->vertex_stream_remap_count = 0;
        r->vertex_stream_wp = 0;
        r->vstream_finish_gen++; /* invalidate bound-but-unrecorded
                               * ring offsets (consume site retargets them) */

        r->descriptor_set_index = 0;
        r->uber_tex_descriptor_set_index = 0;
        /* the GPU is drained — every pending slot free and
         * every in-flight record is past; recycle and reset the ring. */
        if (was_batched) {
            for (int k = 0; k < r->uber_b_npending; k++) {
                int t = r->uber_b_pending[k].type;
                assert(r->uber_b_free_top[t] < UBER_B_SLOTS);
                r->uber_b_free[t][r->uber_b_free_top[t]++] =
                    r->uber_b_pending[k].slot;
            }
            r->uber_b_npending = 0;
            r->uber_b_rec_wp = 0;
            r->uber_b_finish_seq++;
        }
        /* the fence covers every chunk (same-queue signal
         * order) — recycle the pair pool and restart the staging windows. */
        r->cb_pair_index = 0;
        r->command_buffer = r->command_buffers[0];
        r->aux_command_buffer = r->command_buffers[1];
        r->chunk_flushes_since = 0;
        for (int sbi = 0; sbi < ARRAY_SIZE(r->storage_buffers); sbi++) {
            r->storage_buffers[sbi].synced_offset = 0;
        }
        r->storage_buffers[BUFFER_INDEX_STAGING].buffer_offset = 0;
        r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING].buffer_offset = 0;
        r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset = 0;
        r->in_command_buffer = false;
        destroy_framebuffers(pg);

        if (check_budget) {
            pgraph_vk_check_memory_budget(pg);
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_vk_process_pending_reports_internal(d);

    pgraph_vk_compute_finish_complete(r);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(!r->in_command_buffer);

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
    /* bind state does not carry across command
     * buffers. The batch itself cannot carry either (asserted in finish). */
    assert(r->uber_m_count == 0);
    r->uber_m_bds_valid = false;
    r->uber_m_vb_valid = false;
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass(r);
    if (r->query_in_flight) {
        end_query(r);
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static void begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        create_pipeline(pg);
    }

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (!pg->clearing) {
        pgraph_vk_update_descriptor_sets(pg);
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);

    // Visibility testing
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (r->new_query_needed && r->query_in_flight) {
            end_render_pass(r);
            end_query(r);
        }
        if (!r->query_in_flight) {
            end_render_pass(r);
            begin_query(r);
        }
    } else if (r->query_in_flight) {
        end_render_pass(r);
        end_query(r);
    }

    if (pg->clearing) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;
    r->pipeline_binding_changed = false;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }

    if (must_bind_pipeline) {
        uber_m_flush(r);
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        VkViewport viewport = {
            .width = vp_width,
            .height = vp_height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);

        if (r->pipeline_binding->has_dynamic_line_width) {
            float line_width =
                clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
            vkCmdSetLineWidth(r->command_buffer, line_width);
        }
    }

    if (r->uber_active && r->extended_dynamic_state_enabled) {
        uint32_t c0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
        uint32_t c1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
        uint32_t c2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
        uint32_t sr = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
        uint32_t bc = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);

        bool eds1_dirty = !r->eds1_cache.valid ||
            c0 != r->eds1_cache.control_0 ||
            c1 != r->eds1_cache.control_1 ||
            c2 != r->eds1_cache.control_2 ||
            sr != r->eds1_cache.setupraster ||
            bc != r->eds1_cache.blendcolor;

        if (eds1_dirty) {
            uber_m_flush(r);
            r->eds1_cache.control_0 = c0;
            r->eds1_cache.control_1 = c1;
            r->eds1_cache.control_2 = c2;
            r->eds1_cache.setupraster = sr;
            r->eds1_cache.blendcolor = bc;
            r->eds1_cache.valid = true;

            bool depth_test = c0 & NV_PGRAPH_CONTROL_0_ZENABLE;
            bool depth_write = !!(c0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);

            /* emit only moved values (the dirty signal is a
             * 5-reg-word compare; one bit flipping re-emitted the full
             * cluster — ~12 calls). */
            bool force_all = !r->eds1_emit.valid;
            if (force_all || r->eds1_emit.dt != depth_test) {
                vkCmdSetDepthTestEnableEXT(r->command_buffer, depth_test);
                r->eds1_emit.dt = depth_test;
            }
            if (force_all || r->eds1_emit.dw != depth_write) {
                vkCmdSetDepthWriteEnableEXT(r->command_buffer, depth_write);
                r->eds1_emit.dw = depth_write;
            }
            VkCompareOp depth_compare_op = VK_COMPARE_OP_ALWAYS;
            if (depth_test) {
                uint32_t depth_func = GET_MASK(c0, NV_PGRAPH_CONTROL_0_ZFUNC);
                assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
                depth_compare_op = pgraph_depth_func_vk_map[depth_func];
            }
            if (force_all || r->eds1_emit.dop != depth_compare_op) {
                vkCmdSetDepthCompareOpEXT(r->command_buffer, depth_compare_op);
                r->eds1_emit.dop = depth_compare_op;
            }

            bool stencil_test = c1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

            uint32_t stencil_ref = GET_MASK(c1, NV_PGRAPH_CONTROL_1_STENCIL_REF);
            uint32_t mask_read = GET_MASK(c1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t mask_write = GET_MASK(c1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
            uint32_t op_fail = GET_MASK(c2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            uint32_t op_zfail = GET_MASK(c2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            uint32_t op_zpass = GET_MASK(c2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

            assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
            assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

            /* conditional (was hardcoded VK_TRUE) — a stencil-
             * disabled draw must not enable the test; the baked path leaves it
             * false. Forcing true + register ops below corrupted the stencil
             * buffer for the later stencil-reading shadow draw. */
            if (force_all || r->eds1_emit.st_en != stencil_test) {
                vkCmdSetStencilTestEnableEXT(r->command_buffer,
                                             stencil_test ? VK_TRUE : VK_FALSE);
                r->eds1_emit.st_en = stencil_test;
            }

            uint32_t stencil_ops_key;
            if (stencil_test) {
                uint32_t stencil_func = GET_MASK(c1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
                assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
                stencil_ops_key = (op_fail << 24) | (op_zpass << 16) |
                               (op_zfail << 8) | stencil_func;
                if (force_all || r->eds1_emit.st_ops_key != stencil_ops_key) {
                    vkCmdSetStencilOpEXT(r->command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK,
                                         pgraph_stencil_op_vk_map[op_fail],
                                         pgraph_stencil_op_vk_map[op_zpass],
                                         pgraph_stencil_op_vk_map[op_zfail],
                                         pgraph_stencil_func_vk_map[stencil_func]);
                    r->eds1_emit.st_ops_key = stencil_ops_key;
                }
            } else {
                /* KEEP ops (not the register ops) when stencil
                 * disabled — match the baked path, never write the stencil
                 * buffer. (Belt-and-suspenders with the false enable above.) */
                stencil_ops_key = 0xFFFFFFFFu; /* the KEEP tuple */
                if (force_all || r->eds1_emit.st_ops_key != stencil_ops_key) {
                    vkCmdSetStencilOpEXT(r->command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK,
                                         VK_STENCIL_OP_KEEP,
                                         VK_STENCIL_OP_KEEP,
                                         VK_STENCIL_OP_KEEP,
                                         VK_COMPARE_OP_ALWAYS);
                    r->eds1_emit.st_ops_key = stencil_ops_key;
                }
            }
            if (force_all || r->eds1_emit.cmask != mask_read) {
                vkCmdSetStencilCompareMask(r->command_buffer,
                                           VK_STENCIL_FACE_FRONT_AND_BACK, mask_read);
                r->eds1_emit.cmask = mask_read;
            }
            if (force_all || r->eds1_emit.wmask != mask_write) {
                vkCmdSetStencilWriteMask(r->command_buffer,
                                         VK_STENCIL_FACE_FRONT_AND_BACK, mask_write);
                r->eds1_emit.wmask = mask_write;
            }
            if (force_all || r->eds1_emit.ref != stencil_ref) {
                vkCmdSetStencilReference(r->command_buffer,
                                         VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
                r->eds1_emit.ref = stencil_ref;
            }

            /* shader_binding is NULL on the first clear -
             * this deref was the crash that got EDS hardcoded off. */
            bool uber_line_mode_eds = r->uber_active && r->shader_binding &&
                r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE;
            VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
            if (!uber_line_mode_eds &&
                (sr & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
                uint32_t cull_face = GET_MASK(sr, NV_PGRAPH_SETUPRASTER_CULLCTRL);
                assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
                cull_mode = pgraph_cull_face_vk_map[cull_face];
            }
            if (force_all || r->eds1_emit.cull != cull_mode) {
                vkCmdSetCullModeEXT(r->command_buffer, cull_mode);
                r->eds1_emit.cull = cull_mode;
            }
            VkFrontFace front_face = (sr & NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
            if (force_all || r->eds1_emit.ff != front_face) {
                vkCmdSetFrontFaceEXT(r->command_buffer, front_face);
                r->eds1_emit.ff = front_face;
            }

            if (!r->extended_dynamic_state3_enabled) {
                if (force_all || r->eds1_emit.bc_packed != bc) {
                    float blend_constant[4];
                    pgraph_argb_pack32_to_rgba_float(bc, blend_constant);
                    vkCmdSetBlendConstants(r->command_buffer, blend_constant);
                    r->eds1_emit.bc_packed = bc;
                }
            }
            r->eds1_emit.valid = true;
        }
    }

    if (r->uber_active && r->extended_dynamic_state3_enabled && !pg->clearing) {
        uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
        uint32_t blend_reg_c = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
        uint32_t blend_color_c = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        int pm_c = -1;
        if (r->eds3_polygon_mode) {
            pm_c = pgraph_polygon_mode_vk_map[
                r->shader_binding->state.geom.polygon_front_mode];
            if (r->shader_binding == &r->uber_shader_binding_quad_wireframe) {
                pm_c = VK_POLYGON_MODE_FILL;
            }
        }
        /* skip the 4-5 vkCmdSet* when nothing changed since
         * the last draw in this render pass (mirror of the EDS1 cache). */
        if (r->eds3_cache.valid && r->eds3_cache.control_0 == control_0 &&
            r->eds3_cache.blend == blend_reg_c &&
            r->eds3_cache.blendcolor == blend_color_c &&
            r->eds3_cache.pm == pm_c) {
            goto eds3_done;
        }
        uber_m_flush(r);
        r->eds3_cache.control_0 = control_0;
        r->eds3_cache.blend = blend_reg_c;
        r->eds3_cache.blendcolor = blend_color_c;
        r->eds3_cache.pm = pm_c;
        r->eds3_cache.valid = true;

        bool eds3_force = !r->eds3_emit.valid;
        VkColorComponentFlags write_mask = 0;
        if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_R_BIT;
        if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_G_BIT;
        if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_B_BIT;
        if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
            write_mask |= VK_COLOR_COMPONENT_A_BIT;
        if (eds3_force || r->eds3_emit.wm != write_mask) {
            vkCmdSetColorWriteMaskEXT(r->command_buffer, 0, 1, &write_mask);
            r->eds3_emit.wm = write_mask;
        }

        uint32_t blend_reg = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
        VkBool32 blend_en = (blend_reg & NV_PGRAPH_BLEND_EN)
                            ? VK_TRUE : VK_FALSE;
        if (eds3_force || r->eds3_emit.be != blend_en) {
            vkCmdSetColorBlendEnableEXT(r->command_buffer, 0, 1, &blend_en);
            r->eds3_emit.be = blend_en;
        }

        uint32_t sfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_DFACTOR);
        uint32_t equation = GET_MASK(blend_reg, NV_PGRAPH_BLEND_EQN);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        uint32_t blend_eq_key = (sfactor << 16) | (dfactor << 8) | equation;
        if (eds3_force || r->eds3_emit.eq_key != blend_eq_key) {
            VkColorBlendEquationEXT eq = {
                .srcColorBlendFactor = pgraph_blend_factor_vk_map[sfactor],
                .dstColorBlendFactor = pgraph_blend_factor_vk_map[dfactor],
                .colorBlendOp = pgraph_blend_equation_vk_map[equation],
                .srcAlphaBlendFactor = pgraph_blend_factor_vk_map[sfactor],
                .dstAlphaBlendFactor = pgraph_blend_factor_vk_map[dfactor],
                .alphaBlendOp = pgraph_blend_equation_vk_map[equation],
            };
            vkCmdSetColorBlendEquationEXT(r->command_buffer, 0, 1, &eq);
            r->eds3_emit.eq_key = blend_eq_key;
        }

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        if (eds3_force || r->eds3_emit.bc_packed != blend_color) {
            float blend_constant[4];
            pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
            vkCmdSetBlendConstants(r->command_buffer, blend_constant);
            r->eds3_emit.bc_packed = blend_color;
        }

        if (r->eds3_polygon_mode) {
            VkPolygonMode pm = pgraph_polygon_mode_vk_map[
                r->shader_binding->state.geom.polygon_front_mode];
            if (r->shader_binding == &r->uber_shader_binding_quad_wireframe) {
                pm = VK_POLYGON_MODE_FILL;
            }
            if (eds3_force || r->eds3_emit.pm != (int)pm) {
                vkCmdSetPolygonModeEXT(r->command_buffer, pm);
                r->eds3_emit.pm = (int)pm;
            }
        }
        r->eds3_emit.valid = true;
    eds3_done:;
    }

    /* vertex input rides dynamic state; (re)set per render
     * pass or when the active descriptions changed (the changed-path is the
     * win: a format change costs one extension call, not a pipeline). */
    if (r->uber_active && r->vertex_input_dynamic_state_enabled &&
        !pg->clearing) {
        if (!r->vids_cache_valid) {
            uber_m_flush(r);
            VkVertexInputBindingDescription2EXT b2[NV2A_VERTEXSHADER_ATTRIBUTES];
            VkVertexInputAttributeDescription2EXT a2[NV2A_VERTEXSHADER_ATTRIBUTES];
            for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
                b2[i] = (VkVertexInputBindingDescription2EXT){
                    .sType =
                        VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
                    .binding = r->vertex_binding_descriptions[i].binding,
                    .stride = r->vertex_binding_descriptions[i].stride,
                    .inputRate = r->vertex_binding_descriptions[i].inputRate,
                    .divisor = 1,
                };
            }
            for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
                a2[i] = (VkVertexInputAttributeDescription2EXT){
                    .sType =
                        VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                    .location = r->vertex_attribute_descriptions[i].location,
                    .binding = r->vertex_attribute_descriptions[i].binding,
                    .format = r->vertex_attribute_descriptions[i].format,
                    .offset = r->vertex_attribute_descriptions[i].offset,
                };
            }
            vkCmdSetVertexInputEXT(r->command_buffer,
                                   r->num_active_vertex_binding_descriptions,
                                   b2,
                                   r->num_active_vertex_attribute_descriptions,
                                   a2);
            r->vids_cache_valid = true;
        }
    }

    if (!pg->clearing) {
        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
    }

    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

    pgraph_vk_flush_draw(d);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size, start_addr,
                        end_addr - start_addr);

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            pgraph_vk_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                               size);
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    int n = r->num_active_vertex_binding_descriptions;
    for (int i = 0; i < n; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
        /* a finish ran between this attr's stream and now — its
         * ring entry is dead (copyback moved the bytes into the mirror;
         * later streams will overwrite the ring). Read the mirror instead.
         * Entries streamed after the finish are live and keep the ring. */
        if ((r->vstream_mask & (1u << attr_idx)) &&
            !(inline_map & (1u << attr_idx)) &&
            r->vstream_gen[attr_idx] != r->vstream_finish_gen) {
            offsets[i] = offset + r->vstream_mirror_offset[attr_idx];
        }
    }

    if (r->uber_batched) { /* skip identical rebinds */
        if (r->uber_m_vb_valid && r->uber_m_vb_n == (uint32_t)n &&
            memcmp(r->uber_m_vb_buffers, buffers, n * sizeof(buffers[0])) == 0 &&
            memcmp(r->uber_m_vb_offsets, offsets, n * sizeof(offsets[0])) == 0) {
            return;
        }
        uber_m_flush(r);
        r->uber_m_vb_n = (uint32_t)n;
        memcpy(r->uber_m_vb_buffers, buffers, n * sizeof(buffers[0]));
        memcpy(r->uber_m_vb_offsets, offsets, n * sizeof(offsets[0]));
        r->uber_m_vb_valid = true;
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0, n, buffers, offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1)) {
        /* buffer full mid-frame -> forced flush (tracked by FINISH_NEED_BUFFER_SPACE) */
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    if (!remap.attributes) {
        return;
    }

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 256));

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // FIXME: Account for only what is drawn
    assert(start_vertex == 0);
    assert(buffer->mapped);

    // Copy vertex data
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id];

        for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            memcpy(out_ptr, in_ptr, remap.map[attr_id].new_stride);
            out_ptr += remap.map[attr_id].new_stride;
            in_ptr += remap.map[attr_id].old_stride;
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return;
    }

    if (r->uber_batched) {
        /* reserve record-ring space while a finish
         * is still legal (not in_draw). Records of in-flight draws cannot
         * be overwritten; the finish drains and resets the ring. */
        uint32_t need_words =
            (uint32_t)MAX(pg->draw_arrays_length, 1) * 16u;
        if (r->uber_b_rec_wp + need_words > r->uber_b_rec_capacity) {
            pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);

        begin_pre_draw(pg);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        if (r->uber_batched) {
            /* accumulate this flush's ranges; the
             * batch encodes at the next disturbing command (hooked) or at
             * capacity. Records are ring-contiguous between finishes, and
             * a finish flushes the batch via the end_render_pass hook. */
            if (r->uber_m_count + (uint32_t)pg->draw_arrays_length >
                    MIN(r->uber_m_cap, (uint32_t)UBER_M_CAP)) {
                uber_m_flush(r);
            }
            uint32_t slice_base =
                uber_b_append_records(pg, pg->draw_arrays_length);
            if (r->uber_m_count == 0) {
                r->uber_m_base = slice_base;
            } else {
                assert(slice_base == r->uber_m_base + r->uber_m_count);
            }
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                r->uber_m_infos[r->uber_m_count++] = (VkMultiDrawInfoEXT){
                    .firstVertex = (uint32_t)pg->draw_arrays_start[i],
                    .vertexCount = (uint32_t)pg->draw_arrays_count[i],
                };
            }
            r->uber_m_nflushes++;
            if (r->uber_m_count >= r->uber_m_cap) {
                uber_m_flush(r);
            }
        } else if (r->uber_active && r->multi_draw_enabled) {
            /* one command for the whole flush's ranges (the
             * ~18.5k ranges/frame, a couple per flush).
             * Same draws, same order — parity-trivial. */
            VkMultiDrawInfoEXT infos[ARRAY_SIZE(pg->draw_arrays_start)];
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                infos[i] = (VkMultiDrawInfoEXT){
                    .firstVertex = (uint32_t)pg->draw_arrays_start[i],
                    .vertexCount = (uint32_t)pg->draw_arrays_count[i],
                };
            }
            vkCmdDrawMultiEXT(r->command_buffer,
                              (uint32_t)pg->draw_arrays_length, infos, 1, 0,
                              sizeof(VkMultiDrawInfoEXT));
        } else {
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                uint32_t start = pg->draw_arrays_start[i],
                         count = pg->draw_arrays_count[i];
                NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
                vkCmdDraw(r->command_buffer, count, 1, start, 0);
            }
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        size_t index_data_size =
            pg->inline_elements_length * sizeof(pg->inline_elements[0]);

        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);

        begin_pre_draw(pg);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);
        VkDeviceSize buffer_offset = pgraph_vk_update_index_buffer(
            pg, pg->inline_elements, index_data_size);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        uber_m_flush(r);
        uber_b_push_records(pg, 1);
        vkCmdDrawIndexed(r->command_buffer, pg->inline_elements_length, 1, 0, 0,
                         0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);

        begin_pre_draw(pg);
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        uber_m_flush(r);
        uber_b_push_records(pg, 1);
        vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);

        begin_pre_draw(pg);
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        uber_m_flush(r);
        uber_b_push_records(pg, 1);
        vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }

    if (r->chunk_count && r->in_command_buffer &&
        ++r->chunk_flushes_since >= r->chunk_count) {
        r->chunk_flushes_since = 0;
        chunk_soft_submit(pg);
    }
}
