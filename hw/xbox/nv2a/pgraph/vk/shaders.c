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
#include "qemu/mstring.h"
#include "renderer.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include "xemu-version.h"
#include "hw/xbox/nv2a/pgraph/glsl/ubershader.h"

#include "ui/xemu-notifications.h"

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2
#define UBER_GEOM_UBO_BINDING 6
#define UBER_CUBE_TEX_BINDING 7
#define UBER_3D_TEX_BINDING 11
#define UBER_UINT_TEX_BINDING 15

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

/* the three UBER_*_LOC anonymous enums, the uber_*_cached_locs
 * static arrays and cache_uber_uniform_locs() were WRITE-only code
 * (superseded by the per-binding uber_locs caches) - deleted. */


static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_sets = ARRAY_SIZE(r->descriptor_sets);

    int num_ubos = r->uber_active ? 3 : 2;
    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = num_ubos * num_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = (r->uber_active ? 4 : 1) * NV2A_MAX_TEXTURES * num_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        /* uber allocates a second (texture) set array */
        .maxSets = (r->uber_active ? 2 : 1) * ARRAY_SIZE(r->descriptor_sets),
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2 + 4 * NV2A_MAX_TEXTURES + 1];

    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = VSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    bindings[1] = (VkDescriptorSetLayoutBinding){
        .binding = PSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    if (r->uber_active) {
        /* set 0 = UBOs only; textures live in set 1 so UBO
         * churn doesn't rewrite them.
         * Static descriptors, not
         * UNIFORM_BUFFER_DYNAMIC — the dynamic-offset indirection taxes
         * every per-fragment UBO load on NVIDIA; 3 static writes per
         * uniform-change are cheap. */
        bindings[2] = (VkDescriptorSetLayoutBinding){
            .binding = UBER_GEOM_UBO_BINDING,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_GEOMETRY_BIT,
        };
        VkDescriptorSetLayoutCreateInfo ubo_layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 3,
            .pBindings = bindings,
        };
        VK_CHECK(vkCreateDescriptorSetLayout(r->device, &ubo_layout_info,
                                             NULL, &r->descriptor_set_layout));

        int b = 0;
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            bindings[b++] = (VkDescriptorSetLayoutBinding){
                .binding = PSH_TEX_BINDING + i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            };
        }
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            bindings[b++] = (VkDescriptorSetLayoutBinding){
                .binding = UBER_CUBE_TEX_BINDING + i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            };
        }
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            bindings[b++] = (VkDescriptorSetLayoutBinding){
                .binding = UBER_3D_TEX_BINDING + i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            };
        }
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            bindings[b++] = (VkDescriptorSetLayoutBinding){
                .binding = UBER_UINT_TEX_BINDING + i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            };
        }
        VkDescriptorSetLayoutCreateInfo tex_layout_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = b,
            .pBindings = bindings,
        };
        /* flag every uber sampler binding PARTIALLY_BOUND so the
         * inactive Cube/3D slots can be left unbound (the per-draw write path
         * drops their dummies; the shader only accesses the type-matched slot).
         * Always-written slots (2D/Uint) are unaffected by the flag. */
        VkDescriptorBindingFlags tex_binding_flags[4 * NV2A_MAX_TEXTURES];
        VkDescriptorSetLayoutBindingFlagsCreateInfo tex_binding_flags_info;
        if (r->descriptor_indexing_enabled) {
            for (int i = 0; i < b; i++) {
                tex_binding_flags[i] =
                    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            }
            tex_binding_flags_info =
                (VkDescriptorSetLayoutBindingFlagsCreateInfo){
                    .sType =
                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                    .bindingCount = b,
                    .pBindingFlags = tex_binding_flags,
                };
            tex_layout_info.pNext = &tex_binding_flags_info;
        }
        VK_CHECK(vkCreateDescriptorSetLayout(
            r->device, &tex_layout_info, NULL,
            &r->uber_tex_descriptor_set_layout));
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){
            .binding = PSH_TEX_BINDING + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2 + NV2A_MAX_TEXTURES,
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
    if (r->uber_tex_descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(r->device,
                                     r->uber_tex_descriptor_set_layout, NULL);
        r->uber_tex_descriptor_set_layout = VK_NULL_HANDLE;
    }
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[ARRAY_SIZE(r->descriptor_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool,
        .descriptorSetCount = ARRAY_SIZE(r->descriptor_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets));

    /* the texture set array */
    if (r->uber_active) {
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            layouts[i] = r->uber_tex_descriptor_set_layout;
        }
        VkDescriptorSetAllocateInfo tex_alloc_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = r->descriptor_pool,
            .descriptorSetCount = ARRAY_SIZE(r->uber_tex_descriptor_sets),
            .pSetLayouts = layouts,
        };
        VK_CHECK(vkAllocateDescriptorSets(r->device, &tex_alloc_info,
                                          r->uber_tex_descriptor_sets));
        /* fresh sets have no valid contents */
        memset(r->uber_tex_set_pattern, 0xFF,
               sizeof(r->uber_tex_set_pattern));
    }
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->descriptor_sets), r->descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->descriptor_sets); i++) {
        r->descriptor_sets[i] = VK_NULL_HANDLE;
    }
    if (r->uber_active && r->uber_tex_descriptor_sets[0] != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(r->device, r->descriptor_pool,
                             ARRAY_SIZE(r->uber_tex_descriptor_sets),
                             r->uber_tex_descriptor_sets);
        for (int i = 0; i < ARRAY_SIZE(r->uber_tex_descriptor_sets); i++) {
            r->uber_tex_descriptor_sets[i] = VK_NULL_HANDLE;
        }
    }
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    bool need_uniform_write =
        r->uniforms_changed ||
        !r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset;

    if (!(r->shader_bindings_changed || r->texture_bindings_changed ||
          (r->descriptor_set_index == 0) || need_uniform_write)) {
        return; // Nothing changed
    }

    ShaderBinding *binding = r->shader_binding;

    int num_ubo_layouts = 2;
    ShaderUniformLayout *layouts[3];
    static const int ubo_bindings[] = { VSH_UBO_BINDING, PSH_UBO_BINDING,
                                        UBER_GEOM_UBO_BINDING };
    layouts[0] = &binding->vsh.module_info->uniforms;
    layouts[1] = &binding->psh.module_info->uniforms;
    if (r->uber_active && binding->geom.module_info) {
        layouts[num_ubo_layouts++] = &binding->geom.module_info->uniforms;
    }

    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < num_ubo_layouts; i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }
    bool need_ubo_staging_buffer_reset =
        r->uniforms_changed &&
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_UNIFORM_STAGING,
                                        ubo_buffer_total_size,
                                        r->device_props.limits.minUniformBufferOffsetAlignment);

    bool need_descriptor_write_reset =
        (r->descriptor_set_index >= ARRAY_SIZE(r->descriptor_sets)) ||
        (r->uber_active &&
         r->uber_tex_descriptor_set_index >=
             ARRAY_SIZE(r->uber_tex_descriptor_sets));

    if (need_descriptor_write_reset || need_ubo_staging_buffer_reset) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        need_uniform_write = true;
    }

    int uber_tex_extra = r->uber_active ? 3 * NV2A_MAX_TEXTURES : 0;
    int num_descriptor_writes = num_ubo_layouts + NV2A_MAX_TEXTURES + uber_tex_extra;
    VkWriteDescriptorSet descriptor_writes[3 + 4 * NV2A_MAX_TEXTURES];

    assert(r->descriptor_set_index < ARRAY_SIZE(r->descriptor_sets));

    if (need_uniform_write) {
        for (int i = 0; i < num_ubo_layouts; i++) {
            void *data = layouts[i]->allocation;
            VkDeviceSize size = layouts[i]->total_size;
            r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                r->device_props.limits.minUniformBufferOffsetAlignment);
        }

        r->uniforms_changed = false;
    }

    if (r->uber_active) {
        /* split write paths — UBO set vs texture set.
         * Static-offset descriptors, written per uniform
         * change (the dynamic offsets reverted - fragment-bound NVIDIA
         * penalty). */
        bool write_ubos = need_uniform_write || r->shader_bindings_changed ||
                          r->descriptor_set_index == 0;
        bool write_texs = r->texture_bindings_changed ||
                          r->uber_tex_descriptor_set_index == 0;
        /* under batched, textures ride the set-2
         * arrays — the per-draw set-1 writes retire (one initial write
         * keeps a valid bindable handle; stale thereafter is legal, the
         * batched PSH never accesses set-1 samplers + PARTIALLY_BOUND).
         * A stable handle also keeps the bind-shadow batch-friendly. */
        if (r->uber_batched && r->uber_tex_descriptor_set_index != 0) {
            write_texs = false;
        }
        int w = 0;

        /* the set-0 layout declares 3 dynamic descriptors and
         * the bind passes 3 offsets — the geom UBO must always be present. */
        assert(num_ubo_layouts == 3);

        VkDescriptorBufferInfo ubo_buffer_infos[3];
        if (write_ubos) {
            for (int i = 0; i < num_ubo_layouts; i++) {
                ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
                    .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
                    .offset = r->uniform_buffer_offsets[i],
                    .range = layouts[i]->total_size,
                };
                descriptor_writes[w++] = (VkWriteDescriptorSet){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = r->descriptor_sets[r->descriptor_set_index],
                    .dstBinding = ubo_bindings[i],
                    .dstArrayElement = 0,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .descriptorCount = 1,
                    .pBufferInfo = &ubo_buffer_infos[i],
                };
            }
        }

        VkDescriptorImageInfo image_infos[4 * NV2A_MAX_TEXTURES];
        if (write_texs) {
            assert(r->uber_tex_descriptor_set_index <
                   ARRAY_SIZE(r->uber_tex_descriptor_sets));
            int tidx = r->uber_tex_descriptor_set_index;
            VkDescriptorSet tex_set = r->uber_tex_descriptor_sets[tidx];

            /* dummy descriptors are eternal objects — if this
             * (recycled) set was last written with the same family pattern,
             * its dummy slots are already correct and only the real-texture
             * slots need rewriting (16 writes -> 8 on stable scenes). */
            uint8_t pattern = 0;
            for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                TextureBinding *tb = r->texture_bindings[i];
                bool is_cube = tb->key.state.cubemap;
                bool is_3d = (!is_cube && tb->key.state.dimensionality == 3);
                pattern |= (uint8_t)((is_cube ? 1 : 0) | (is_3d ? 2 : 0))
                           << (2 * i);
            }
            bool full = (r->uber_tex_set_pattern[tidx] != pattern);
            /* with PARTIALLY_BOUND the inactive Cube/3D slots are
             * left unbound (not dummy-filled), so the family-"full" rewrite no
             * longer forces their dummies — only the always-bound 2D/Uint slots
             * still honor it. The shader never accesses an inactive cube/3d slot
             * (uber_sample_cube/_3d are all under is_cubemap/is_3d), and the 2D
             * slot stays dummy-bound because uber_normalize_tex reads it for
             * every type. Full-path 16->8 writes/draw on all-2D content. */
            bool pb = r->descriptor_indexing_enabled;

            for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
                TextureBinding *tb = r->texture_bindings[i];
                bool is_cube = tb->key.state.cubemap;
                bool is_3d = (!is_cube && tb->key.state.dimensionality == 3);
                /* the uber PSH samples the Uint slot only under
                 * is_shadow && is_x8y24 (ubershader.c:762,809) — and X8_Y24 is a
                 * depth format, so is_x8y24 implies is_shadow. Bind Uint only if the
                 * texture is X8_Y24; otherwise it's never dynamically used and may
                 * stay unbound (conservative: covers all uint-access cases). */
                bool is_x8y24 =
                    tb->key.state.color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
                    tb->key.state.color_format ==
                        NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT;

                /* the texture size read rides the UBO, so a
                 * cube/3d stage's 2D slot is never dynamically accessed (its
                 * sampling goes through the Cube/3D views) — under
                 * PARTIALLY_BOUND it may stay unbound/stale. Completes the
                 * descriptor-skip path: the last dummy writes drop. */
                if ((full && !pb) || !(is_cube || is_3d)) {
                    TextureBinding *tex_2d =
                        (is_cube || is_3d) ? &r->dummy_texture : tb;
                    image_infos[i] = (VkDescriptorImageInfo){
                        .imageLayout =
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .imageView = tex_2d->image_view,
                        .sampler = tex_2d->sampler,
                    };
                    descriptor_writes[w++] = (VkWriteDescriptorSet){
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = tex_set,
                        .dstBinding = PSH_TEX_BINDING + i,
                        .dstArrayElement = 0,
                        .descriptorType =
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .descriptorCount = 1,
                        .pImageInfo = &image_infos[i],
                    };
                }

                if ((full && !pb) || is_cube) { /* no dummy_cube */
                    int ci = NV2A_MAX_TEXTURES + i;
                    TextureBinding *tex_cube =
                        is_cube ? tb : &r->dummy_texture_cube;
                    image_infos[ci] = (VkDescriptorImageInfo){
                        .imageLayout =
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .imageView = tex_cube->image_view,
                        .sampler = tex_cube->sampler,
                    };
                    descriptor_writes[w++] = (VkWriteDescriptorSet){
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = tex_set,
                        .dstBinding = UBER_CUBE_TEX_BINDING + i,
                        .dstArrayElement = 0,
                        .descriptorType =
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .descriptorCount = 1,
                        .pImageInfo = &image_infos[ci],
                    };
                }

                if ((full && !pb) || is_3d) { /* no dummy_3d */
                    int ti = 2 * NV2A_MAX_TEXTURES + i;
                    TextureBinding *tex_3d =
                        is_3d ? tb : &r->dummy_texture_3d;
                    image_infos[ti] = (VkDescriptorImageInfo){
                        .imageLayout =
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .imageView = tex_3d->image_view,
                        .sampler = tex_3d->sampler,
                    };
                    descriptor_writes[w++] = (VkWriteDescriptorSet){
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = tex_set,
                        .dstBinding = UBER_3D_TEX_BINDING + i,
                        .dstArrayElement = 0,
                        .descriptorType =
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .descriptorCount = 1,
                        .pImageInfo = &image_infos[ti],
                    };
                }

                /* drop the always-bound Uint slot when the shader
                 * can't access it (!is_x8y24) — 8->4 writes/draw on non-shadow
                 * content. pb=false keeps the old always-write. */
                if (!pb || is_x8y24) {
                    int ui = 3 * NV2A_MAX_TEXTURES + i;
                    image_infos[ui] = (VkDescriptorImageInfo){
                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .imageView = tb->image_view,
                        .sampler = tb->sampler,
                    };
                    descriptor_writes[w++] = (VkWriteDescriptorSet){
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = tex_set,
                        .dstBinding = UBER_UINT_TEX_BINDING + i,
                        .dstArrayElement = 0,
                        .descriptorType =
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .descriptorCount = 1,
                        .pImageInfo = &image_infos[ui],
                    };
                }
            }

            r->uber_tex_set_pattern[tidx] = pattern;
        }

        if (w > 0) {
            vkUpdateDescriptorSets(r->device, w, descriptor_writes, 0, NULL);
        }
        if (write_ubos) {
            r->descriptor_set_index++;
        }
        if (write_texs) {
            r->uber_tex_descriptor_set_index++;
        }
        return;
    }

    VkDescriptorBufferInfo ubo_buffer_infos[3];
    for (int i = 0; i < num_ubo_layouts; i++) {
        ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
            .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
            .offset = r->uniform_buffer_offsets[i],
            .range = layouts[i]->total_size,
        };
        descriptor_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = ubo_bindings[i],
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &ubo_buffer_infos[i],
        };
    }

    VkDescriptorImageInfo image_infos[4 * NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *tb = r->texture_bindings[i];

        image_infos[i] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = tb->image_view,
            .sampler = tb->sampler,
        };
        descriptor_writes[num_ubo_layouts + i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = PSH_TEX_BINDING + i,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[i],
        };
    }

    vkUpdateDescriptorSets(r->device, num_descriptor_writes, descriptor_writes,
                           0, NULL);

    r->descriptor_set_index++;
}

