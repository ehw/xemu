/*
 * Geforce NV2A PGRAPH GLSL Ubershader Generator
 *
 * Copyright (c) 2025
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

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_UBERSHADER_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_UBERSHADER_H

/* Ubershader version, embedded in the generated GLSL and in the on-disk shader
 * cache so a cache written by a different build is rejected. Distinct from the
 * GLSL #version directive; bump it whenever the generated shaders change. */
#define UBERSHADER_VERSION 1

#include "common.h"
#include "psh.h"
#include "vsh.h"
#include "geom.h"

typedef struct PGRAPHState PGRAPHState;

typedef struct UberPshUniforms {
    uint32_t combiner_control;
    uint32_t shader_stage_program;
    uint32_t other_stage_input;
    uint32_t final_inputs_0;
    uint32_t final_inputs_1;

    uint32_t rgb_inputs[8];
    uint32_t rgb_outputs[8];
    uint32_t alpha_inputs[8];
    uint32_t alpha_outputs[8];

    uint32_t uber_flags;

    int32_t alpha_func;
    int32_t shadow_depth_func;
    int32_t depth_format;

    uint32_t tex_modes;
    uint32_t tex_flags[4];
    int32_t  colorkey_mode[4];

    float    border_logical_size[4][4];
    float    border_inv_real_size[4][4];
    uint32_t compare_mode_packed;
    int32_t  dot_map[4];
    int32_t  input_tex[4];
    int32_t  conv_tex[4];
    /* per-stage level-0 texture dims for the VK 2D slot
     * (mirrors the descriptor's real-vs-dummy choice). Filled VK-side after
     * the agnostic setter; the GL arm keeps textureSize() and ignores it. */
    int32_t  tex_size[4][2];
} UberPshUniforms;

#define UBER_FLAG_ALPHA_TEST          (1u << 0)
#define UBER_FLAG_WINDOW_CLIP_EXCL    (1u << 1)
#define UBER_FLAG_SMOOTH_SHADING      (1u << 2)
#define UBER_FLAG_DEPTH_CLIPPING      (1u << 3)
#define UBER_FLAG_Z_PERSPECTIVE       (1u << 4)
#define UBER_FLAG_POINT_SPRITE        (1u << 5)

#define UBER_TEX_FLAG_RECT            (1u << 0)
#define UBER_TEX_FLAG_ALPHAKILL       (1u << 2)
#define UBER_TEX_FLAG_X8Y24           (1u << 3)
#define UBER_TEX_FLAG_CUBEMAP         (1u << 4)
#define UBER_TEX_FLAG_SHADOW          (1u << 5)
#define UBER_TEX_FLAG_3D              (1u << 6)

typedef struct UberVshUniforms {
    uint32_t uber_vsh_flags;
    int32_t  skinning_mode;
    int32_t  fog_mode;
    int32_t  foggen_mode;
    uint32_t light_enable_packed;
    uint32_t texgen_packed[4];
    uint32_t mat_color_src_packed;
    uint32_t uniform_attrs;
    uint32_t compressed_attrs;
    uint32_t swizzle_attrs;
    float    point_size;
    float    point_params[8];
    int32_t  surface_scale_factor;
    int32_t  program_length;
    uint32_t program_data[136 * 3]; /* w0 never decoded */
} UberVshUniforms;

#define UBER_VSH_FLAG_IS_FF         (1u << 0)
#define UBER_VSH_FLAG_FOG_ENABLE    (1u << 1)
#define UBER_VSH_FLAG_SPEC_ENABLE   (1u << 2)
#define UBER_VSH_FLAG_SEP_SPEC      (1u << 3)
#define UBER_VSH_FLAG_IGN_SPEC_A    (1u << 4)
#define UBER_VSH_FLAG_PT_PARAMS     (1u << 5)
#define UBER_VSH_FLAG_SMOOTH        (1u << 6)
#define UBER_VSH_FLAG_Z_PERSP       (1u << 7)
#define UBER_VSH_FLAG_NORMALIZE     (1u << 8)
#define UBER_VSH_FLAG_LIGHTING      (1u << 9)
#define UBER_VSH_FLAG_LOCAL_EYE     (1u << 10)
#define UBER_VSH_FLAG_TEXMAT0       (1u << 11)
#define UBER_VSH_FLAG_TEXMAT1       (1u << 12)
#define UBER_VSH_FLAG_TEXMAT2       (1u << 13)
#define UBER_VSH_FLAG_TEXMAT3       (1u << 14)

typedef struct UberGeomUniforms {
    int32_t primitive_mode;
    int32_t polygon_front_mode;
    int32_t polygon_back_mode;
    uint32_t uber_geom_flags;
    int32_t tri_rot0;
    int32_t tri_rot1;
} UberGeomUniforms;

#define UBER_GEOM_FLAG_SMOOTH         (1u << 0)
#define UBER_GEOM_FLAG_FIRST_PROVOKING (1u << 1)
#define UBER_GEOM_FLAG_Z_PERSP        (1u << 2)

typedef struct GenUberGlslOptions {
    bool vulkan;
    /* emit the uber UBO blocks with std430 layout (kills the
     * std140 16B-per-element array padding; u_program_data[408] 6528B->1632B).
     * Vulkan-only; requires uniformBufferStandardLayout/scalarBlockLayout. */
    bool ubo_std430;
    int psh_ubo_binding;
    int tex_binding;
    int uber_psh_ubo_binding;
    int uber_vsh_ubo_binding;
    int uber_geom_ubo_binding;
    /* GL-only, PSH-only: generate the GS-less smooth-triangle PSH variant. The
     * program links the unmodified shared VSH object (so the FMA-sensitive
     * transform math stays bit-identical) directly to this PSH, with no
     * geometry stage. The PSH declares its 9 inputs with the v_vtx* names (the
     * body is aliased by #define; the VSH's GS-only outputs are left unmatched,
     * which is legal) and derives zvalue/triMZ from gl_FragCoord rather than the
     * explicit barycentric plane. That can differ by a single depth LSB in rare
     * cases, but avoids NVIDIA's GL geometry-shader slow path. */
    bool gl_nogs_tri;
    /* (VK cross-draw batching): per-draw records indexed by
     * gl_DrawID select texture slots in runtime descriptor arrays (set 2);
     * the draw index rides VSH->GS->PSH as an appended-last flat varying.
     * Dead until the CPU side lights it. */
    bool vk_batched;
} GenUberGlslOptions;

MString *pgraph_glsl_gen_uber_psh(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_vsh(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom_tri_passthrough(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom_tri_wireframe(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom_quad(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom_quad_wireframe(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom_line(GenUberGlslOptions opts);
MString *pgraph_glsl_gen_uber_geom_points(GenUberGlslOptions opts);

void pgraph_uber_set_psh_uniforms(PGRAPHState *pg, UberPshUniforms *u);
void pgraph_uber_set_vsh_uniforms(PGRAPHState *pg, const VshState *vsh,
                                  UberVshUniforms *u);
void pgraph_uber_set_geom_uniforms(PGRAPHState *pg, const GeomState *geom,
                                   UberGeomUniforms *u);

#endif
