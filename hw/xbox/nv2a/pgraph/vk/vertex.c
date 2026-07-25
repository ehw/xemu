/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "renderer.h"

VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    return pgraph_vk_append_to_buffer(pg, BUFFER_INDEX_STAGING, &data, &size, 1,
                                      1);
}

VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
    return pgraph_vk_append_to_buffer(pg, BUFFER_VERTEX_INLINE_STAGING, data,
                                      sizes, count, 1);
}

void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                        void *data, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_download_surfaces_in_range_if_dirty(pg, offset, size);

    size_t start_bit = offset / TARGET_PAGE_SIZE;
    size_t end_bit = TARGET_PAGE_ALIGN(offset + size) / TARGET_PAGE_SIZE;
    size_t nbits = end_bit - start_bit;

    if (find_next_bit(r->uploaded_bitmap, start_bit + nbits, start_bit) <
        end_bit) {
        // Vertex data changed while building the draw list. Finish drawing
        // before updating RAM buffer.
        pgraph_vk_finish(pg, VK_FINISH_REASON_VERTEX_BUFFER_DIRTY);
    }

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    memcpy(r->storage_buffers[BUFFER_VERTEX_RAM].mapped + offset, data, size);

    bitmap_set(r->uploaded_bitmap, start_bit, nbits);
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(r->vertex_ram_buffer_syncs));
    r->vertex_ram_buffer_syncs[r->num_vertex_ram_buffer_syncs++] =
        (MemorySyncRequirement){ .addr = addr, .size = size };
}

static const VkFormat float_to_count[] = {
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,
};

static const VkFormat ub_to_count[] = {
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_R8G8B8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
};

static const VkFormat s1_to_count[] = {
    VK_FORMAT_R16_SNORM,
    VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16B16_SNORM,
    VK_FORMAT_R16G16B16A16_SNORM,
};

static const VkFormat s32k_to_count[] = {
    VK_FORMAT_R16_SSCALED,
    VK_FORMAT_R16G16_SSCALED,
    VK_FORMAT_R16G16B16_SSCALED,
    VK_FORMAT_R16G16B16A16_SSCALED,
};

static char const * const vertex_data_array_format_to_str[] = {
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D] = "UB_D3D",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL] = "UB_OGL",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1] = "S1",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F] = "F",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K] = "S32K",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP] = "CMP",
};

/* FNV-1a over the exact descriptions
 * vkCmdSetVertexInputEXT consumes (binding + attribute descriptions + counts).
 * both the array and the inline rebuild paths must keep r->vids_format_hash in
 * sync with the descriptions they install: an inline-buffer draw SETs the inline
 * vertex-input on the GPU (begin_draw, vids_cache_valid=true) but historically
 * left vids_format_hash at the last *array* format, so a following same-format
 * array draw matched the stale hash, skipped the needed re-SET and rendered with
 * the inline vertex-input (broke the nxdk Lighting-spotlight cluster). */
static uint64_t vids_compute_format_hash(PGRAPHVkState *r)
{
    uint64_t vh = 1469598103934665603ULL;
    const uint8_t *vhp = (const uint8_t *)r->vertex_binding_descriptions;
    size_t vhn = r->num_active_vertex_binding_descriptions *
                 sizeof(r->vertex_binding_descriptions[0]);
    for (size_t b = 0; b < vhn; b++) {
        vh = (vh ^ vhp[b]) * 1099511628211ULL;
    }
    vhp = (const uint8_t *)r->vertex_attribute_descriptions;
    vhn = r->num_active_vertex_attribute_descriptions *
          sizeof(r->vertex_attribute_descriptions[0]);
    for (size_t b = 0; b < vhn; b++) {
        vh = (vh ^ vhp[b]) * 1099511628211ULL;
    }
    vh ^= ((uint64_t)r->num_active_vertex_binding_descriptions << 8) ^
          r->num_active_vertex_attribute_descriptions;
    return vh;
}