static void upload_uber_psh_uniforms_vk(ShaderBinding *binding,
                                        const UberPshUniforms *u)
{
    /* resolve the 20 upload locs once per module — not 20
     * uniform_index strcmp scans every draw. */
    ShaderUniformLayout *layout = &binding->psh.module_info->uniforms;
    int *locs = binding->psh.uber_locs;
    if (binding->psh.uber_locs_for != binding->psh.module_info) {
        locs[0]  = uniform_index(layout, "u_combiner_control");
        /* shader_stage_program/other_stage_input/dim_tex staging
         * removed (uniforms deleted from the GLSL). */
        locs[1]  = uniform_index(layout, "u_final_inputs_0");
        locs[2]  = uniform_index(layout, "u_final_inputs_1");
        locs[3]  = uniform_index(layout, "u_rgb_inputs");
        locs[4]  = uniform_index(layout, "u_rgb_outputs");
        locs[5]  = uniform_index(layout, "u_alpha_inputs");
        locs[6]  = uniform_index(layout, "u_alpha_outputs");
        locs[7]  = uniform_index(layout, "u_uber_flags");
        locs[8]  = uniform_index(layout, "u_alpha_func");
        locs[9]  = uniform_index(layout, "u_shadow_depth_func");
        locs[10] = uniform_index(layout, "u_depth_format");
        locs[11] = uniform_index(layout, "u_tex_modes");
        locs[12] = uniform_index(layout, "u_tex_flags");
        locs[13] = uniform_index(layout, "u_colorkey_mode");
        locs[14] = uniform_index(layout, "u_border_logical_size");
        locs[15] = uniform_index(layout, "u_border_inv_real_size");
        locs[16] = uniform_index(layout, "u_compare_mode_packed");
        locs[17] = uniform_index(layout, "u_dot_map");
        locs[18] = uniform_index(layout, "u_input_tex");
        locs[19] = uniform_index(layout, "u_conv_tex");
        locs[20] = uniform_index(layout, "u_tex_size");
        binding->psh.uber_locs_for = binding->psh.module_info;
    }
#define U(slot, field, n) do { \
    if (locs[slot] != -1) uniform_copy(layout, locs[slot], (void *)&u->field, 4, n); \
} while (0)
    U(0,  combiner_control, 1);
    U(1,  final_inputs_0, 1);
    U(2,  final_inputs_1, 1);
    U(3,  rgb_inputs, 8);
    U(4,  rgb_outputs, 8);
    U(5,  alpha_inputs, 8);
    U(6,  alpha_outputs, 8);
    U(7,  uber_flags, 1);
    U(8,  alpha_func, 1);
    U(9,  shadow_depth_func, 1);
    U(10, depth_format, 1);
    U(11, tex_modes, 1);
    U(12, tex_flags, 4);
    U(13, colorkey_mode, 4);
    U(14, border_logical_size, 16);
    U(15, border_inv_real_size, 16);
    U(16, compare_mode_packed, 1);
    U(17, dot_map, 4);
    U(18, input_tex, 4);
    U(19, conv_tex, 4);
    U(20, tex_size, 8); /* ivec2[4] = 8 ints */
