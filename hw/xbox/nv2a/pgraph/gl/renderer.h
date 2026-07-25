/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
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

#ifndef HW_XBOX_NV2A_PGRAPH_GL_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_GL_RENDERER_H

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
#include "hw/xbox/nv2a/pgraph/glsl/ubershader.h"

#include "gloffscreen.h"
#include "constants.h"

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

    GLuint gl_buffer;
    SurfaceFormatInfo fmt;
} SurfaceBinding;

typedef struct TextureBinding {
    /* last-applied raw inputs of apply_texture_parameters —
     * object-accurate (the same binding can serve multiple stages with
     * different filter regs; per-stage shadows would desync from the GL
     * object). Equal inputs mean the apply is a deterministic no-op, so
     * skipping is identity. Uber-only fields; spec never reads them. */
    bool uber_tp_valid;
    bool uber_tp_bordered;
    unsigned int uber_tp_dim;
    uint32_t uber_tp_filter;
    uint32_t uber_tp_address;
    uint32_t uber_tp_border_color;
    uint32_t uber_tp_aniso;
    const void *uber_tp_finfo;
    unsigned int refcnt;
    int draw_time;
    uint64_t data_hash;
    unsigned int scale;
    unsigned int min_filter;
    unsigned int mag_filter;
    uint32_t lod_bias;
    unsigned int addru;
    unsigned int addrv;
    unsigned int addrp;
    uint32_t border_color;
    bool border_color_set;
    GLenum gl_target;
    GLuint gl_texture;
    /* shadow for the previously-unconditional
     * anisotropy parameter call (uber path only; spec ignores it). */
    uint32_t uber_max_anisotropy;
    /* level-0 dims of the GL_TEXTURE_2D allocation
     * (exactly what textureSize(samp,0) returns for this object), captured at
     * every glTexImage2D redefine; 0,0 for non-2D targets (never consumed -
     * normalize/convolution are rect/2D-only paths). */
    GLint uber_tex_size[2];
} TextureBinding;

typedef struct ShaderModuleCacheKey {
    GLenum kind;
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
    GLuint gl_shader;
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    bool initialized;

    bool cached;
    void *program;
    size_t program_size;
    GLenum program_format;
    ShaderState state;
    QemuThread *save_thread;

    GLuint gl_program;
    GLenum gl_primitive_mode;

    struct {
        PshUniformLocs psh;
        VshUniformLocs vsh;
    } uniform_locs;

    /* per-program last-applied family fingerprints (vsh
     * values / psh values / the 3 uber structs). On a hash miss only
     * the families whose subhash moved get re-applied — GL retains program
     * uniform state, so an equal fingerprint means this program already
     * holds those exact bytes. valid=false after a non-hashed apply (the
     * force-write path computes no fingerprints). Uber
     * bindings only; spec cache entries never read these. */
    uint64_t uber_vshv_hash;
    uint64_t uber_pshv_hash;
    uint64_t uber_structs_hash;
    bool uber_family_valid;
    /* c[] generation this program's uniform array last
     * absorbed. Equal to the renderer's uber_gl_c_gen means this program
     * already holds the current 192-reg contents and a binding switch can
     * skip the 3KB re-upload entirely (a naive binding switch would
     * re-upload on every switch, since the dirty array is global). */
    uint64_t uber_c_gen;
} ShaderBinding;

typedef struct VertexKey {
    size_t count;
    size_t stride;
    hwaddr addr;

    GLboolean gl_normalize;
    GLuint gl_type;
} VertexKey;

typedef struct VertexLruNode {
    LruNode node;
    VertexKey key;
    bool initialized;

    GLuint gl_buffer;
} VertexLruNode;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
} TextureKey;

typedef struct TextureLruNode {
    LruNode node;
    TextureKey key;
    TextureBinding *binding;
    bool possibly_dirty;
} TextureLruNode;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
    GLuint *queries;
} QueryReport;