void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int num_elements = max_element - min_element + 1;

    if (inline_data) {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;
    r->vstream_mask = 0;
    /* the vids invalidation moved to the end of the rebuild and
     * is now conditional on the format hash (was: unconditional here, which
     * forced a vkCmdSetVertexInputEXT every draw and defeated the cache). */

    /* memoize the (at most 2) vertex DMA maps for this draw.
     * nv_dma_map -> nv_dma_load reads the DMA object from guest memory and was
     * called per attribute for only dma_vertex_a/b; it is
     * pure, so resolving each once per draw is parity-identical. uber-only. */
    uint8_t *dma_ptrs[2] = { NULL, NULL };
    hwaddr dma_lens[2] = { 0, 0 };
    bool dma_valid[2] = { false, false };

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        NV2A_VK_DGROUP_BEGIN("[attr %02d] format=%s, count=%d, stride=%d", i,
                             vertex_data_array_format_to_str[attr->format],
                             attr->count, attr->stride);
        r->vertex_attribute_to_description_location[i] = -1;
        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        VkFormat vk_format;
        bool needs_conversion = false;
        bool d3d_swizzle = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            assert(attr->count == 4);
            d3d_swizzle = true;
            /* fallthru */
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            assert(attr->count <= ARRAY_SIZE(ub_to_count));
            vk_format = ub_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            assert(attr->count <= ARRAY_SIZE(s1_to_count));
            vk_format = s1_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            assert(attr->count <= ARRAY_SIZE(float_to_count));
            vk_format = float_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            assert(attr->count <= ARRAY_SIZE(s32k_to_count));
            vk_format = s32k_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            vk_format =
                VK_FORMAT_R32_SINT; // VK_FORMAT_B10G11R11_UFLOAT_PACK32 ??
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;

        hwaddr start = 0;
        bool vstream_streamed = false;
        size_t vstream_off = 0;
        if (inline_data) {
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data;
            if (r->uber_active) { /* per-draw DMA-map memoization */
                int sel = attr->dma_select ? 1 : 0;
                if (!dma_valid[sel]) {
                    dma_ptrs[sel] = (uint8_t *)nv_dma_map(
                        d, sel ? pg->dma_vertex_b : pg->dma_vertex_a,
                        &dma_lens[sel]);
                    dma_valid[sel] = true;
                }
                attr_data = dma_ptrs[sel];
                dma_len = dma_lens[sel];
            } else {
                attr_data = (uint8_t *)nv_dma_map(
                    d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                    &dma_len);
            }
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            start = attrib_data_addr + min_element * stride;
            /* if these vertex bytes are still in-flight (a prior
             * draw reads them), stream the NEW copy to the ring + bind there
             * (offset set below) instead of forcing a GPU sync; otherwise queue
             * the normal mirror upload. Exact per-attribute key = no ambiguity. */
            VkDeviceSize vstream_sz = num_elements * stride;
            size_t vstream_sb = start / TARGET_PAGE_SIZE;
            size_t vstream_eb =
                TARGET_PAGE_ALIGN(start + vstream_sz) / TARGET_PAGE_SIZE;
            size_t vstream_al = (vstream_sz + 15) & ~(size_t)15;
            bool vstream_want =
                r->uber_active && r->vstream_enabled && stride && vstream_sz &&
                /* (Stencil REPLACE_ST/ZERO_ST): never stream an
                 * attr that needs CPU conversion — pgraph_vk_remap_attributes
                 * reads its source as d->vram_ptr + vertex_attribute_offsets[]
                 * (a guest-vram offset), but a STREAMED attr's offset is a
                 * ring-buffer offset; vram_ptr + ring_off reads past guest RAM →
                 * garbage converted vertices → corrupted geometry (the stencil-
                 * write pass drew the wrong shape). Converted attrs are read by
                 * the GPU from the remap SCRATCH buffer, so streaming them to the
                 * ring never helped — the ring copy was unused. Excluding them is
                 * a pure correctness win with no perf cost. */
                /* only stream an attr whose stride is 4-aligned: an unaligned
                 * attr is routed through the unaligned-attribute remap, whose
                 * copy reads d->vram_ptr + vertex_attribute_offsets[] (a guest
                 * offset), but a streamed attr's offset is a ring-buffer offset,
                 * so that read would land past guest RAM. A 4-aligned stride with
                 * the 16-aligned ring write position keeps the streamed offset
                 * aligned, so the attr is never remapped; unaligned attrs fall
                 * back to the normal mirror upload path. */
                (stride % 4 == 0) && !needs_conversion && !d3d_swizzle &&
                find_next_bit(r->uploaded_bitmap, vstream_eb, vstream_sb) < vstream_eb;
            if (vstream_want) {
                /* (a vertex-stream flicker case): the slow-path twin
                 * (pgraph_vk_update_vertex_ram_buffer) downloads dirty
                 * overlapping surfaces before reading guest RAM; the stream
                 * read must too, or it rings stale bytes while the real data
                 * still sits in a not-yet-downloaded surface (timing-
                 * dependent, so nondeterministic geometry corruption). The
                 * download can drain the GPU (clearing uploaded_bitmap), so
                 * re-test in-flight before committing to the ring. */
                pgraph_vk_download_surfaces_in_range_if_dirty(pg, start,
                                                              vstream_sz);
                vstream_want = find_next_bit(r->uploaded_bitmap, vstream_eb,
                                          vstream_sb) < vstream_eb;
            }
            if (vstream_want &&
                r->vertex_stream_wp + vstream_al <= r->vertex_stream_size &&
                r->vertex_stream_remap_count <
                    (int)ARRAY_SIZE(r->vertex_stream_remap)) {
                size_t sp = r->vertex_stream_base + r->vertex_stream_wp;
                memcpy(r->storage_buffers[BUFFER_VERTEX_RAM].mapped + sp,
                       d->vram_ptr + start, vstream_sz);
                r->vertex_stream_wp += vstream_al;
                int e = r->vertex_stream_remap_count++;
                r->vertex_stream_remap[e].region_start = start;
                r->vertex_stream_remap[e].size = vstream_sz;
                r->vertex_stream_remap[e].stream_pos = sp;
                vstream_off = sp - min_element * stride;
                vstream_streamed = true;
                /* stamp the ring epoch + remember the mirror
                 * fallback for the consume-site retarget. */
                r->vstream_mask |= 1u << i;
                r->vstream_gen[i] = r->vstream_finish_gen;
                r->vstream_mirror_offset[i] = attrib_data_addr;
                /* Also queue the 1:1 mirror refresh (a deferred
                 * MemorySyncRequirement — no GPU finish fires, since the in-flight
                 * readers are on the ring, not the mirror, so the sync-kill win
                 * is kept). Without this, a later same-window draw whose
                 * uploaded_bitmap was cleared by a surface download reads STALE
                 * mirror bytes (broke Stencil_REPLACE_ST/ZERO_ST). Supersedes the
                 * finish-time copyback, removed in draw.c. */
                update_memory_buffer(d, start, vstream_sz);
            } else {
                update_memory_buffer(d, start, vstream_sz);
            }
        }

        uint32_t provoking_element_index = provoking_element - min_element;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry =
                (uint8_t *)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pg->uniform_attrs |= 1 << i;
            pgraph_update_inline_value(attr, last_entry);
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        NV2A_VK_DPRINTF("offset = %08" HWADDR_PRIx, attrib_data_addr);
        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);

        r->vertex_attribute_to_description_location[i] =
            r->num_active_vertex_binding_descriptions;

        r->vertex_binding_descriptions
            [r->num_active_vertex_binding_descriptions++] =
            (VkVertexInputBindingDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .stride = stride,
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };

        r->vertex_attribute_descriptions
            [r->num_active_vertex_attribute_descriptions++] =
            (VkVertexInputAttributeDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .location = i,
                .format = vk_format,
            };

        r->vertex_attribute_offsets[i] =
            vstream_streamed ? vstream_off : attrib_data_addr;

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }
        if (d3d_swizzle) {
            pg->swizzle_attrs |= (1 << i);
        }

        NV2A_VK_DGROUP_END();
    }

    /* invalidate the vids dynamic-state cache only when the
     * active vertex-input format actually changed (FNV over the exact
     * descriptions vkCmdSetVertexInputEXT consumes). The per-render-pass
     * invalidation in begin_render_pass still forces a re-SET; the buffer
     * offsets ride vkCmdBindVertexBuffers, not this state. */
    {
        uint64_t vh = vids_compute_format_hash(r);
        if (vh != r->vids_format_hash) {
            r->vids_format_hash = vh;
            r->vids_cache_valid = false;
        }
    }

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;
    r->vids_cache_valid = false; /* descs are being rebuilt */

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->inline_buffer_populated) {
            r->vertex_attribute_to_description_location[i] =
                r->num_active_vertex_binding_descriptions;
            r->vertex_binding_descriptions
                [r->num_active_vertex_binding_descriptions++] =
                (VkVertexInputBindingDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .stride = 4 * sizeof(float),
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                };
            r->vertex_attribute_descriptions
                [r->num_active_vertex_attribute_descriptions++] =
                (VkVertexInputAttributeDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .location = i,
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                };
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            r->vertex_attribute_to_description_location[i] = -1;
            pg->uniform_attrs |= 1 << i;
        }
    }

    /* keep vids_format_hash in sync with the inline format
     * installed above. begin_draw will SET this inline vertex-input on the GPU
     * (vids_cache_valid was cleared at entry); recording the hash here makes the
     * following array draw re-SET on the inline->array format change instead of
     * skipping on a stale hash and rendering with the inline vertex-input. */
    r->vids_format_hash = vids_compute_format_hash(r);
}