#undef U
}

static void upload_uber_vsh_uniforms_vk(ShaderBinding *binding,
                                        const UberVshUniforms *u)
{
    /* resolve the 15 upload locs once per module — not 15
     * uniform_index strcmp scans every draw. */
    ShaderUniformLayout *layout = &binding->vsh.module_info->uniforms;
    int *locs = binding->vsh.uber_locs;
    if (binding->vsh.uber_locs_for != binding->vsh.module_info) {
        locs[0]  = uniform_index(layout, "u_uber_vsh_flags");
        locs[1]  = uniform_index(layout, "u_skinning_mode");
        locs[2]  = uniform_index(layout, "u_fog_mode");
        locs[3]  = uniform_index(layout, "u_foggen_mode");
        locs[4]  = uniform_index(layout, "u_light_enable_packed");
        locs[5]  = uniform_index(layout, "u_texgen_packed");
        locs[6]  = uniform_index(layout, "u_mat_color_src_packed");
        locs[7]  = uniform_index(layout, "u_uniform_attrs");
        locs[8]  = uniform_index(layout, "u_compressed_attrs");
        locs[9]  = uniform_index(layout, "u_swizzle_attrs");
        locs[10] = uniform_index(layout, "u_point_size");
        locs[11] = uniform_index(layout, "u_point_params");
        locs[12] = uniform_index(layout, "u_surface_scale_factor");
        locs[13] = uniform_index(layout, "u_program_length");
        locs[14] = uniform_index(layout, "u_program_data");
        binding->vsh.uber_locs_for = binding->vsh.module_info;
    }
#define U(slot, field, n) do { \
    if (locs[slot] != -1) uniform_copy(layout, locs[slot], (void *)&u->field, 4, n); \
} while (0)
    U(0,  uber_vsh_flags, 1);
    U(1,  skinning_mode, 1);
    U(2,  fog_mode, 1);
    U(3,  foggen_mode, 1);
    U(4,  light_enable_packed, 1);
    U(5,  texgen_packed, 4);
    U(6,  mat_color_src_packed, 1);
    U(7,  uniform_attrs, 1);
    U(8,  compressed_attrs, 1);
    U(9,  swizzle_attrs, 1);
    U(10, point_size, 1);
    U(11, point_params, 8);
    U(12, surface_scale_factor, 1);
    U(13, program_length, 1);
    /* stage only the words the interpreter can read
     * (now 3 packed words/instr of 408); FF draws stage none. */
    U(14, program_data, u->program_length * 3);
#undef U
}

static void upload_uber_geom_uniforms_vk(ShaderBinding *binding,
                                         const UberGeomUniforms *u)
{
    /* resolve uniform locs lazily, once per binding per module,
     * instead of 6 uniform_index strcmp scans per draw. Lazy-at-upload covers
     * every module_info assignment site (init pre-assigns the uber variants,
     * which a bind-site-only resolve missed). Re-resolution is idempotent
     * (same module -> same indices), so even a racing resolve is benign. */
    ShaderUniformLayout *layout = &binding->geom.module_info->uniforms;
    int *locs = binding->geom.uber_locs;
    if (binding->geom.uber_locs_for != binding->geom.module_info) {
        locs[0] = uniform_index(layout, "u_primitive_mode");
        locs[1] = uniform_index(layout, "u_polygon_front_mode");
        locs[2] = uniform_index(layout, "u_polygon_back_mode");
        locs[3] = uniform_index(layout, "u_uber_geom_flags");
        locs[4] = uniform_index(layout, "u_tri_rot0");
        locs[5] = uniform_index(layout, "u_tri_rot1");
        binding->geom.uber_locs_for = binding->geom.module_info;
    }
#define U(slot, field, n) do { \
    if (locs[slot] != -1) \
        uniform_copy(layout, locs[slot], (void *)&u->field, 4, n); \
} while (0)
    U(0, primitive_mode, 1);
    U(1, polygon_front_mode, 1);
    U(2, polygon_back_mode, 1);
    U(3, uber_geom_flags, 1);
    U(4, tri_rot0, 1);
    U(5, tri_rot1, 1);
#undef U
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_ref_shader_module(module->module_info);
    return module->module_info;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&binding->state.geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key.geom.state = binding->state.geom;
        key.geom.glsl_opts.vulkan = true;
        binding->geom.module_info = get_and_ref_shader_module_for_key(r, &key);
    } else {
        binding->geom.module_info = NULL;
    }

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    key.vsh.state = binding->state.vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    binding->vsh.module_info = get_and_ref_shader_module_for_key(r, &key);

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key.psh.state = binding->state.psh;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key.psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    binding->psh.module_info = get_and_ref_shader_module_for_key(r, &key);

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return memcmp(&snode->state, key, sizeof(ShaderState));
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, mstring_get_str(code));
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_unref_shader_module(r, module->module_info);
    module->module_info = NULL;
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const size_t shader_cache_size = 1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((void *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

static void apply_uniform_updates(ShaderUniformLayout *layout,
                                  const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] != -1) {
            uniform_copy(layout, locs[i], (char*)values + info[i].val_offs,
                         4, (info[i].size * info[i].count) / 4);
        }
    }
}