typedef struct PGRAPHGLState {
    GLuint gl_framebuffer;
    GLuint gl_display_buffer;
    GLint gl_display_buffer_internal_format;
    GLsizei gl_display_buffer_width;
    GLsizei gl_display_buffer_height;
    GLenum gl_display_buffer_format;
    GLenum gl_display_buffer_type;

    Lru element_cache;
    VertexLruNode *element_cache_entries;
    GLuint gl_inline_array_buffer;
    GLuint gl_memory_buffer;
    GLuint gl_vertex_array;
    GLuint gl_inline_buffer[NV2A_VERTEXSHADER_ATTRIBUTES];

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    TextureBinding *texture_binding[NV2A_MAX_TEXTURES];
    Lru texture_cache;
    TextureLruNode *texture_cache_entries;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    QemuMutex shader_cache_lock;
    QemuThread shader_disk_thread;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    bool uber_active;
    GLuint uber_program;
    GLuint uber_program_tri_wireframe;
    GLuint uber_program_quad;
    GLuint uber_program_quad_wireframe;
    GLuint uber_program_line;
    GLuint uber_program_points;
    /* passthrough-GS smooth-tri variant (shared VSH
     * object + NV passthrough GS + the v_vtx*-in PSH variant). 0 when the
     * extension is unavailable or the link fails -> classic GS serves. */
    GLuint uber_program_tri_nogs;

    ShaderBinding uber_shader_binding;
    ShaderBinding uber_shader_binding_tri_wireframe;
    ShaderBinding uber_shader_binding_quad;
    ShaderBinding uber_shader_binding_quad_wireframe;
    ShaderBinding uber_shader_binding_line;
    ShaderBinding uber_shader_binding_points;
    ShaderBinding uber_shader_binding_tri_nogs;

    /* texture-bind churn shadows — skip repeated
     * disabled-stage unbinds and same-texture aux-unit rebinds. */
    bool uber_stage_unbound[NV2A_MAX_TEXTURES];
    GLuint uber_aux_cube_tex[NV2A_MAX_TEXTURES];
    GLuint uber_aux_3d_tex[NV2A_MAX_TEXTURES];

    /* value shadow for draw_begin's raw GL state calls. The
     * whole block is computed register-side into this struct and issued only
     * when the struct moved (one memcmp gates ~25 calls/begin); the one-time
     * constants (provoking vertex, depth clamp, point size, polygon-offset
     * disables, scissor enable) ride `valid`. Uber-gated; spec issues the
     * raw calls unconditionally as before. */
    struct UberGlBeginState {
        GLboolean cmask_r, cmask_g, cmask_b, cmask_a;
        GLboolean depth_mask;
        GLuint stencil_mask;
        bool blend_en;
        GLenum blend_sfactor, blend_dfactor, blend_equation;
        float blend_color[4];
        bool cull_en;
        GLenum cull_face, front_face;
        bool depth_test_en;
        GLenum depth_func;
        bool stencil_en;
        GLenum stencil_func;
        GLint stencil_ref;
        GLuint stencil_func_mask;
        GLenum stencil_op_fail, stencil_op_zfail, stencil_op_zpass;
        bool dither_en;
        bool line_smooth_en;
        float line_width;
        bool poly_smooth_en;
        GLenum polygon_mode;
        GLint vp_w, vp_h;
        GLint sc_x, sc_y, sc_w, sc_h;
    } uber_begin_state;
    bool uber_begin_state_valid;
    /* render-context GL_ARRAY_BUFFER tracking — collapses
     * the 44k/frame glBindBuffer calls in the attr loop (the quick-path
     * bind existed only so later glVertexAttribPointer calls capture the
     * right buffer). 0 = unknown. Display-context binds are a different
     * GL context and never touch this. */
    GLuint uber_gl_array_buf;

    /* Deferred multidraw merge (uber only). Draw-arrays flushes append their
     * ranges here instead of submitting. The first real GL mutation of any
     * kind (begin-state change, shader write/program switch, texture bind,
     * attr respecification, vertex-buffer upload, surface/clear/query/blit/
     * report/present activity, a non-arrays flush, a primitive-mode change, or
     * running out of capacity) submits the accumulated ranges first; at that
     * point the GL state still matches what the ranges were recorded under, so
     * draw order is preserved. */
    GLenum uber_md_mode;
    int uber_md_count;
    GLint uber_md_start[8192];
    GLsizei uber_md_counts[8192];

    /* snapshots backing the two-layer GL uber dirty-skip
     * (see uber_gl_texsurf_clean / uber_gl_values_clean / uber_gl_snapshot). */
    uint64_t uber_last_uniform_state_gen;
    /* last-consumed writer generations (see pgraph.h). */
    uint64_t uber_gl_seen_c_wgen;
    uint64_t uber_gl_seen_ltctx_wgen;
    uint32_t uber_last_alpha_ref;
    uint32_t uber_last_z_persp;
    uint32_t uber_last_setupraster;
    uint32_t uber_last_zoffsetbias;
    uint32_t uber_last_zoffsetfactor;
    unsigned int uber_last_surface_width;
    unsigned int uber_last_surface_height;
    /* c[] upload-bandwidth gate. uber_gl_c_gen advances once per consume-cycle
     * in which the global dirty array was seen non-empty (the clear in
     * uber_gl_snapshot pairs 1:1 with the observe); a binding whose uber_c_gen
     * matches needs no upload on a program switch. When the last-consuming
     * binding is re-bound at the next change-point, only the current dirty
     * min..max reg span is stale, uploaded as a sub-range if the driver laid
     * the array out in consecutive locations (checked once at link; otherwise
     * a full upload). */
    uint64_t uber_gl_c_gen;
    bool uber_gl_c_locs_consecutive;
    TextureBinding *uber_last_texture_binding[NV2A_MAX_TEXTURES];
    float uber_last_tex_scale[NV2A_MAX_TEXTURES];
    GLint uber_last_tex_size[NV2A_MAX_TEXTURES][2];

    /* GL port of the VK source-hash full upload-skip
     * (+ the structs-subhash and c[]-subhash shapes). The value
     * structs and the three uber structs are persistent so loc==-1 holes
     * stay zero from g_malloc0 and the hashed bytes are deterministic.
     * Lifetime matches the uber GL programs (both created once per renderer
     * init), so a baseline can never describe a deleted program. */
    VshUniformValues uber_gl_vsh_values;
    PshUniformValues uber_gl_psh_values;
    UberPshUniforms uber_gl_psh_cached;
    UberVshUniforms uber_gl_vsh_cached;
    UberGeomUniforms uber_gl_geom_cached;
    uint64_t uber_gl_structs_subhash;
    uint64_t uber_gl_c_subhash;
    bool uber_gl_c_hash_valid;
    uint64_t uber_gl_src_hash;
    bool uber_gl_src_hash_valid;
    int uber_gl_hash_credit;
    uint32_t uber_gl_hash_probe;

    /* per-attribute GL-state shadows — skip identical
     * respecifications (bind_vertex_attributes re-issued ~5.7 attr calls per
     * draw). One VAO exists; every attr-state write site
     * goes through the pgraph_gl_uber_attr_* helpers, so the shadow is
     * faithful. Uber-gated inside the helpers; spec issues raw calls. */
    struct UberGlAttrShadow {
        bool valid;
        bool enabled;     /* array enabled vs generic-value mode */
        GLuint buffer;    /* GL_ARRAY_BUFFER bound at pointer time */
        GLint count;      /* incl. GL_BGRA */
        GLenum type;
        GLboolean normalize;
        bool ipointer;
        GLsizei stride;
        uintptr_t offset;
        float generic[4]; /* the glVertexAttrib4fv value when disabled */
    } uber_gl_attr_shadow[NV2A_VERTEXSHADER_ATTRIBUTES];

    struct UberPshLocs {
        GLint combiner_control;
        GLint final_inputs_0, final_inputs_1;
        GLint rgb_inputs, rgb_outputs, alpha_inputs, alpha_outputs;
        GLint uber_flags, alpha_func, shadow_depth_func, depth_format;
        GLint tex_modes, tex_flags, colorkey_mode;
        GLint tex_size;
        GLint border_logical_size, border_inv_real_size;
        GLint compare_mode_packed, dot_map, input_tex, conv_tex;
    } uber_psh_locs, uber_psh_locs_tri_wireframe,
      uber_psh_locs_quad, uber_psh_locs_quad_wireframe,
      uber_psh_locs_line, uber_psh_locs_points,
      uber_psh_locs_tri_nogs;
    struct UberVshLocs {
        GLint uber_vsh_flags, skinning_mode, fog_mode, foggen_mode;
        GLint light_enable_packed, texgen_packed, mat_color_src_packed;
        GLint uniform_attrs, compressed_attrs, swizzle_attrs;
        GLint point_size, point_params, surface_scale_factor;
        GLint program_length, program_data;
    } uber_vsh_locs, uber_vsh_locs_tri_wireframe,
      uber_vsh_locs_quad, uber_vsh_locs_quad_wireframe,
      uber_vsh_locs_line, uber_vsh_locs_points,
      uber_vsh_locs_tri_nogs;
    struct UberGeomLocs {
        GLint primitive_mode, polygon_front_mode, polygon_back_mode;
        GLint uber_geom_flags, tri_rot0, tri_rot1;
    } uber_geom_locs, uber_geom_locs_tri_wireframe,
      uber_geom_locs_quad, uber_geom_locs_quad_wireframe,
      uber_geom_locs_line, uber_geom_locs_points,
      uber_geom_locs_tri_nogs; /* pt GS uniforms */

    unsigned int zpass_pixel_count_result;
    unsigned int gl_zpass_pixel_count_query_count;
    GLuint *gl_zpass_pixel_count_queries;
    QSIMPLEQ_HEAD(, QueryReport) report_queue;

    bool shader_cache_writeback_pending;
    QemuEvent shader_cache_writeback_complete;

    struct s2t_rndr {
        GLuint fbo, vao, vbo, prog;
        GLuint tex_loc, surface_size_loc;
    } s2t_rndr;

    struct disp_rndr {
        GLuint fbo, vao, vbo, prog;
        GLuint display_size_loc;
        GLuint line_offset_loc;
        GLuint tex_loc;
        GLuint pvideo_tex;
        GLint pvideo_enable_loc;
        GLint pvideo_tex_loc;
        GLint pvideo_in_pos_loc;
        GLint pvideo_pos_loc;
        GLint pvideo_scale_loc;
        GLint pvideo_color_key_enable_loc;
        GLint pvideo_color_key_loc;
        GLint palette_loc[256];
    } disp_rndr;

    GLfloat supported_aliased_line_width_range[2];
    GLfloat supported_smooth_line_width_range[2];

    struct supported_extensions {
        GLboolean texture_filter_anisotropic;
    } supported_extensions;
} PGRAPHGLState;