static bool check_uniform_regs_dirty(PGRAPHState *pg,
                                      PGRAPHVkState *r)
{
    static const unsigned int uniform_regs[] = {
        NV_PGRAPH_COMBINEFACTOR0,     NV_PGRAPH_COMBINEFACTOR0 + 4,
        NV_PGRAPH_COMBINEFACTOR0 + 8, NV_PGRAPH_COMBINEFACTOR0 + 12,
        NV_PGRAPH_COMBINEFACTOR0 + 16, NV_PGRAPH_COMBINEFACTOR0 + 20,
        NV_PGRAPH_COMBINEFACTOR0 + 24, NV_PGRAPH_COMBINEFACTOR0 + 28,
        NV_PGRAPH_COMBINEFACTOR1,     NV_PGRAPH_COMBINEFACTOR1 + 4,
        NV_PGRAPH_COMBINEFACTOR1 + 8, NV_PGRAPH_COMBINEFACTOR1 + 12,
        NV_PGRAPH_COMBINEFACTOR1 + 16, NV_PGRAPH_COMBINEFACTOR1 + 20,
        NV_PGRAPH_COMBINEFACTOR1 + 24, NV_PGRAPH_COMBINEFACTOR1 + 28,
        NV_PGRAPH_SPECFOGFACTOR0, NV_PGRAPH_SPECFOGFACTOR1,
        NV_PGRAPH_COLORKEYCOLOR0, NV_PGRAPH_COLORKEYCOLOR1,
        NV_PGRAPH_COLORKEYCOLOR2, NV_PGRAPH_COLORKEYCOLOR3,
        NV_PGRAPH_FOGCOLOR, NV_PGRAPH_FOGPARAM0, NV_PGRAPH_FOGPARAM1,
        NV_PGRAPH_BUMPMAT00, NV_PGRAPH_BUMPMAT00 + 4, NV_PGRAPH_BUMPMAT00 + 8,
        NV_PGRAPH_BUMPMAT01, NV_PGRAPH_BUMPMAT01 + 4, NV_PGRAPH_BUMPMAT01 + 8,
        NV_PGRAPH_BUMPMAT10, NV_PGRAPH_BUMPMAT10 + 4, NV_PGRAPH_BUMPMAT10 + 8,
        NV_PGRAPH_BUMPMAT11, NV_PGRAPH_BUMPMAT11 + 4, NV_PGRAPH_BUMPMAT11 + 8,
        NV_PGRAPH_BUMPSCALE1, NV_PGRAPH_BUMPSCALE1 + 4, NV_PGRAPH_BUMPSCALE1 + 8,
        NV_PGRAPH_BUMPOFFSET1, NV_PGRAPH_BUMPOFFSET1 + 4, NV_PGRAPH_BUMPOFFSET1 + 8,
        NV_PGRAPH_WINDOWCLIPX0,      NV_PGRAPH_WINDOWCLIPX0 + 4,
        NV_PGRAPH_WINDOWCLIPX0 + 8,  NV_PGRAPH_WINDOWCLIPX0 + 12,
        NV_PGRAPH_WINDOWCLIPX0 + 16, NV_PGRAPH_WINDOWCLIPX0 + 20,
        NV_PGRAPH_WINDOWCLIPX0 + 24, NV_PGRAPH_WINDOWCLIPX0 + 28,
        NV_PGRAPH_WINDOWCLIPY0,      NV_PGRAPH_WINDOWCLIPY0 + 4,
        NV_PGRAPH_WINDOWCLIPY0 + 8,  NV_PGRAPH_WINDOWCLIPY0 + 12,
        NV_PGRAPH_WINDOWCLIPY0 + 16, NV_PGRAPH_WINDOWCLIPY0 + 20,
        NV_PGRAPH_WINDOWCLIPY0 + 24, NV_PGRAPH_WINDOWCLIPY0 + 28,
        NV_PGRAPH_ZCLIPMIN, NV_PGRAPH_ZCLIPMAX,
    };
    for (int i = 0; i < ARRAY_SIZE(uniform_regs); i++) {
        if (pgraph_is_reg_dirty(pg, uniform_regs[i])) {
            return true;
        }
    }

    uint32_t c0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t alpha_ref = GET_MASK(c0, NV_PGRAPH_CONTROL_0_ALPHAREF);
    uint32_t z_persp = c0 & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE;
    uint32_t sr = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    uint32_t zb = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
    uint32_t zf = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
    if (alpha_ref != r->last_alpha_ref ||
        z_persp != r->last_z_persp ||
        sr != r->last_setupraster_uniform ||
        zb != r->last_zoffsetbias ||
        zf != r->last_zoffsetfactor) {
        r->last_alpha_ref = alpha_ref;
        r->last_z_persp = z_persp;
        r->last_setupraster_uniform = sr;
        r->last_zoffsetbias = zb;
        r->last_zoffsetfactor = zf;
        return true;
    }

    return false;
}

/* hash the inline vertex-attribute values that
 * set_vsh_uniform_values consumes via pgraph_get_inline_values(uniform_attrs).
 * Those (SET_VERTEX_DATA*) writes bump no covering signal — unlike material/
 * light/point which bump uniform_state_gen — so the vsh-values cache went
 * stale on inline-value changes (fog/material/spotlight/vsh-round + the vertex
 * colors feeding shadow/blend). Gate the vsh-values recompute on this. Hashes
 * only the uniform_attrs slots that are actually read; uber-only call site. */
static uint64_t uber_inline_values_hash_vk(PGRAPHState *pg)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t ua = pg->uniform_attrs;
    h = (h ^ ua) * 1099511628211ULL;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (!(ua & (1u << i))) {
            continue;
        }
        const uint8_t *p =
            (const uint8_t *)pg->vertex_attributes[i].inline_value;
        for (size_t b = 0; b < sizeof(pg->vertex_attributes[i].inline_value);
             b++) {
            h = (h ^ p[b]) * 1099511628211ULL;
        }
    }
    return h;
}

static void update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);
    ShaderBinding *binding = r->shader_binding;

    /* captured before the early-out's last_* update below. */
    bool uber_dims_changed =
        pg->surface_binding_dim.width != r->last_surface_width ||
        pg->surface_binding_dim.height != r->last_surface_height;

    /* the early-out below was unreachable for uber
     * (uber_state_changed was force-true at the bind site) and missed the
     * c[]/lighting-context dirty arrays .
     * Both fixed: the bind site now passes real state_dirty, and the
     * uniform-value arrays join the condition (consumed after upload).
     * The signals are hoisted into named bools — the same
     * condition set also gates the per-setter value caches below (the reads
     * are idempotent; the reg-dirty map clears once at end-of-draw). */
    bool uber_gen_changed = false, uber_ltctx_any = false;
    bool uber_c_any = false, uber_regs_dirty = false;
    /* inline vertex-attr values feed vsh_values but
     * bump no covering signal — track by hash so the value-cache invalidates. */
    bool uber_inline_changed = false;
    uint64_t uber_inline_h = 0;
    if (r->uber_active) {
        uber_gen_changed = pg->uniform_state_gen != r->last_uniform_state_gen;
        uber_inline_h = uber_inline_values_hash_vk(pg);
        uber_inline_changed = uber_inline_h != r->uber_inline_values_hash;
        /* writer generations replace the per-draw flag
         * scans (the memcmp over ~290 bools); gens differ exactly when at least
         * one flag was set since this path last consumed (the flags
         * themselves are untouched; the span logic below still reads them).
         */
        uber_c_any = pg->vsh_constants_wgen != r->uber_last_c_wgen;
        uber_ltctx_any = pg->ltctx_wgen != r->uber_last_ltctx_wgen;
        uber_regs_dirty = check_uniform_regs_dirty(pg, r);
        if (!r->uber_state_changed && !r->texture_bindings_changed &&
            !uber_gen_changed && !uber_dims_changed && !uber_c_any &&
            !uber_ltctx_any && !uber_regs_dirty && !uber_inline_changed) {
            r->uniforms_changed = false;
            nv2a_profile_inc_counter(NV2A_PROF_SHADER_UBO_NOTDIRTY);
            NV2A_VK_DGROUP_END();
            return;
        }
    }
    r->last_uniform_state_gen = pg->uniform_state_gen;
    /* every path past the early-out consumes the dirty
     * state (the setters/staging read and clear the flags), so the seen
     * generations advance here. */
    r->uber_last_c_wgen = pg->vsh_constants_wgen;
    r->uber_last_ltctx_wgen = pg->ltctx_wgen;
    r->last_surface_width = pg->surface_binding_dim.width;
    r->last_surface_height = pg->surface_binding_dim.height;
    r->uber_inline_values_hash = uber_inline_h; /* uber_inline_h is 0 for spec */

    int num_ubo_layouts = 2;
    ShaderUniformLayout *layouts[3];
    layouts[0] = &binding->vsh.module_info->uniforms;
    layouts[1] = &binding->psh.module_info->uniforms;
    if (r->uber_active && binding->geom.module_info) {
        layouts[num_ubo_layouts++] = &binding->geom.module_info->uniforms;
    }

    /* compute phase: derive every per-draw source value first
     * (no UBO writes yet) so they can be hashed before the std140 expansion.
     * For uber the value structs live in PGRAPHVkState and are
     * recomputed only when a covering signal fires (vsh: state/gen/ltctx/regs/
     * dims — c[] excluded, texture bindings verified-unread by the
     * setter; psh: state/tex/gen/regs/dims — ltctx verified-unread). The spec
     * path recomputes its locals every call, exactly as before. */
    VshUniformValues vsh_values_local;
    PshUniformValues psh_values_local;
    VshUniformValues *vshv =
        r->uber_active ? &r->uber_vsh_values_cached : &vsh_values_local;
    PshUniformValues *pshv =
        r->uber_active ? &r->uber_psh_values_cached : &psh_values_local;
    /* PERF: separate the inline-only case so it takes the cheap
     * inlineValue-refresh path below (recovers the cache win on inline-
     * churning content, e.g. vertex colors) instead of a full recompute. */
    bool vshv_dirty_other = !r->uber_active || !r->uber_values_valid ||
                            r->uber_state_changed || uber_gen_changed ||
                            uber_ltctx_any || uber_regs_dirty || uber_dims_changed;
    bool vshv_dirty = vshv_dirty_other || uber_inline_changed;
    bool pshv_dirty = !r->uber_active || !r->uber_values_valid ||
                      r->uber_state_changed || r->texture_bindings_changed ||
                      uber_gen_changed || uber_regs_dirty || uber_dims_changed;

    int c_loc = binding->vsh.uniform_locs[VshUniform_c];
    /* c[] is staged straight from pg->vsh_constants (locs_no_c
     * drops it from the apply), so vsh_values excludes it. */
    bool uber_c = r->uber_active && c_loc != -1;
    int locs_no_c[VshUniform__COUNT];
    if (uber_c) {
        memcpy(locs_no_c, binding->vsh.uniform_locs, sizeof(locs_no_c));
        locs_no_c[VshUniform_c] = -1;
    }
    if (vshv_dirty_other) {
        pgraph_glsl_set_vsh_uniform_values(
            pg, &binding->state.vsh,
            uber_c ? locs_no_c : binding->vsh.uniform_locs, vshv);
        if (r->uber_active) {
            r->uber_vsh_values_subhash = fast_hash((const uint8_t *)vshv, sizeof(*vshv));
        }
    } else if (uber_inline_changed) {
        /* PERF: only the inline vertex-attr values changed — the rest
         * of the cached vsh_values is still valid (no other signal fired), so
         * refresh just inlineValue (cheap) instead of the full recompute. The
         * resulting struct is byte-identical to a full set_vsh_uniform_values
         * here (fresh inlineValue + unchanged remainder). uber-only by
         * construction (!uber_active forces vshv_dirty_other). */
        if (binding->vsh.uniform_locs[VshUniform_inlineValue] != -1) {
            pgraph_get_inline_values(pg, binding->state.vsh.uniform_attrs,
                                     vshv->inlineValue, NULL);
        }
        r->uber_vsh_values_subhash = fast_hash((const uint8_t *)vshv, sizeof(*vshv));
    }

    if (pshv_dirty) {
        pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                           pshv);
        for (int i = 0; i < 4; i++) {
            assert(r->texture_bindings[i] != NULL);
            float scale = r->texture_bindings[i]->key.scale;

            BasicColorFormatInfo f_basic =
                kelvin_color_format_info_map[pg->vk_renderer_state
                                                 ->texture_bindings[i]
                                                 ->key.state.color_format];
            if (!f_basic.linear) {
                scale = 1.0;
            }

            pshv->texScale[i] = scale;
        }
        if (r->uber_active) {
            r->uber_psh_values_subhash = fast_hash((const uint8_t *)pshv, sizeof(*pshv));
        }
    }
    if (r->uber_active) {
        r->uber_values_valid = true;
    }

    /* the uber structs depend only on shader state, texture
     * border/scale and surface dims — the upload below is gated on the same. */
    bool uber_structs_dirty =
        r->uber_active && (r->uber_state_changed ||
                           r->texture_bindings_changed || uber_dims_changed);
    /* recompute the three uber structs (incl.
     * the program_data 3-word pack) and their hash contribution only when
     * the gate fires — every input they read (combiner/tex/raster regs,
     * binding->state, texture borders/scale, surface dims) is covered by it.
     * Value-only draws (the common dirty class) reuse the cache. */
    if (uber_structs_dirty) {
        pgraph_uber_set_psh_uniforms(pg, &r->uber_psh_cached);
        /* per-stage dims of the BOUND 2D slot — must mirror
         * the descriptor write's real-vs-dummy choice exactly so the uniform
         * equals what textureSize() would have returned. Covered by this
         * gate (texture changes set texture_bindings_changed). */
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            TextureBinding *tb = r->texture_bindings[i];
            bool is_cube = tb->key.state.cubemap;
            bool is_3d = (!is_cube && tb->key.state.dimensionality == 3);
            TextureBinding *t2d =
                (is_cube || is_3d) ? &r->dummy_texture : tb;
            r->uber_psh_cached.tex_size[i][0] = (int32_t)t2d->uber_dim_w;
            r->uber_psh_cached.tex_size[i][1] = (int32_t)t2d->uber_dim_h;
        }
        pgraph_uber_set_vsh_uniforms(pg, &binding->state.vsh,
                                     &r->uber_vsh_cached);
        pgraph_uber_set_geom_uniforms(pg, &binding->state.geom,
                                      &r->uber_geom_cached);
        uint64_t sh = fast_hash((const uint8_t *)&r->uber_psh_cached,
                                sizeof(r->uber_psh_cached));
        sh = (sh * 0x100000001b3ull) ^
             fast_hash((const uint8_t *)&r->uber_vsh_cached, sizeof(r->uber_vsh_cached));
        sh = (sh * 0x100000001b3ull) ^
             fast_hash((const uint8_t *)&r->uber_geom_cached, sizeof(r->uber_geom_cached));
        r->uber_structs_subhash = sh;
    }

    /* hash ALL the per-draw source (vsh/psh values + the three
     * uber structs + c[]) before expanding into the std140 layouts. Most FF
     * draws re-derive byte-identical source, so every apply/memcpy/upload below
     * and the staging is wasted — skip it all. Gated on the adaptive credit
     * (churny-varying content pays ~0 hash: hash while credit>=0, decay to -8 on
     * sustained change then stop, re-arm 1/1024; a non-hashed draw force-writes
     * + marks the baseline stale so the next hash re-writes). Supersedes the
     * an earlier layout-hash (cheaper: ~5KB source vs 12KB layout) and struct-
     * hash. uber_struct_hash is reused as the source-hash baseline. */
    bool uber_skip = false;
    if (r->uber_active && r->uber_hash_credit >= 0) {
        /* every contribution is a precomputed subhash riding
         * its own recompute signal. The 3KB c[]
         * hash is reused while the constants' dirty flags are clean — the
         * fingerprint stays fixed exactly while the bytes do. */
        if (uber_c_any || !r->uber_c_hash_valid) {
            r->uber_c_subhash =
                fast_hash((const uint8_t *)pg->vsh_constants, sizeof(pg->vsh_constants));
            r->uber_c_hash_valid = true;
        }
        uint64_t h = r->uber_vsh_values_subhash;
        h = (h * 0x100000001b3ull) ^ r->uber_psh_values_subhash;
        /* the three structs' contribution is precomputed in
         * the gated block above (~2.5KB of the ~5KB hash skipped on
         * value-only draws). */
        h = (h * 0x100000001b3ull) ^ r->uber_structs_subhash;
        h = (h * 0x100000001b3ull) ^ r->uber_c_subhash;
        if (!r->uber_hash_valid) {
            r->uber_struct_hash = h;
            r->uber_hash_valid = true;
        } else if (h == r->uber_struct_hash) {
            uber_skip = true;
            if (r->uber_hash_credit < 8) {
                r->uber_hash_credit++;
            }
        } else {
            r->uber_struct_hash = h;
            if (r->uber_hash_credit > -8) {
                r->uber_hash_credit--;
            }
        }
    } else if (r->uber_active) {
        r->uber_hash_valid = false;
        if ((r->uber_hash_probe++ & 1023u) == 0) {
            r->uber_hash_credit = 0; /* re-arm */
        }
    }

    if (uber_skip) {
        /* Source byte-identical to last draw: the layouts already hold it.
         * Consume the dirty-arrays and reuse the staged UBO. */
        memset(pg->vsh_constants_dirty, 0, sizeof(pg->vsh_constants_dirty));
        memset(pg->ltctxa_dirty, 0, sizeof(pg->ltctxa_dirty));
        memset(pg->ltctxb_dirty, 0, sizeof(pg->ltctxb_dirty));
        memset(pg->ltc1_dirty, 0, sizeof(pg->ltc1_dirty));
        r->uniforms_changed = false;
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_UBO_NOTDIRTY);
        NV2A_VK_DGROUP_END();
        return;
    }

    /* ---- write phase (uber source changed, or spec) ----
     * The value applies are gated on the same flags that
     * gated their recompute — when the values didn't change, the persistent
     * layout allocation already holds these exact bytes from the last apply
     * (module switches imply state_dirty -> dirty), so re-expanding them is a
     * byte-identical rewrite. c[]-only draws now skip both std430 expansions;
     * the c[] span memcpy below still runs. Spec: flags forced true. */
    if (uber_c) {
        if (vshv_dirty) {
            apply_uniform_updates(&binding->vsh.module_info->uniforms,
                                  VshUniformInfo, locs_no_c, vshv,
                                  VshUniform__COUNT);
        }
        ShaderUniformLayout *vl = &binding->vsh.module_info->uniforms;
        ShaderUniform *su = &vl->uniforms[c_loc - 1];
        assert(su->stride == 16 && "c[] must be a packed vec4 array");
        /* state-changed → full range; value-only → only the dirty c[] span
         * (std140 vec4 stride-16 = one contiguous memcpy). */
        int lo = 0, hi = NV2A_VERTEXSHADER_CONSTANTS - 1;
        if (!r->uber_state_changed) {
            lo = NV2A_VERTEXSHADER_CONSTANTS; hi = -1;
            for (int i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
                if (pg->vsh_constants_dirty[i]) {
                    if (i < lo) lo = i;
                    if (i > hi) hi = i;
                }
            }
        }
        if (hi >= lo) {
            memcpy((char *)vl->allocation + su->offset + (size_t)lo * 16,
                   (const char *)pg->vsh_constants + (size_t)lo * 16,
                   (size_t)(hi - lo + 1) * 16);
        }
    } else if (vshv_dirty) {
        apply_uniform_updates(&binding->vsh.module_info->uniforms,
                              VshUniformInfo, binding->vsh.uniform_locs,
                              vshv, VshUniform__COUNT);
    }
    if (pshv_dirty) {
        apply_uniform_updates(&binding->psh.module_info->uniforms,
                              PshUniformInfo, binding->psh.uniform_locs, pshv,
                              PshUniform__COUNT);
    }
    if (uber_structs_dirty) {
        upload_uber_psh_uniforms_vk(binding, &r->uber_psh_cached);
        upload_uber_vsh_uniforms_vk(binding, &r->uber_vsh_cached);
        if (binding->geom.module_info) {
            upload_uber_geom_uniforms_vk(binding, &r->uber_geom_cached);
        }
    }

    if (r->uber_active) {
        /* reaching here means the source changed (or first draw)
         * → the layouts were just rewritten, so stage them. Consume the value
         * dirty-arrays (the c[] dirty-span above used them). The adaptive credit
         * + source-hash decision was made in the COMPUTE phase above. */
        memset(pg->vsh_constants_dirty, 0, sizeof(pg->vsh_constants_dirty));
        memset(pg->ltctxa_dirty, 0, sizeof(pg->ltctxa_dirty));
        memset(pg->ltctxb_dirty, 0, sizeof(pg->ltctxb_dirty));
        memset(pg->ltc1_dirty, 0, sizeof(pg->ltc1_dirty));
        r->uniforms_changed = true;
    } else {
        for (int i = 0; i < num_ubo_layouts; i++) {
            uint64_t hash =
                fast_hash(layouts[i]->allocation, layouts[i]->total_size);
            r->uniforms_changed |= (hash != r->uniform_buffer_hashes[i]);
            r->uniform_buffer_hashes[i] = hash;
        }
    }

    nv2a_profile_inc_counter(r->uniforms_changed ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);
    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    if (r->uber_active) {
        ShaderBinding *binding;
        ShaderModuleInfo *geom_mod;

        bool wireframe = ((enum ShaderPolygonMode)GET_MASK(
            pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
            NV_PGRAPH_SETUPRASTER_FRONTFACEMODE) == POLY_MODE_LINE);

        /* smooth tri-class draws take the NV passthrough GS
         * (per-vertex attrs forward unchanged; the GS runs once per prim).
         * Flat shading needs the runtime provoking-vertex select, which
         * passthrough cannot express - the classic tri GS serves it. */
        bool tri_pass = r->geom_passthrough_enabled &&
            (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                      NV_PGRAPH_CONTROL_3_SHADEMODE) ==
             NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH);

        switch ((enum ShaderPrimitiveMode)pg->primitive_mode) {
        case PRIM_TYPE_POINTS:
            binding = &r->uber_shader_binding_points;
            geom_mod = r->uber_geom_points_module;
            break;
        case PRIM_TYPE_LINES:
        case PRIM_TYPE_LINE_LOOP:
        case PRIM_TYPE_LINE_STRIP:
            binding = &r->uber_shader_binding_line;
            geom_mod = r->uber_geom_line_module;
            break;
        case PRIM_TYPE_QUADS:
        case PRIM_TYPE_QUAD_STRIP:
            if (wireframe) {
                binding = &r->uber_shader_binding_quad_wireframe;
                geom_mod = r->uber_geom_quad_wireframe_module;
            } else {
                binding = &r->uber_shader_binding_quad;
                geom_mod = r->uber_geom_quad_module;
            }
            break;
        case PRIM_TYPE_POLYGON:
            if (wireframe) {
                binding = &r->uber_shader_binding_line;
                geom_mod = r->uber_geom_line_module;
            } else if (tri_pass) {
                binding = &r->uber_shader_binding_tri_pass;
                geom_mod = r->uber_geom_tri_pass_module;
            } else {
                binding = &r->uber_shader_binding;
                geom_mod = r->uber_geom_module;
            }
            break;
        default:
            if (tri_pass) {
                binding = &r->uber_shader_binding_tri_pass;
                geom_mod = r->uber_geom_tri_pass_module;
            } else {
                binding = &r->uber_shader_binding;
                geom_mod = r->uber_geom_module;
            }
            break;
        }

        bool binding_changed = (r->shader_binding != binding);
        /* the uber_program_data_dirty computation removed —
         * it was derived here every draw (2 reg-dirty probes + ors) and
         * never read anywhere (write-only vestige of a removed gate). */
        bool state_dirty = !r->shader_binding || binding_changed ||
            pgraph_glsl_check_shader_state_dirty(pg,
                                                 &r->shader_binding->state);

        /* was `state_dirty || r->uber_active` — the blunt
         * An earlier correctness patch forced this whole derivation block (3KB
         * ShaderState rebuild + ~15 reg reads) every draw. Every input below
         * (SHADERPROG, CONTROL_0/3, SETUPRASTER, ZCOMPRESSOCCLUDE,
         * TEXCTL0_n, TEXFMT0_n, zeta_format) is covered by
         * pgraph_glsl_check_shader_state_dirty, so !state_dirty implies every
         * derived value (and binding->state itself) is unchanged. The
         * uniform UPLOAD forcing below (uber_state_changed) is untouched.
         * program_data_dirty cannot leak: while set, the check returns dirty,
         * so the clean path is unreachable until get_shader_state clears it. */
        if (state_dirty) {
            /* minimal binding->state fill (was the full 3KB
             * get_shader_state + 3KB copy — the prebaked uber never consumes
             * that state; only the next draw's dirty check reads the 10 fields
             * the minimal builder sets). Dominant BIND_SHADERS cost. */
            pgraph_glsl_set_uber_state_min(pg, &binding->state);

            uint32_t sp = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
            uint32_t te = 0;
            uint32_t tm = sp & 0xFFFFF;
            for (int i = 0; i < 4; i++) {
                if (((sp >> (i * 5)) & 0x1F) != 0) te |= (1u << i);
            }
            if (te != r->uber_tex_enable || tm != r->uber_tex_modes) {
                r->uber_tex_enable = te;
                r->uber_tex_modes = tm;
                r->shader_bindings_changed = true;
            }

            unsigned int z_format = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                             NV_PGRAPH_SETUPRASTER_Z_FORMAT);
            switch (pg->surface_shape.zeta_format) {
            case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
                r->uber_depth_format = z_format ? 3 : 0; break;
            case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
                r->uber_depth_format = z_format ? 2 : 1; break;
            default:
                r->uber_depth_format = 0; break;
            }
        }

        if (binding_changed) {
            r->shader_bindings_changed = true;
        }
        binding->geom.module_info = geom_mod;
        r->shader_binding = binding;
        /* was `state_dirty || r->uber_active` — the force-true
         * kept the update_shader_uniforms early-out permanently disabled for
         * uber. With the early-out's value coverage completed (dirty arrays
         * added there), the real signal is safe. */
        r->uber_state_changed = state_dirty;
        update_shader_uniforms(pg);
        NV2A_VK_DGROUP_END();
        return;
    }

    if (!r->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        if (!r->shader_binding || memcmp(&r->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    update_shader_uniforms(pg);

    NV2A_VK_DGROUP_END();
}

static char *uber_cache_get_path_vk(void)
{
    return g_strdup_printf("%s/ubershader_cache_vk.bin",
                           xemu_settings_get_base_path());
}

static bool uber_cache_load_vk(PGRAPHVkState *r)
{
    char *path = uber_cache_get_path_vk();
    FILE *f = qemu_fopen(path, "rb");
    g_free(path);
    if (!f) return false;

    bool ok = false;

    #define READ(dst, sz) if (fread(dst, sz, 1, f) != 1) goto done

    uint64_t ver_len;
    READ(&ver_len, sizeof(ver_len));
    char *cached_ver = g_malloc(ver_len);
    READ(cached_ver, ver_len);
    if (strcmp(cached_ver, xemu_version) != 0) { g_free(cached_ver); goto done; }
    g_free(cached_ver);

    uint32_t cached_vendor_id, cached_driver_ver;
    char cached_dev_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    READ(&cached_vendor_id, sizeof(cached_vendor_id));
    READ(&cached_driver_ver, sizeof(cached_driver_ver));
    READ(cached_dev_name, sizeof(cached_dev_name));
    if (cached_vendor_id != r->device_props.vendorID ||
        cached_driver_ver != r->device_props.driverVersion ||
        strcmp(cached_dev_name, r->device_props.deviceName) != 0) {
        goto done;
    }

    struct { uint64_t size; uint8_t *data; } blobs[7] = {};
    for (int i = 0; i < 7; i++) {
        if (fread(&blobs[i].size, sizeof(blobs[i].size), 1, f) != 1) goto blob_err;
        blobs[i].data = g_malloc(blobs[i].size);
        if (fread(blobs[i].data, blobs[i].size, 1, f) != 1) goto blob_err;
    }

    #undef READ

    r->uber_vsh_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[0].data, blobs[0].size);
    r->uber_psh_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[1].data, blobs[1].size);
    r->uber_geom_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[2].data, blobs[2].size);
    r->uber_geom_quad_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[3].data, blobs[3].size);
    r->uber_geom_line_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[4].data, blobs[4].size);
    r->uber_geom_points_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[5].data, blobs[5].size);
    r->uber_geom_quad_wireframe_module = pgraph_vk_create_shader_module_from_spirv(
        r, blobs[6].data, blobs[6].size);

    for (int i = 0; i < 7; i++) g_free(blobs[i].data);
    ok = true;
    goto done;