extern GloContext *g_nv2a_context_render;
extern GloContext *g_nv2a_context_display;

unsigned int pgraph_gl_bind_inline_array(NV2AState *d);
/* shadow-aware attr-state writers (skip identical respecs) */
/* submit any deferred multidraw ranges. Call before the
 * first real GL mutation on every path that can change state the deferred
 * draw reads, read the framebuffer, or reorder against other draw arms. */
static inline void pgraph_gl_uber_bind_array_buffer(PGRAPHGLState *r,
                                                    GLuint buf)
{
    /* skip-identical ARRAY_BUFFER binds (render context).
     * Spec path always binds raw (tracking parked at unknown). */
    if (r->uber_active && r->uber_gl_array_buf == buf) {
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    r->uber_gl_array_buf = r->uber_active ? buf : 0;
}

static inline void pgraph_gl_uber_submit_md(PGRAPHGLState *r)
{
    if (r->uber_md_count) {
        glMultiDrawArrays(r->uber_md_mode, r->uber_md_start,
                          r->uber_md_counts, r->uber_md_count);
        r->uber_md_count = 0;
    }
}

void pgraph_gl_uber_attr_disable_generic(PGRAPHGLState *r, int i,
                                         const float value[4]);
void pgraph_gl_uber_attr_pointer(PGRAPHGLState *r, int i, GLuint buffer,
                                 GLint count, GLenum type,
                                 GLboolean normalize, bool ipointer,
                                 GLsizei stride, uintptr_t offset);
void pgraph_gl_bind_shaders(PGRAPHState *pg);
void pgraph_gl_bind_textures(NV2AState *d);
void pgraph_gl_bind_vertex_attributes(NV2AState *d, unsigned int min_element, unsigned int max_element, bool inline_data, unsigned int inline_stride, unsigned int provoking_element);
bool pgraph_gl_check_surface_to_texture_compatibility(const SurfaceBinding *surface, const TextureShape *shape);
GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src);
void pgraph_gl_download_dirty_surfaces(NV2AState *d);
void pgraph_gl_clear_report_value(NV2AState *d);
void pgraph_gl_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_gl_draw_begin(NV2AState *d);
void pgraph_gl_draw_end(NV2AState *d);
void pgraph_gl_flush_draw(NV2AState *d);
void pgraph_gl_get_report(NV2AState *d, uint32_t parameter);
void pgraph_gl_image_blit(NV2AState *d);
void pgraph_gl_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr, hwaddr size);
void pgraph_gl_process_pending_reports(NV2AState *d);
void pgraph_gl_surface_flush(NV2AState *d);
void pgraph_gl_surface_update(NV2AState *d, bool upload, bool color_write, bool zeta_write);
void pgraph_gl_sync(NV2AState *d);
void pgraph_gl_update_entire_memory_buffer(NV2AState *d);
void pgraph_gl_init_display(NV2AState *d);
void pgraph_gl_finalize_display(PGRAPHState *pg);
void pgraph_gl_init_reports(NV2AState *d);
void pgraph_gl_finalize_reports(PGRAPHState *pg);
void pgraph_gl_init_shaders(PGRAPHState *pg);
void pgraph_gl_finalize_shaders(PGRAPHState *pg);
void pgraph_gl_init_surfaces(PGRAPHState *pg);
void pgraph_gl_finalize_surfaces(PGRAPHState *pg);
void pgraph_gl_init_textures(NV2AState *d);
void pgraph_gl_finalize_textures(PGRAPHState *pg);
void pgraph_gl_init_buffers(NV2AState *d);
void pgraph_gl_finalize_buffers(PGRAPHState *pg);
void pgraph_gl_process_pending_downloads(NV2AState *d);
void pgraph_gl_reload_surface_scale_factor(PGRAPHState *pg);
void pgraph_gl_render_surface_to_texture(NV2AState *d, SurfaceBinding *surface, TextureBinding *texture, TextureShape *texture_shape, int texture_unit);
void pgraph_gl_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_gl_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
SurfaceBinding *pgraph_gl_surface_get(NV2AState *d, hwaddr addr);
SurfaceBinding *pgraph_gl_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_gl_surface_invalidate(NV2AState *d, SurfaceBinding *e);
void pgraph_gl_unbind_surface(NV2AState *d, bool color);
void pgraph_gl_upload_surface_data(NV2AState *d, SurfaceBinding *surface, bool force);
void pgraph_gl_shader_cache_to_disk(ShaderBinding *snode);
bool pgraph_gl_shader_load_from_memory(ShaderBinding *snode);
void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg);
void pgraph_gl_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_gl_get_surface_scale_factor(NV2AState *d);
int pgraph_gl_get_framebuffer_surface(NV2AState *d);
/**  Note: The caller must set up a clean GL context before invoking. */
void pgraph_gl_determine_gpu_properties(void);
GPUProperties *pgraph_gl_get_gpu_properties(void);

#endif