blob_err:
    for (int i = 0; i < 7; i++) g_free(blobs[i].data);
done:
    fclose(f);
    return ok;
}

static void uber_cache_save_vk(PGRAPHVkState *r)
{
    char *path = uber_cache_get_path_vk();
    FILE *f = qemu_fopen(path, "wb");
    g_free(path);
    if (!f) return;

    #define WRITE(src, sz) if (fwrite(src, sz, 1, f) != 1) goto err

    uint64_t ver_len = strlen(xemu_version) + 1;
    WRITE(&ver_len, sizeof(ver_len));
    WRITE(xemu_version, ver_len);
    WRITE(&r->device_props.vendorID, sizeof(r->device_props.vendorID));
    WRITE(&r->device_props.driverVersion, sizeof(r->device_props.driverVersion));
    WRITE(r->device_props.deviceName, sizeof(r->device_props.deviceName));

    ShaderModuleInfo *modules[7] = {
        r->uber_vsh_module, r->uber_psh_module, r->uber_geom_module,
        r->uber_geom_quad_module, r->uber_geom_line_module,
        r->uber_geom_points_module, r->uber_geom_quad_wireframe_module
    };
    for (int i = 0; i < 7; i++) {
        uint64_t sz = modules[i]->spirv->len;
        WRITE(&sz, sizeof(sz));
        WRITE(modules[i]->spirv->data, sz);
    }

    #undef WRITE

    fclose(f);
    fprintf(stderr, "nv2a: Vulkan ubershader SPIR-V cached to disk\n");
    return;

err:
    fclose(f);
}

/* set 2 — the batched-draw machinery. One persistent
 * UPDATE_AFTER_BIND set: b0 = the per-range record SSBO, b1..b4 = fixed
 * 2048-entry PARTIALLY_BOUND sampler arrays (2D/cube/3D/uint; variable
 * count is legal only on the last binding, so fixed sizes everywhere).
 * The record ring is host-visible and resets at finish (GPU idle). */
#define UBER_B_REC_CAPACITY (4u * 1024u * 1024u)

static void uber_batched_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[5];
    VkDescriptorBindingFlags bflags[5];
    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    bflags[0] = 0;
    for (int i = 1; i <= 4; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = UBER_B_SLOTS,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        bflags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bflags_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 5,
        .pBindingFlags = bflags,
    };
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bflags_info,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 5,
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->uber_b_set_layout));

    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 4 * UBER_B_SLOTS },
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->uber_b_pool));

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->uber_b_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &r->uber_b_set_layout,
    };
    VK_CHECK(vkAllocateDescriptorSets(r->device, &alloc_info,
                                      &r->uber_b_set));

    VkBufferCreateInfo buf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = UBER_B_REC_CAPACITY,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo buf_alloc = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VmaAllocationInfo out_info;
    VK_CHECK(vmaCreateBuffer(r->allocator, &buf_info, &buf_alloc,
                             &r->uber_b_rec_buffer, &r->uber_b_rec_alloc,
                             &out_info));
    r->uber_b_rec_mapped = (uint8_t *)out_info.pMappedData;
    r->uber_b_rec_capacity = UBER_B_REC_CAPACITY;
    r->uber_b_rec_wp = 0;

    VkDescriptorBufferInfo rec_buf = {
        .buffer = r->uber_b_rec_buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = r->uber_b_set,
        .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &rec_buf,
    };
    vkUpdateDescriptorSets(r->device, 1, &w, 0, NULL);

    for (int t = 0; t < 4; t++) {
        for (int s = 0; s < UBER_B_SLOTS; s++) {
            r->uber_b_free[t][s] = (uint16_t)(UBER_B_SLOTS - 1 - s);
        }
        r->uber_b_free_top[t] = UBER_B_SLOTS;
    }
    r->uber_b_npending = 0;
    r->uber_b_finish_seq = 1;

    /* Cross-draw merge cap for the batched, descriptor-indexed path. */
    r->uber_m_cap = (uint32_t)UBER_M_CAP;
    r->uber_m_count = 0;
    r->uber_m_bds_valid = false;
    r->uber_m_vb_valid = false;
}

static void uber_batched_finalize(PGRAPHVkState *r)
{
    if (r->uber_b_rec_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(r->allocator, r->uber_b_rec_buffer,
                         r->uber_b_rec_alloc);
        r->uber_b_rec_buffer = VK_NULL_HANDLE;
    }
    if (r->uber_b_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(r->device, r->uber_b_pool, NULL);
        r->uber_b_pool = VK_NULL_HANDLE;
    }
    if (r->uber_b_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(r->device, r->uber_b_set_layout, NULL);
        r->uber_b_set_layout = VK_NULL_HANDLE;
    }
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_init_glsl_compiler();

    r->uber_active = g_config.perf.use_ubershaders;
    /* Batched mode needs multidraw plus the fully-dynamic pipeline caps, and is
     * decided before module generation so the shaders are built in batched
     * form. */
    r->uber_batched = r->uber_active && r->uber_batched_caps &&
                      r->multi_draw_enabled &&
                      r->extended_dynamic_state_enabled &&
                      r->extended_dynamic_state3_enabled &&
                      r->eds3_polygon_mode &&
                      r->vertex_input_dynamic_state_enabled;
    if (r->uber_batched) {
        uber_batched_init(pg);
    }
    /* force the first draw to read everything. */
    r->uber_last_c_wgen = (uint64_t)-1;
    r->uber_last_ltctx_wgen = (uint64_t)-1;

    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    if (!r->uber_active) {
        shader_cache_init(pg);
    }

    r->use_push_constants_for_uniform_attrs =
        (r->device_props.limits.maxPushConstantsSize >=
         MAX_UNIFORM_ATTR_VALUES_SIZE);

    if (r->uber_active) {
        r->use_push_constants_for_uniform_attrs = false;
        bool from_cache = g_config.perf.cache_shaders && uber_cache_load_vk(r);

        if (!from_cache) {
            GenUberGlslOptions uber_opts = {
                .vulkan = true,
                .vk_batched = r->uber_batched,
                .ubo_std430 = r->uber_ubo_std430,
                .psh_ubo_binding = PSH_UBO_BINDING,
                .tex_binding = PSH_TEX_BINDING,
                .uber_psh_ubo_binding = PSH_UBO_BINDING,
                .uber_vsh_ubo_binding = VSH_UBO_BINDING,
                .uber_geom_ubo_binding = UBER_GEOM_UBO_BINDING,
            };

            MString *vsh_code = pgraph_glsl_gen_uber_vsh(uber_opts);
            r->uber_vsh_module = pgraph_vk_create_shader_module_from_glsl(
                r, VK_SHADER_STAGE_VERTEX_BIT, mstring_get_str(vsh_code));
            mstring_unref(vsh_code);

            MString *psh_code = pgraph_glsl_gen_uber_psh(uber_opts);
            r->uber_psh_module = pgraph_vk_create_shader_module_from_glsl(
                r, VK_SHADER_STAGE_FRAGMENT_BIT, mstring_get_str(psh_code));
            mstring_unref(psh_code);

            MString *geom_code = pgraph_glsl_gen_uber_geom(uber_opts);
            r->uber_geom_module = pgraph_vk_create_shader_module_from_glsl(
                r, VK_SHADER_STAGE_GEOMETRY_BIT, mstring_get_str(geom_code));
            mstring_unref(geom_code);

            struct {
                MString *(*gen)(GenUberGlslOptions);
                ShaderModuleInfo **mod;
            } extra_gs[] = {
                { pgraph_glsl_gen_uber_geom_quad,            &r->uber_geom_quad_module },
                { pgraph_glsl_gen_uber_geom_quad_wireframe,  &r->uber_geom_quad_wireframe_module },
                { pgraph_glsl_gen_uber_geom_line,            &r->uber_geom_line_module },
                { pgraph_glsl_gen_uber_geom_points,          &r->uber_geom_points_module },
            };
            for (int i = 0; i < ARRAY_SIZE(extra_gs); i++) {
                MString *code = extra_gs[i].gen(uber_opts);
                *extra_gs[i].mod = pgraph_vk_create_shader_module_from_glsl(
                    r, VK_SHADER_STAGE_GEOMETRY_BIT, mstring_get_str(code));
                mstring_unref(code);
            }

            if (g_config.perf.cache_shaders) {
                uber_cache_save_vk(r);
            }

        }

        /* the passthrough tri GS compiles outside the disk
         * cache (keeps the 7-blob format untouched; it is one small GS).
         * On compile failure the feature self-disables and the classic tri
         * GS keeps serving everything. */
        if (r->geom_passthrough_enabled) {
            GenUberGlslOptions pass_opts = {
                .vulkan = true,
                .vk_batched = r->uber_batched,
                .ubo_std430 = r->uber_ubo_std430,
                .psh_ubo_binding = PSH_UBO_BINDING,
                .tex_binding = PSH_TEX_BINDING,
                .uber_psh_ubo_binding = PSH_UBO_BINDING,
                .uber_vsh_ubo_binding = VSH_UBO_BINDING,
                .uber_geom_ubo_binding = UBER_GEOM_UBO_BINDING,
            };
            MString *pass_code =
                pgraph_glsl_gen_uber_geom_tri_passthrough(pass_opts);
            r->uber_geom_tri_pass_module =
                pgraph_vk_create_shader_module_from_glsl(
                    r, VK_SHADER_STAGE_GEOMETRY_BIT,
                    mstring_get_str(pass_code));
            mstring_unref(pass_code);
            if (!r->uber_geom_tri_pass_module) {
                fprintf(stderr, "nv2a/VK: passthrough GS compile failed - "
                                "falling back to the classic tri GS\n");
                r->geom_passthrough_enabled = false;
            }
        }

        pgraph_vk_ref_shader_module(r->uber_vsh_module);
        pgraph_vk_ref_shader_module(r->uber_psh_module);
        pgraph_vk_ref_shader_module(r->uber_geom_module);
        pgraph_vk_ref_shader_module(r->uber_geom_quad_module);
        pgraph_vk_ref_shader_module(r->uber_geom_quad_wireframe_module);
        pgraph_vk_ref_shader_module(r->uber_geom_line_module);
        pgraph_vk_ref_shader_module(r->uber_geom_points_module);
        if (r->geom_passthrough_enabled) {
            pgraph_vk_ref_shader_module(r->uber_geom_tri_pass_module);
        }

        struct {
            ShaderBinding *binding;
            ShaderModuleInfo *geom_mod;
        } vk_variants[] = {
            { &r->uber_shader_binding,        r->uber_geom_module },
            { &r->uber_shader_binding_quad,            r->uber_geom_quad_module },
            { &r->uber_shader_binding_quad_wireframe,  r->uber_geom_quad_wireframe_module },
            { &r->uber_shader_binding_line,            r->uber_geom_line_module },
            { &r->uber_shader_binding_points,          r->uber_geom_points_module },
        };
        for (int i = 0; i < ARRAY_SIZE(vk_variants); i++) {
            ShaderBinding *b = vk_variants[i].binding;
            memset(b, 0, sizeof(*b));
            b->vsh.module_info = r->uber_vsh_module;
            b->geom.module_info = vk_variants[i].geom_mod;
            b->psh.module_info = r->uber_psh_module;
            update_shader_uniform_locs(b);
        }
        if (r->geom_passthrough_enabled) {
            ShaderBinding *b = &r->uber_shader_binding_tri_pass;
            memset(b, 0, sizeof(*b));
            b->vsh.module_info = r->uber_vsh_module;
            b->geom.module_info = r->uber_geom_tri_pass_module;
            b->psh.module_info = r->uber_psh_module;
            update_shader_uniform_locs(b);
        }

        fprintf(stderr,
                "nv2a: Vulkan ubershaders %s (tri/quad/quad_wf/line/points)\n",
                from_cache ? "loaded from cache" : "compiled and cached");
        xemu_queue_notification(from_cache
            ? "Vulkan ubershaders loaded from cache"
            : "Vulkan ubershaders compiled and ready");
    }
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    uber_batched_finalize(r);

    if (r->uber_active) {
        pgraph_vk_unref_shader_module(r, r->uber_vsh_module);
        pgraph_vk_unref_shader_module(r, r->uber_geom_module);
        pgraph_vk_unref_shader_module(r, r->uber_geom_quad_module);
        pgraph_vk_unref_shader_module(r, r->uber_geom_quad_wireframe_module);
        pgraph_vk_unref_shader_module(r, r->uber_geom_line_module);
        pgraph_vk_unref_shader_module(r, r->uber_geom_points_module);
        if (r->uber_geom_tri_pass_module) {
            pgraph_vk_unref_shader_module(r, r->uber_geom_tri_pass_module);
            r->uber_geom_tri_pass_module = NULL;
        }
        pgraph_vk_unref_shader_module(r, r->uber_psh_module);
        r->uber_vsh_module = NULL;
        r->uber_geom_module = NULL;
        r->uber_geom_quad_module = NULL;
        r->uber_geom_quad_wireframe_module = NULL;
        r->uber_geom_line_module = NULL;
        r->uber_geom_points_module = NULL;
        r->uber_psh_module = NULL;
        r->uber_active = false;
    }

    if (!r->uber_active) {
        shader_cache_finalize(pg);
    }
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    pgraph_vk_finalize_glsl_compiler();
}
