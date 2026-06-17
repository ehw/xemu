/*
 * Geforce NV2A PGRAPH GLSL Ubershader Generator
 *
 * Generates a single "uber" fragment, vertex, and geometry shader that handles
 * all NV2A rendering states dynamically via uniforms, eliminating per-state
 * shader compilation stalls. Combiner stage configuration, texture modes,
 * alpha test, and vertex program instructions are all passed as uniform data
 * and evaluated at runtime using loops and branches.
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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "ubershader.h"
#include "psh.h"
#include "vsh.h"
#include "geom.h"

/* shared shader prologue. std430 layout on the uber UBO
 * blocks (kills the std140 16B-per-array-element padding; u_program_data[408]
 * 6528B->1632B) must declare GL_EXT_scalar_block_layout before any block. */
static void uber_glsl_prologue(MString *s, GenUberGlslOptions opts)
{
    /* (bit-exact): the GL no-GS tri PSH needs GLSL 4.50 +
     * GL_NV_fragment_shader_barycentric (pervertexNV) to read all 3 triangle
     * vertices in the fragment shader and run the GS's exact depth math. */
    int ver = (opts.vulkan || opts.gl_nogs_tri) ? 450 : 400;
    mstring_append_fmt(s, "#version %d\n// XEMU ubershader v%d\n", ver,
                       UBERSHADER_VERSION);
    if (opts.ubo_std430) {
        mstring_append(s, "#extension GL_EXT_scalar_block_layout : require\n");
    }
    if (opts.gl_nogs_tri) {
        mstring_append(s,
            "#extension GL_NV_fragment_shader_barycentric : require\n");
    }
}

static const char *uber_ubo_layout(GenUberGlslOptions opts)
{
    return opts.ubo_std430 ? "std430" : "std140";
}

/* ========================================================================
 * Fragment Ubershader
 * ======================================================================== */

static void uber_psh_append_uniforms(MString *s, GenUberGlslOptions opts)
{
    const char *u = opts.vulkan ? "" : "uniform ";

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = %d, %s) uniform PshUniforms {\n",
            opts.psh_ubo_binding, uber_ubo_layout(opts));
    }

    for (int i = 0; i < PshUniform__COUNT; i++) {
        const UniformInfo *info = &PshUniformInfo[i];
        const char *type_str = uniform_element_type_to_str[info->type];
        if (info->count == 1) {
            mstring_append_fmt(s, "%s%s %s;\n", u, type_str, info->name);
        } else {
            mstring_append_fmt(s, "%s%s %s[%zd];\n", u, type_str, info->name,
                               info->count);
        }
    }

    if (opts.vulkan) {
        mstring_append(s,
            /* u_shader_stage_program / u_other_stage_input /
             * u_dim_tex / u_draw_id removed — declared but never read by the
             * GLSL (CPU-side struct fields remain as setter intermediates). */
            "uint u_combiner_control;\n"
            "uint u_final_inputs_0;\n"
            "uint u_final_inputs_1;\n"
            "uint u_rgb_inputs[8];\n"
            "uint u_rgb_outputs[8];\n"
            "uint u_alpha_inputs[8];\n"
            "uint u_alpha_outputs[8];\n"
            "uint u_uber_flags;\n"
            "int u_alpha_func;\n"
            "int u_shadow_depth_func;\n"
            "int u_depth_format;\n"
            "uint u_tex_modes;\n"
            "uint u_tex_flags[4];\n"
            "int u_colorkey_mode[4];\n"
            "vec4 u_border_logical_size[4];\n"
            "vec4 u_border_inv_real_size[4];\n"
            "uint u_compare_mode_packed;\n"
            "int u_dot_map[4];\n"
            "int u_input_tex[4];\n"
            "int u_conv_tex[4];\n"
            /* level-0 dims of each stage's bound 2D slot —
             * replaces the per-fragment textureSize() query (same ints). */
            "ivec2 u_tex_size[4];\n"
            "};\n");
    } else {
        mstring_append(s,
            "uniform uint u_combiner_control;\n" /* (4 removed) */
            "uniform uint u_final_inputs_0;\n"
            "uniform uint u_final_inputs_1;\n"
            "uniform uint u_rgb_inputs[8];\n"
            "uniform uint u_rgb_outputs[8];\n"
            "uniform uint u_alpha_inputs[8];\n"
            "uniform uint u_alpha_outputs[8];\n"
            "uniform uint u_uber_flags;\n"
            "uniform int u_alpha_func;\n"
            "uniform int u_shadow_depth_func;\n"
            "uniform int u_depth_format;\n"
            "uniform uint u_tex_modes;\n"
            "uniform uint u_tex_flags[4];\n"
            "uniform int u_colorkey_mode[4];\n"
            "uniform vec4 u_border_logical_size[4];\n"
            "uniform vec4 u_border_inv_real_size[4];\n"
            "uniform uint u_compare_mode_packed;\n"
            "uniform int u_dot_map[4];\n"
            "uniform int u_input_tex[4];\n"
            "uniform int u_conv_tex[4];\n"
            /* (the GL arm) */
            "uniform ivec2 u_tex_size[4];\n");
    }


}

static void uber_psh_append_helpers(MString *s)
{
    mstring_append(s,
        "float sign1(float x) {\n"
        "    x *= 255.0;\n"
        "    return (x-128.0)/127.0;\n"
        "}\n"
        "float sign2(float x) {\n"
        "    x *= 255.0;\n"
        "    if (x >= 128.0) return (x-255.5)/127.5;\n"
        "               else return (x+0.5)/127.5;\n"
        "}\n"
        "float sign3(float x) {\n"
        "    x *= 255.0;\n"
        "    if (x >= 128.0) return (x-256.0)/127.0;\n"
        "               else return (x)/127.0;\n"
        "}\n"
        "float sign3_to_0_to_1(float x) {\n"
        "    if (x >= 0) return x/2;\n"
        "           else return 1+x/2;\n"
        "}\n"
        "vec3 dotmap_func(int mode, vec4 col) {\n"
        "    if (mode == 0) return col.rgb;\n"
        "    if (mode == 1) return vec3(sign1(col.r),sign1(col.g),sign1(col.b));\n"
        "    if (mode == 2) return vec3(sign2(col.r),sign2(col.g),sign2(col.b));\n"
        "    if (mode == 3) return vec3(sign3(col.r),sign3(col.g),sign3(col.b));\n"
        "    if (mode == 4) {\n"
        "        uint hi_i = uint(col.a * float(0xff)) << 8 | uint(col.r * float(0xff));\n"
        "        uint lo_i = uint(col.g * float(0xff)) << 8 | uint(col.b * float(0xff));\n"
        "        return vec3(float(hi_i)/float(0xffff), float(lo_i)/float(0xffff), 1.0);\n"
        "    }\n"
        "    if (mode >= 5 && mode <= 7) return col.rgb;\n" // match spec stub dotmap_hilo_hemisphere* (FIXME)
        "    return col.rgb;\n"
        "}\n"
        "float kahan_det(vec2 a, vec2 b) {\n"
        "    precise float cd = a.y*b.x;\n"
        "    precise float err = fma(-a.y, b.x, cd);\n"
        "    precise float res = fma(a.x, b.y, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "float area(vec2 a, vec2 b, vec2 c) {\n"
        "    return kahan_det(b - a, c - a);\n"
        "}\n"
        "const float[9] gaussian3x3 = float[9](\n"
        "    1.0/16.0, 2.0/16.0, 1.0/16.0,\n"
        "    2.0/16.0, 4.0/16.0, 2.0/16.0,\n"
        "    1.0/16.0, 2.0/16.0, 1.0/16.0);\n"
        "const vec2[9] convolution3x3 = vec2[9](\n"
        "    vec2(-1,-1),vec2(0,-1),vec2(1,-1),\n"
        "    vec2(-1, 0),vec2(0, 0),vec2(1, 0),\n"
        "    vec2(-1, 1),vec2(0, 1),vec2(1, 1));\n"
        "vec2 remapCubeTo2D(vec3 tc) {\n"
        "    vec2 uv;\n"
        "    vec3 a = abs(tc);\n"
        "    if (a.x > a.y && a.x > a.z) {\n"
        "        uv = tc.x > 0.0 ? vec2(-tc.z, tc.y) : vec2(tc.z, tc.y);\n"
        "        uv /= a.x;\n"
        "    } else if (a.y > a.x && a.y > a.z) {\n"
        "        uv = tc.y > 0.0 ? vec2(tc.x, -tc.z) : vec2(tc.x, tc.z);\n"
        "        uv /= a.y;\n"
        "    } else {\n"
        "        uv = tc.z > 0.0 ? vec2(tc.x, tc.y) : vec2(-tc.x, tc.y);\n"
        "        uv /= a.z;\n"
        "    }\n"
        "    return uv;\n"
        "}\n"
        "vec3 remap2DToCube(vec3 tc) {\n"
        "    vec2 st = tc.xy / tc.z;\n"
        "    return normalize(vec3(1.0, st.y, -st.x));\n"
        "}\n"
        "bool check_color_key(vec4 texel, uint color_key, uint color_key_mask) {\n"
        "    uvec4 c = uvec4(texel * 255.0 + 0.5);\n"
        "    uint color = (c.a << 24) | (c.r << 16) | (c.g << 8) | c.b;\n"
        "    return (color & color_key_mask) == (color_key & color_key_mask);\n"
        "}\n"
    );
}

static void uber_psh_append_register_combiner(MString *s,
                                               GenUberGlslOptions opts)
{
    mstring_append(s,
        "vec4 uber_lo[8];\n"
        "vec4 uber_hi[8];\n"
        "void init_regs(vec4 d0, vec4 d1, vec4 fog, "
        "vec4 t0v, vec4 t1v, vec4 t2v, vec4 t3v) {\n"
        "    uber_lo[0] = vec4(0.0);\n"
        "    uber_lo[1] = vec4(0.0);\n"
        "    uber_lo[2] = vec4(0.0);\n"
        "    uber_lo[3] = fog;\n"
        "    uber_lo[4] = d0;\n"
        "    uber_lo[5] = d1;\n"
        "    uber_lo[6] = vec4(0.0);\n"
        "    uber_lo[7] = vec4(0.0);\n"
        "    uber_hi[0] = t0v;\n"
        "    uber_hi[1] = t1v;\n"
        "    uber_hi[2] = t2v;\n"
        "    uber_hi[3] = t3v;\n"
        "    uber_hi[4] = vec4(0.0);\n"
        "    uber_hi[5] = vec4(0.0);\n"
        "    uber_hi[6] = vec4(0.0);\n"
        "    uber_hi[7] = vec4(0.0);\n"
        "}\n"
        "\n"
    );

    mstring_append(s,
        "vec4 get_reg(int r) {\n"
        "    if (r == 0) return uber_lo[0];\n"
        "    if (r == 1) return uber_lo[1];\n"
        "    if (r == 2) return uber_lo[2];\n"
        "    if (r == 3) return uber_lo[3];\n"
        "    if (r == 4) return uber_lo[4];\n"
        "    if (r == 5) return uber_lo[5];\n"
        "    if (r == 6) return uber_lo[6];\n"
        "    if (r == 7) return uber_lo[7];\n"
        "    if (r == 8) return uber_hi[0];\n"
        "    if (r == 9) return uber_hi[1];\n"
        "    if (r == 10) return uber_hi[2];\n"
        "    if (r == 11) return uber_hi[3];\n"
        "    if (r == 12) return uber_hi[4];\n"
        "    if (r == 13) return uber_hi[5];\n"
        "    if (r == 14) return uber_hi[6];\n"
        "    if (r == 15) return uber_hi[7];\n"
        "    return vec4(0.0);\n"
        "}\n"
        "\n"
        /* component stores write IN PLACE — one chain per
         * store instead of get_reg + set_reg (untouched components are
         * preserved exactly as the old read-modify-write preserved them).
         * set_reg(vec4) had no remaining callers and is gone. */
        "void set_reg_rgb(int r, vec3 v) {\n"
        "    if (r == 1) uber_lo[1].rgb = v;\n"
        "    else if (r == 2) uber_lo[2].rgb = v;\n"
        "    else if (r == 3) uber_lo[3].rgb = v;\n"
        "    else if (r == 4) uber_lo[4].rgb = v;\n"
        "    else if (r == 5) uber_lo[5].rgb = v;\n"
        "    else if (r == 6) uber_lo[6].rgb = v;\n"
        "    else if (r == 7) uber_lo[7].rgb = v;\n"
        "    else if (r == 8) uber_hi[0].rgb = v;\n"
        "    else if (r == 9) uber_hi[1].rgb = v;\n"
        "    else if (r == 10) uber_hi[2].rgb = v;\n"
        "    else if (r == 11) uber_hi[3].rgb = v;\n"
        "    else if (r == 12) uber_hi[4].rgb = v;\n"
        "    else if (r == 13) uber_hi[5].rgb = v;\n"
        "}\n"
        "\n"
        "void set_reg_a(int r, float v) {\n"
        "    if (r == 1) uber_lo[1].a = v;\n"
        "    else if (r == 2) uber_lo[2].a = v;\n"
        "    else if (r == 3) uber_lo[3].a = v;\n"
        "    else if (r == 4) uber_lo[4].a = v;\n"
        "    else if (r == 5) uber_lo[5].a = v;\n"
        "    else if (r == 6) uber_lo[6].a = v;\n"
        "    else if (r == 7) uber_lo[7].a = v;\n"
        "    else if (r == 8) uber_hi[0].a = v;\n"
        "    else if (r == 9) uber_hi[1].a = v;\n"
        "    else if (r == 10) uber_hi[2].a = v;\n"
        "    else if (r == 11) uber_hi[3].a = v;\n"
        "    else if (r == 12) uber_hi[4].a = v;\n"
        "    else if (r == 13) uber_hi[5].a = v;\n"
        "}\n"
    );

    mstring_append(s,
        "\n"
        /* switch dispatch, byte-identical bodies (the
         * catalog's affine-LUT idea is invalid — mode 0x20 uses clamp). */
        "vec3 apply_input_mapping_rgb(vec3 v, int mapping) {\n"
        "    switch (mapping) {\n"
        "    case 0x00: return max(v, 0.0);\n"
        "    case 0x20: return 1.0 - clamp(v, 0.0, 1.0);\n"
        "    case 0x40: return 2.0 * max(v, 0.0) - 1.0;\n"
        "    case 0x60: return -2.0 * max(v, 0.0) + 1.0;\n"
        "    case 0x80: return max(v, 0.0) - 0.5;\n"
        "    case 0xA0: return -max(v, 0.0) + 0.5;\n"
        "    case 0xC0: return v;\n"
        "    case 0xE0: return -v;\n"
        "    default: return v;\n"
        "    }\n"
        "}\n"
        "\n"
        "float apply_input_mapping_a(float v, int mapping) {\n"
        "    switch (mapping) {\n"
        "    case 0x00: return max(v, 0.0);\n"
        "    case 0x20: return 1.0 - clamp(v, 0.0, 1.0);\n"
        "    case 0x40: return 2.0 * max(v, 0.0) - 1.0;\n"
        "    case 0x60: return -2.0 * max(v, 0.0) + 1.0;\n"
        "    case 0x80: return max(v, 0.0) - 0.5;\n"
        "    case 0xA0: return -max(v, 0.0) + 0.5;\n"
        "    case 0xC0: return v;\n"
        "    case 0xE0: return -v;\n"
        "    default: return v;\n"
        "    }\n"
        "}\n"
        "\n"
        "vec3 get_combiner_input_rgb(int reg_id, int chan, int mapping) {\n"
        "    vec4 rv = get_reg(reg_id);\n"
        "    vec3 v;\n"
        "    if (chan == 0x10) v = rv.aaa;\n"
        "    else v = rv.rgb;\n"
        "    return apply_input_mapping_rgb(v, mapping);\n"
        "}\n"
        "\n"
        "float get_combiner_input_a(int reg_id, int chan, int mapping) {\n"
        "    vec4 rv = get_reg(reg_id);\n"
        "    float v;\n"
        "    if (chan == 0x10) v = rv.a;\n"
        "    else v = rv.b;\n"
        "    return apply_input_mapping_a(v, mapping);\n"
        "}\n"
        "\n"
        /* switch dispatch (the sibling), bodies identical. */
        "vec3 apply_output_mapping_rgb(vec3 v, int mapping) {\n"
        "    switch (mapping) {\n"
        "    case 0x00: return v;\n"
        "    case 0x08: return v - 0.5;\n"
        "    case 0x10: return v * 2.0;\n"
        "    case 0x18: return (v - 0.5) * 2.0;\n"
        "    case 0x20: return v * 4.0;\n"
        "    case 0x30: return v / 2.0;\n"
        "    default: return v;\n"
        "    }\n"
        "}\n"
        "\n"
        "float apply_output_mapping_a(float v, int mapping) {\n"
        "    switch (mapping) {\n"
        "    case 0x00: return v;\n"
        "    case 0x08: return v - 0.5;\n"
        "    case 0x10: return v * 2.0;\n"
        "    case 0x18: return (v - 0.5) * 2.0;\n"
        "    case 0x20: return v * 4.0;\n"
        "    case 0x30: return v / 2.0;\n"
        "    default: return v;\n"
        "    }\n"
        "}\n"
        "\n"
    );

    mstring_append(s,
        "void run_combiner_stage(int stage, uint rgb_in, uint rgb_out,\n"
        "                        uint a_in, uint a_out,\n"
        "                        uint combiner_flags, bool mux_msb) {\n"
        "\n"
        "    bool unique_c0 = (combiner_flags & 0x10u) != 0u;\n"
        "    bool unique_c1 = (combiner_flags & 0x100u) != 0u;\n"
        /* reseed only when the seed can change (stage 0 or
         * unique constants). The slots can't be game-written in between:
         * a dest=C0/C1 write is inexpressible in valid content (spec would
         * emit a store into the consts[] UBO — illegal GLSL). */
        "    if (stage == 0 || unique_c0) {\n"
        "        int c0_idx = unique_c0 ? stage*2 : 0;\n"
        "        uber_lo[1] = consts[c0_idx];\n"
        "    }\n"
        "    if (stage == 0 || unique_c1) {\n"
        "        int c1_idx = unique_c1 ? stage*2+1 : 1;\n"
        "        uber_lo[2] = consts[c1_idx];\n"
        "    }\n"
        "\n"
        "    int ra_reg = int(rgb_in >> 24) & 0xF;\n"
        "    int ra_chan = int(rgb_in >> 24) & 0x10;\n"
        "    int ra_mod = int(rgb_in >> 24) & 0xE0;\n"
        "    int rb_reg = int(rgb_in >> 16) & 0xF;\n"
        "    int rb_chan = int(rgb_in >> 16) & 0x10;\n"
        "    int rb_mod = int(rgb_in >> 16) & 0xE0;\n"
        "    int rc_reg = int(rgb_in >> 8) & 0xF;\n"
        "    int rc_chan = int(rgb_in >> 8) & 0x10;\n"
        "    int rc_mod = int(rgb_in >> 8) & 0xE0;\n"
        "    int rd_reg = int(rgb_in) & 0xF;\n"
        "    int rd_chan = int(rgb_in) & 0x10;\n"
        "    int rd_mod = int(rgb_in) & 0xE0;\n"
        "\n"
        "    vec3 rA = get_combiner_input_rgb(ra_reg, ra_chan, ra_mod);\n"
        "    vec3 rB = get_combiner_input_rgb(rb_reg, rb_chan, rb_mod);\n"
        "    vec3 rC = get_combiner_input_rgb(rc_reg, rc_chan, rc_mod);\n"
        "    vec3 rD = get_combiner_input_rgb(rd_reg, rd_chan, rd_mod);\n"
        "\n"
        "    int aa_reg = int(a_in >> 24) & 0xF;\n"
        "    int aa_chan = int(a_in >> 24) & 0x10;\n"
        "    int aa_mod = int(a_in >> 24) & 0xE0;\n"
        "    int ab2_reg = int(a_in >> 16) & 0xF;\n"
        "    int ab2_chan = int(a_in >> 16) & 0x10;\n"
        "    int ab2_mod = int(a_in >> 16) & 0xE0;\n"
        "    int ac_reg = int(a_in >> 8) & 0xF;\n"
        "    int ac_chan = int(a_in >> 8) & 0x10;\n"
        "    int ac_mod = int(a_in >> 8) & 0xE0;\n"
        "    int ad_reg = int(a_in) & 0xF;\n"
        "    int ad_chan = int(a_in) & 0x10;\n"
        "    int ad_mod = int(a_in) & 0xE0;\n"
        "\n"
        "    float aA = get_combiner_input_a(aa_reg, aa_chan, aa_mod);\n"
        "    float aB = get_combiner_input_a(ab2_reg, ab2_chan, ab2_mod);\n"
        "    float aC = get_combiner_input_a(ac_reg, ac_chan, ac_mod);\n"
        "    float aD = get_combiner_input_a(ad_reg, ad_chan, ad_mod);\n"
        "\n"
        "    int rgb_flags = int(rgb_out >> 12);\n"
        "    int out_mapping = rgb_flags & 0x38;\n"
        "    bool ab_dot = (rgb_flags & 2) != 0;\n"
        "    bool cd_dot = (rgb_flags & 1) != 0;\n"
        "    bool mux_op = (rgb_flags & 4) != 0;\n"
        "\n"
        "    vec3 ab_rgb = ab_dot ? vec3(dot(rA, rB)) : (rA * rB);\n"
        "    vec3 cd_rgb = cd_dot ? vec3(dot(rC, rD)) : (rC * rD);\n"
        "\n"
        "    int ab_dest = int(rgb_out >> 4) & 0xF;\n"
        "    int cd_dest = int(rgb_out) & 0xF;\n"
        "    int mux_dest = int(rgb_out >> 8) & 0xF;\n"
        "\n"
        "    int a_flags = int(a_out >> 12);\n"
        "    int a_out_mapping = a_flags & 0x38;\n"
        "    bool a_mux_op = (a_flags & 4) != 0;\n"
        "\n"
        "    float a_ab = aA * aB;\n"
        "    float a_cd = aC * aD;\n"
        "\n"
        "    int a_ab_dest = int(a_out >> 4) & 0xF;\n"
        "    int a_cd_dest = int(a_out) & 0xF;\n"
        "    int a_mux_dest = int(a_out >> 8) & 0xF;\n"
        "\n"
        /* do_mux computed once for both RGB and alpha (the
         * two sites were identical; no register writes occur between them).
         * Only when a taken mux output consumes it —
         * still positioned before all register stores, so it reads the
         * same pre-store r0.a as always. */
        "    bool uber_do_mux = false;\n"
        "    if ((mux_op && mux_dest != 0) || (a_mux_op && a_mux_dest != 0)) {\n"
        "        uber_do_mux = mux_msb ? (uber_hi[4].a >= 0.5) : ((uint(uber_hi[4].a * 255.0) & 1u) == 1u);\n"
        "    }\n"
        "\n"
        /* each output's mapping+clamp computed only when its
         * dest is taken. Stored values are byte-identical expressions on
         * locals fixed above; store order is unchanged. */
        "    if (ab_dest != 0) {\n"
        "        vec3 ab_out = clamp(apply_output_mapping_rgb(ab_rgb, out_mapping), -1.0, 1.0);\n"
        "        set_reg_rgb(ab_dest, ab_out);\n"
        "        if ((rgb_flags & 0x80) != 0) set_reg_a(ab_dest, ab_out.b);\n"
        "    }\n"
        "    if (cd_dest != 0) {\n"
        "        vec3 cd_out = clamp(apply_output_mapping_rgb(cd_rgb, out_mapping), -1.0, 1.0);\n"
        "        set_reg_rgb(cd_dest, cd_out);\n"
        "        if ((rgb_flags & 0x40) != 0) set_reg_a(cd_dest, cd_out.b);\n"
        "    }\n"
        "    if (mux_dest != 0) {\n"
        "        vec3 mux_sum_rgb;\n"
        "        if (mux_op) {\n"
        "            mux_sum_rgb = uber_do_mux ? cd_rgb : ab_rgb;\n"
        "        } else {\n"
        "            mux_sum_rgb = ab_rgb + cd_rgb;\n"
        "        }\n"
        "        vec3 mux_out = clamp(apply_output_mapping_rgb(mux_sum_rgb, out_mapping), -1.0, 1.0);\n"
        "        set_reg_rgb(mux_dest, mux_out);\n"
        "    }\n"
        "\n"
        "    if (a_ab_dest != 0) {\n"
        "        float a_ab_out = clamp(apply_output_mapping_a(a_ab, a_out_mapping), -1.0, 1.0);\n"
        "        set_reg_a(a_ab_dest, a_ab_out);\n"
        "    }\n"
        "    if (a_cd_dest != 0) {\n"
        "        float a_cd_out = clamp(apply_output_mapping_a(a_cd, a_out_mapping), -1.0, 1.0);\n"
        "        set_reg_a(a_cd_dest, a_cd_out);\n"
        "    }\n"
        "    if (a_mux_dest != 0) {\n"
        "        float a_mux_sum;\n"
        "        if (a_mux_op) {\n"
        "            a_mux_sum = uber_do_mux ? a_cd : a_ab;\n"
        "        } else {\n"
        "            a_mux_sum = a_ab + a_cd;\n"
        "        }\n"
        "        float a_mux_out = clamp(apply_output_mapping_a(a_mux_sum, a_out_mapping), -1.0, 1.0);\n"
        "        set_reg_a(a_mux_dest, a_mux_out);\n"
        "    }\n"
        "}\n"
    );
}

static void uber_psh_append_final_combiner(MString *s, GenUberGlslOptions opts)
{
    mstring_append(s, "vec4 uber_final_combiner() {\n");
    mstring_append(s,
        "    uint fi0 = u_final_inputs_0;\n"
        "    uint fi1 = u_final_inputs_1;\n");
    /* re-roll (new-driver epoch): the V1R0 sum and EF product
     * feed only final-combiner input regs 0xE/0xF; when no A-D/G slot
     * references them (the common case) their computes + uber_hi stores are
     * dead — the final combiner is the last reader of uber_hi in the
     * invocation. E/F input reads are side-effect-free and stay relative to
     * the unconditional uber_lo[1]/[2] constant stores they may reference. */
    mstring_append(s,
        "    if (fi0 == 0u && fi1 == 0u) {\n"
        "        return uber_hi[4];\n"
        "    }\n"
        "\n"
        "    int fc_c0_idx = 16;\n"
        "    int fc_c1_idx = 17;\n"
        "    vec4 fc_c0 = consts[fc_c0_idx];\n"
        "    vec4 fc_c1 = consts[fc_c1_idx];\n"
        "    uber_lo[1] = fc_c0;\n"
        "    uber_lo[2] = fc_c1;\n"
        "\n"
        "    int a_reg = int(fi0 >> 24) & 0xF;\n"
        "    int a_chan = int(fi0 >> 24) & 0x10;\n"
        "    int a_mod = int(fi0 >> 24) & 0xE0;\n"
        "    int b_reg = int(fi0 >> 16) & 0xF;\n"
        "    int b_chan = int(fi0 >> 16) & 0x10;\n"
        "    int b_mod = int(fi0 >> 16) & 0xE0;\n"
        "    int c_reg = int(fi0 >> 8) & 0xF;\n"
        "    int c_chan = int(fi0 >> 8) & 0x10;\n"
        "    int c_mod = int(fi0 >> 8) & 0xE0;\n"
        "    int d_reg = int(fi0) & 0xF;\n"
        "    int d_chan = int(fi0) & 0x10;\n"
        "    int d_mod = int(fi0) & 0xE0;\n"
        "    int g_reg = int(fi1 >> 8) & 0xF;\n"
        "    int g_chan = int(fi1 >> 8) & 0x10;\n"
        "    int g_mod = int(fi1 >> 8) & 0xE0;\n"
        "\n"
        "    bool fc_need_sum = a_reg == 14 || b_reg == 14 || c_reg == 14 ||\n"
        "                       d_reg == 14 || g_reg == 14;\n"
        "    bool fc_need_ef = a_reg == 15 || b_reg == 15 || c_reg == 15 ||\n"
        "                      d_reg == 15 || g_reg == 15;\n"
        "\n"
        "    /* EF first: E/F input reads must see the PRE-final-combiner\n"
        "       uber_hi[6]/[7] (stage-8 leftovers), matching the unguarded\n"
        "       read-then-store order. */\n"
        "    if (fc_need_ef) {\n"
        "        int e_reg = int(fi1 >> 24) & 0xF;\n"
        "        int e_chan = int(fi1 >> 24) & 0x10;\n"
        "        int e_mod = int(fi1 >> 24) & 0xE0;\n"
        "        int f_reg = int(fi1 >> 16) & 0xF;\n"
        "        int f_chan = int(fi1 >> 16) & 0x10;\n"
        "        int f_mod = int(fi1 >> 16) & 0xE0;\n"
        "        vec3 E = get_combiner_input_rgb(e_reg, e_chan, e_mod);\n"
        "        vec3 F = get_combiner_input_rgb(f_reg, f_chan, f_mod);\n"
        "        uber_hi[7] = vec4(E * F, 0.0);\n"
        "    }\n"
        "\n"
        "    if (fc_need_sum) {\n"
        "        int fc_flags = int(fi1) & 0xFF;\n"
        "        bool clamp_sum = (fc_flags & 0x80) != 0;\n"
        "        bool inv_v1 = (fc_flags & 0x40) != 0;\n"
        "        bool inv_r0 = (fc_flags & 0x20) != 0;\n"
        "        vec4 v1r0_sum;\n"
        "        vec3 v1_val = inv_v1 ? (1.0 - uber_lo[5].rgb) : uber_lo[5].rgb;\n"
        "        vec3 r0_val = inv_r0 ? (1.0 - uber_hi[4].rgb) : uber_hi[4].rgb;\n"
        "        if (clamp_sum) {\n"
        "            v1r0_sum = clamp(vec4(v1_val + r0_val, 0.0), 0.0, 1.0);\n"
        "        } else {\n"
        "            v1r0_sum = vec4(v1_val + r0_val, 0.0);\n"
        "        }\n"
        "        uber_hi[6] = v1r0_sum;\n"
        "    }\n"
        "\n"
        "    vec3 A = get_combiner_input_rgb(a_reg, a_chan, a_mod);\n"
        "    vec3 B = get_combiner_input_rgb(b_reg, b_chan, b_mod);\n"
        "    vec3 C = get_combiner_input_rgb(c_reg, c_chan, c_mod);\n"
        "    vec3 D = get_combiner_input_rgb(d_reg, d_chan, d_mod);\n"
        "    float G = get_combiner_input_a(g_reg, g_chan, g_mod);\n"
        "\n"
        "    vec4 result;\n"
        "    result.rgb = D + mix(C, B, A);\n"
        "    result.a = G;\n"
        "    return result;\n"
        "}\n"
    );
}

static void uber_psh_append_texture_sampling(MString *s, GenUberGlslOptions opts)
{
    int cube_base = opts.uber_geom_ubo_binding + 1;
    int s3d_base = cube_base + 4;

    /* VK textures live in descriptor set 1 (UBOs stay in the
     * implicit set 0) so per-draw UBO churn doesn't rewrite them.
     * GL-only sampler arrays (VK keeps per-stage decls +
     * if-chain dispatch, rejected for the VK code generator). */
    if (opts.vulkan && opts.vk_batched) {
        /* per-draw records + runtime texture arrays. The
         * record index arrives as the appended-last flat varying; slots
         * are (2D|uint<<16) packed per stage. Indexing is dynamically
         * uniform per sub-draw (gl_DrawID), so no nonuniformEXT. */
        mstring_append(s,
            "#extension GL_EXT_nonuniform_qualifier : enable\n"
            "layout(set = 2, binding = 0, std430) readonly buffer UberRecs {\n"
            "    uvec4 uber_recs[];\n"
            "};\n"
            "layout(set = 2, binding = 1) uniform sampler2D uber_tex2d[];\n"
            "layout(set = 2, binding = 2) uniform samplerCube uber_texcube[];\n"
            "layout(set = 2, binding = 3) uniform sampler3D uber_tex3d[];\n"
            "layout(set = 2, binding = 4) uniform usampler2D uber_texuint[];\n"
            "int g_uberSlot[4];\n"
            "int g_uberSlotU[4];\n"
        );
    }
    if (opts.vulkan) { /* P8 PROBE: classic decls under batched too */
        for (int i = 0; i < 4; i++) {
            mstring_append_fmt(s, "layout(set = 1, binding = %d) "
                               "uniform sampler2D texSamp%d;\n",
                               opts.tex_binding + i, i);
        }
        for (int i = 0; i < 4; i++) {
            mstring_append_fmt(s, "layout(set = 1, binding = %d) "
                               "uniform samplerCube texSampCube%d;\n",
                               cube_base + i, i);
        }
        for (int i = 0; i < 4; i++) {
            mstring_append_fmt(s, "layout(set = 1, binding = %d) "
                               "uniform sampler3D texSamp3D%d;\n",
                               s3d_base + i, i);
        }
    } else if (!opts.vulkan) {
        mstring_append(s, "uniform sampler2D texSamp[4];\n");
        mstring_append(s, "uniform samplerCube texSampCube[4];\n");
        mstring_append(s, "uniform sampler3D texSamp3D[4];\n");
    }

    if (opts.vulkan) {
        int uint_base = s3d_base + 4;
        for (int i = 0; i < 4; i++) {
            mstring_append_fmt(
                s, "layout(set = 1, binding = %d) uniform usampler2D texSampUint%d;\n",
                uint_base + i, i);
        }
        mstring_append(s,
            "uvec4 uber_sample_tex_uint(int i, vec3 coord) {\n"
            "    vec2 tc = coord.xy / coord.z;\n");
        if (opts.vk_batched) {
            mstring_append(s,
                "    if (i == 0) return texture(uber_texuint[g_uberSlotU[0]], tc);\n"
                "    if (i == 1) return texture(uber_texuint[g_uberSlotU[1]], tc);\n"
                "    if (i == 2) return texture(uber_texuint[g_uberSlotU[2]], tc);\n"
                "    return texture(uber_texuint[g_uberSlotU[3]], tc);\n");
        } else {
            mstring_append(s,
                "    if (i == 0) return texture(texSampUint0, tc);\n"
                "    if (i == 1) return texture(texSampUint1, tc);\n"
                "    if (i == 2) return texture(texSampUint2, tc);\n"
                "    return texture(texSampUint3, tc);\n");
        }
        mstring_append(s, "}\n");
    }

    if (opts.vulkan) { /* P8 PROBE: classic helpers under batched */
        mstring_append(s,
            /* u_tex_size carries the exact ints textureSize()
             * returned (mirrors the descriptor's real-vs-dummy slot choice);
             * the expression keeps the identical implicit ivec2/float form,
             * so the math is bit-for-bit the same — minus the per-fragment
             * image query and the sampler if-chain. */
            "vec2 uber_normalize_tex(int i, vec2 coord) {\n"
            "    return coord / (u_tex_size[i] / texScale[i]);\n"
            "}\n"
            "\n"
            "vec4 uber_sample_tex(int i, vec3 coord) {\n"
        );
        if (opts.vk_batched) { /* unrolled constant slots;
            * textureProj kept — bit-identical to the classic form (the
            * earlier "Proj broken" reading was the stale-record artifact) */
            mstring_append(s,
                "    if (i == 0) return textureProj(uber_tex2d[g_uberSlot[0]], coord);\n"
                "    if (i == 1) return textureProj(uber_tex2d[g_uberSlot[1]], coord);\n"
                "    if (i == 2) return textureProj(uber_tex2d[g_uberSlot[2]], coord);\n"
                "    return textureProj(uber_tex2d[g_uberSlot[3]], coord);\n");
        } else {
            mstring_append(s,
                "    if (i == 0) return textureProj(texSamp0, coord);\n"
                "    if (i == 1) return textureProj(texSamp1, coord);\n"
                "    if (i == 2) return textureProj(texSamp2, coord);\n"
                "    return textureProj(texSamp3, coord);\n");
        }
        mstring_append(s,
            "}\n"
            "\n"
            "vec4 uber_sample_tex2d(int i, vec2 coord) {\n"
        );
        if (opts.vk_batched) { /* unrolled constant-component slots */
            mstring_append(s,
                "    if (i == 0) return texture(uber_tex2d[g_uberSlot[0]], coord);\n"
                "    if (i == 1) return texture(uber_tex2d[g_uberSlot[1]], coord);\n"
                "    if (i == 2) return texture(uber_tex2d[g_uberSlot[2]], coord);\n"
                "    return texture(uber_tex2d[g_uberSlot[3]], coord);\n");
        } else {
            mstring_append(s,
                "    if (i == 0) return texture(texSamp0, coord);\n"
                "    if (i == 1) return texture(texSamp1, coord);\n"
                "    if (i == 2) return texture(texSamp2, coord);\n"
                "    return texture(texSamp3, coord);\n");
        }
        mstring_append(s,
            "}\n"
            "\n"
            "vec4 uber_sample_cube(int i, vec3 coord) {\n"
        );
        if (opts.vk_batched) {
            mstring_append(s,
                "    if (i == 0) return texture(uber_texcube[g_uberSlot[0]], coord);\n"
                "    if (i == 1) return texture(uber_texcube[g_uberSlot[1]], coord);\n"
                "    if (i == 2) return texture(uber_texcube[g_uberSlot[2]], coord);\n"
                "    return texture(uber_texcube[g_uberSlot[3]], coord);\n");
        } else {
            mstring_append(s,
                "    if (i == 0) return texture(texSampCube0, coord);\n"
                "    if (i == 1) return texture(texSampCube1, coord);\n"
                "    if (i == 2) return texture(texSampCube2, coord);\n"
                "    return texture(texSampCube3, coord);\n");
        }
        mstring_append(s,
            "}\n"
            "\n"
            "vec4 uber_sample_3d(int i, vec3 coord) {\n"
        );
        if (opts.vk_batched) {
            mstring_append(s,
                "    if (i == 0) return texture(uber_tex3d[g_uberSlot[0]], coord);\n"
                "    if (i == 1) return texture(uber_tex3d[g_uberSlot[1]], coord);\n"
                "    if (i == 2) return texture(uber_tex3d[g_uberSlot[2]], coord);\n"
                "    return texture(uber_tex3d[g_uberSlot[3]], coord);\n");
        } else {
            mstring_append(s,
                "    if (i == 0) return texture(texSamp3D0, coord);\n"
                "    if (i == 1) return texture(texSamp3D1, coord);\n"
                "    if (i == 2) return texture(texSamp3D2, coord);\n"
                "    return texture(texSamp3D3, coord);\n");
        }
        mstring_append(s,
            "}\n"
            "\n"
            "vec4 uber_sample_3d_proj(int i, vec4 coord) {\n"
            "    vec3 tc = coord.xyz / coord.w;\n"
        );
        if (opts.vk_batched) {
            mstring_append(s,
                "    if (i == 0) return texture(uber_tex3d[g_uberSlot[0]], tc);\n"
                "    if (i == 1) return texture(uber_tex3d[g_uberSlot[1]], tc);\n"
                "    if (i == 2) return texture(uber_tex3d[g_uberSlot[2]], tc);\n"
                "    return texture(uber_tex3d[g_uberSlot[3]], tc);\n");
        } else {
            mstring_append(s,
                "    if (i == 0) return texture(texSamp3D0, tc);\n"
                "    if (i == 1) return texture(texSamp3D1, tc);\n"
                "    if (i == 2) return texture(texSamp3D2, tc);\n"
                "    return texture(texSamp3D3, tc);\n");
        }
        mstring_append(s,
            "}\n"
            "\n"
            /* textureSize is uniform across the 9 taps — hoist
             * out of the loop (9 queries -> 1; same value, parity-identical). */
            "vec4 uber_sample_convolution(int i, vec3 coord) {\n"
            /* same uniform substitution as normalize. */
            "    vec2 t117_tsz = u_tex_size[i];\n"
            "    vec4 result = vec4(0.0);\n"
            "    for (int j = 0; j < 9; j++) {\n"
            "        vec3 delta = vec3(convolution3x3[j], 0);\n"
            "        delta.xy /= t117_tsz;\n"
            "        result += uber_sample_tex(i, coord + delta) * gaussian3x3[j];\n"
            "    }\n"
            "    return result;\n"
            "}\n"
            "\n"
        );
    }
    if (!opts.vulkan) {
        /* direct dynamically-uniform indexing (GL 4.00-legal) */
        mstring_append(s,
            /* the uniform substitution, GL arm — the
             * uniform carries the exact ints textureSize() returned, in the
             * identical implicit ivec2/float expression (bit-exact). */
            "vec2 uber_normalize_tex(int i, vec2 coord) {\n"
            "    return coord / (u_tex_size[i] / texScale[i]);\n"
            "}\n"
            "\n"
            "vec4 uber_sample_tex(int i, vec3 coord) {\n"
            "    return textureProj(texSamp[i], coord);\n"
            "}\n"
            "\n"
            "vec4 uber_sample_tex2d(int i, vec2 coord) {\n"
            "    return texture(texSamp[i], coord);\n"
            "}\n"
            "\n"
            "vec4 uber_sample_cube(int i, vec3 coord) {\n"
            "    return texture(texSampCube[i], coord);\n"
            "}\n"
            "\n"
            "vec4 uber_sample_3d(int i, vec3 coord) {\n"
            "    return texture(texSamp3D[i], coord);\n"
            "}\n"
            "\n"
            "vec4 uber_sample_3d_proj(int i, vec4 coord) {\n"
            "    vec3 tc = coord.xyz / coord.w;\n"
            "    return texture(texSamp3D[i], tc);\n"
            "}\n"
            "\n"
            /* hoist the per-tap textureSize (9 -> 1). */
            "vec4 uber_sample_convolution(int i, vec3 coord) {\n"
            "    vec2 t117_tsz = u_tex_size[i];\n"
            "    vec4 result = vec4(0.0);\n"
            "    for (int j = 0; j < 9; j++) {\n"
            "        vec3 delta = vec3(convolution3x3[j], 0);\n"
            "        delta.xy /= t117_tsz;\n"
            "        result += uber_sample_tex(i, coord + delta) * gaussian3x3[j];\n"
            "    }\n"
            "    return result;\n"
            "}\n"
            "\n"
        );
    }

    mstring_append(s,
        "void uber_apply_border(int i, inout vec3 coord) {\n"
        "    vec3 ls = u_border_logical_size[i].xyz;\n"
        "    if (ls.x == 0.0) return;\n"
        "    vec3 irs = u_border_inv_real_size[i].xyz;\n"
        "    coord = (coord * ls + vec3(4,4,4)) * irs;\n"
        "}\n"
        "\n"
    );

    mstring_append(s,
        /* pT/t live as file-scope globals (the g_smod
         * pattern) — no 8-vec4 copy-in per call. Mode 3 uses a local instead
         * of writing g_pT (the old by-value write was invisible to later
         * stages; a global write would not be). */
        "vec4 g_pT[4];\n"
        "vec4 g_t[4];\n"
        "vec4 uber_fetch_texture(int i) {\n"
    );
    mstring_append(s,
        "    int mode = (int(u_tex_modes) >> (i*5)) & 0x1F;\n");
    mstring_append(s,
        "    uint tf = u_tex_flags[i];\n");
    mstring_append(s,
        "    bool is_rect = (tf & 1u) != 0u;\n"
        "    bool is_cubemap = (tf & 0x10u) != 0u;\n"
        "    bool is_shadow = (tf & 0x20u) != 0u;\n"
        "    bool is_3d = (tf & 0x40u) != 0u;\n"
        "\n"
        /* 19-way dispatch as a switch (bodies untouched). */
        "    switch (mode) {\n"
        "    case 0: return vec4(0.0, 0.0, 0.0, 1.0);\n"
        "\n"
        "    case 1: {\n"
        "        if (is_shadow) {\n"
        "            if (u_shadow_depth_func == 0) return vec4(0.0);\n"
        "            if (u_shadow_depth_func == 7) return vec4(1.0);\n"
    );
    if (opts.vulkan) {
        mstring_append(s,
        "            bool is_x8y24 = (tf & 0x08u) != 0u;\n"
        "            vec4 dv;\n"
        "            if (is_x8y24) {\n"
        "                uvec4 raw = is_rect ? uber_sample_tex_uint(i, vec3(uber_normalize_tex(i, g_pT[i].xy), g_pT[i].w)) : uber_sample_tex_uint(i, g_pT[i].xyw);\n"
        "                dv = vec4(float(raw.x >> 8u) / float(0xFFFFFF), 1.0, 0.0, 0.0);\n"
        "            } else {\n"
        "                dv = is_rect ? uber_sample_tex(i, vec3(uber_normalize_tex(i, g_pT[i].xy), g_pT[i].w)) : uber_sample_tex(i, g_pT[i].xyw);\n"
        "            }\n"
        );
    } else {
        mstring_append(s,
        "            vec4 dv = is_rect ? uber_sample_tex(i, vec3(uber_normalize_tex(i, g_pT[i].xy), g_pT[i].w)) : uber_sample_tex(i, g_pT[i].xyw);\n"
        );
    }
    mstring_append(s,
        "            float cmp = dv.x;\n"
        "            bool pass = false;\n"
        "            if (u_shadow_depth_func == 1) pass = cmp < 0.0;\n"
        "            else if (u_shadow_depth_func == 2) pass = cmp == 0.0;\n"
        "            else if (u_shadow_depth_func == 3) pass = cmp <= 0.0;\n"
        "            else if (u_shadow_depth_func == 4) pass = cmp > 0.0;\n"
        "            else if (u_shadow_depth_func == 5) pass = cmp != 0.0;\n"
        "            else if (u_shadow_depth_func == 6) pass = cmp >= 0.0;\n"
        "            return vec4(pass ? 1.0 : 0.0);\n"
        "        }\n"
        "        vec3 ba = g_pT[i].xyz;\n"
        "        uber_apply_border(i, ba);\n"
        "        if (is_rect) {\n"
        "            vec2 ntc = uber_normalize_tex(i, ba.xy);\n"
        "            float rw = g_pT[i].w;\n"
        "            if (u_conv_tex[i] != 0) return uber_sample_convolution(i, vec3(ntc, rw));\n"
        "            if (is_cubemap) return uber_sample_cube(i, remap2DToCube(vec3(ntc, rw)));\n"
        "            return uber_sample_tex(i, vec3(ntc, rw));\n"
        "        }\n"
        "        vec3 tc = vec3(ba.xy, g_pT[i].w);\n"
        "        if (u_conv_tex[i] != 0) return uber_sample_convolution(i, tc);\n"
        "        if (is_cubemap) return uber_sample_cube(i, remap2DToCube(tc));\n"
        "        if (is_3d) return uber_sample_3d_proj(i, vec4(ba.xy, 0.0, g_pT[i].w));\n"
        "        return uber_sample_tex(i, tc);\n"
        "    }\n"
        "\n"
        "    case 2: {\n"
        "        if (is_shadow) {\n"
        "            if (u_shadow_depth_func == 0) return vec4(0.0);\n"
        "            if (u_shadow_depth_func == 7) return vec4(1.0);\n"
    );
    if (opts.vulkan) {
        mstring_append(s,
        "            bool is_x8y24 = (tf & 0x08u) != 0u;\n"
        "            if (is_x8y24) {\n"
        "                uvec4 raw = is_rect ? uber_sample_tex_uint(i, vec3(uber_normalize_tex(i, g_pT[i].xy), g_pT[i].w)) : uber_sample_tex_uint(i, g_pT[i].xyw);\n"
        "                float depth_val = float(raw.x >> 8u);\n"
        "                float ref = clamp(g_pT[i].z / g_pT[i].w, 0.0, float(0xFFFFFF));\n"
        "                bool pass = false;\n"
        "                if (u_shadow_depth_func == 1) pass = depth_val < ref;\n"
        "                else if (u_shadow_depth_func == 2) pass = depth_val == ref;\n"
        "                else if (u_shadow_depth_func == 3) pass = depth_val <= ref;\n"
        "                else if (u_shadow_depth_func == 4) pass = depth_val > ref;\n"
        "                else if (u_shadow_depth_func == 5) pass = depth_val != ref;\n"
        "                else if (u_shadow_depth_func == 6) pass = depth_val >= ref;\n"
        "                return vec4(pass ? 1.0 : 0.0);\n"
        "            }\n"
        "            vec4 dv = is_rect ? uber_sample_tex(i, vec3(uber_normalize_tex(i, g_pT[i].xy), g_pT[i].w)) : uber_sample_tex(i, g_pT[i].xyw);\n"
        );
    } else {
        mstring_append(s,
        "            vec4 dv = is_rect ? uber_sample_tex(i, vec3(uber_normalize_tex(i, g_pT[i].xy), g_pT[i].w)) : uber_sample_tex(i, g_pT[i].xyw);\n"
        );
    }
    mstring_append(s,
        "            float depth_val = dv.x;\n"
        "            float max_depth;\n"
        "            if (dv.y > 0.0) {\n"
        "                max_depth = float(0xFFFFFF);\n"
        "            } else {\n"
        "                max_depth = dv.z > 0.0 ? 511.9375 : float(0xFFFF);\n"
        "            }\n"
        "            depth_val *= max_depth;\n"
        "            float ref = clamp(g_pT[i].z / g_pT[i].w, 0.0, max_depth);\n"
        "            bool pass = false;\n"
        "            if (u_shadow_depth_func == 1) pass = depth_val < ref;\n"
        "            else if (u_shadow_depth_func == 2) pass = depth_val == ref;\n"
        "            else if (u_shadow_depth_func == 3) pass = depth_val <= ref;\n"
        "            else if (u_shadow_depth_func == 4) pass = depth_val > ref;\n"
        "            else if (u_shadow_depth_func == 5) pass = depth_val != ref;\n"
        "            else if (u_shadow_depth_func == 6) pass = depth_val >= ref;\n"
        "            return vec4(pass ? 1.0 : 0.0);\n"
        "        }\n"
        "        vec3 tc4 = g_pT[i].xyz;\n"
        "        uber_apply_border(i, tc4);\n"
        "        if (is_3d) {\n"
        "            vec4 c3d = is_rect ? vec4(uber_normalize_tex(i, tc4.xy), tc4.z, g_pT[i].w) : vec4(tc4, g_pT[i].w);\n"
        "            return uber_sample_3d_proj(i, c3d);\n"
        "        }\n"
        "        if (is_rect) return uber_sample_tex(i, vec3(uber_normalize_tex(i, tc4.xy), g_pT[i].w));\n"
        "        return uber_sample_tex(i, vec3(tc4.xy, g_pT[i].w));\n"
        "    }\n"
        "\n"
        "    case 3: {\n"
        "        if (is_cubemap) return uber_sample_cube(i, g_pT[i].xyz);\n"
        "        vec2 c2d = remapCubeTo2D(g_pT[i].xyz);\n"
        "        return uber_sample_tex2d(i, c2d);\n"
        "    }\n"
        "\n"
        "    case 4: return g_pT[i];\n"
        "\n"
        "    case 5: {\n"
        "        uint cmp = u_compare_mode_packed;\n"
        "        for (int j = 0; j < 4; j++) {\n"
        "            bool cmode = ((cmp >> (4*i + j)) & 1u) != 0u;\n"
        "            float val;\n"
        "            if (j==0) val = g_pT[i].x;\n"
        "            else if (j==1) val = g_pT[i].y;\n"
        "            else if (j==2) val = g_pT[i].z;\n"
        "            else val = g_pT[i].w;\n"
        "            if (cmode) { if (val >= 0.0) discard; }\n"
        "            else { if (val < 0.0) discard; }\n"
        "        }\n"
        "        return vec4(0.0);\n"
        "    }\n"
        "\n"
        "    case 6: case 7: {\n"
        "        int inp = u_input_tex[i];\n"
        "        vec2 dsdt = vec2(sign3(g_t[inp].b), sign3(g_t[inp].g));\n"
        "        dsdt = bumpMat[i] * dsdt;\n"
        "        vec2 tc2 = g_pT[i].xy + dsdt;\n"
        "        if (is_rect) tc2 = uber_normalize_tex(i, tc2);\n"
        "        vec4 res = (is_3d) ? uber_sample_3d(i, vec3(tc2, g_pT[i].z)) : uber_sample_tex2d(i, tc2);\n"
        "        if (mode == 7) {\n"
        "            res *= bumpScale[i] * g_t[inp].r + bumpOffset[i];\n"
        "        }\n"
        "        return res;\n"
        "    }\n"
        "\n"
        "    case 8: {\n"
        "        return vec4(0.0);\n"
        "    }\n"
        "\n"
        "    case 9: {\n"
        "        int inp = u_input_tex[i];\n"
        "        int dm = u_dot_map[i];\n"
        "        float d = dot(g_pT[i].xyz, dotmap_func(dm, g_t[inp]));\n"
        "        float prev_d = 0.0;\n"
        "        if (i >= 2) {\n"
        "            int prev_inp = u_input_tex[i-1];\n"
        "            int prev_dm = u_dot_map[i-1];\n"
        "            prev_d = dot(g_pT[i-1].xyz, dotmap_func(prev_dm, g_t[prev_inp]));\n"
        "        }\n"
        "        vec3 dotST = vec3(prev_d, d, 0.0);\n"
        "        uber_apply_border(i, dotST);\n"
        "        vec2 tc_dot = dotST.xy;\n"
        "        return uber_sample_tex2d(i, is_rect ? uber_normalize_tex(i, tc_dot) : tc_dot);\n"
        "    }\n"
        "\n"
        "    case 10: {\n"
        "        return vec4(0.0);\n"
        "    }\n"
        "\n"
        "    case 11: {\n"
        "        int inp = u_input_tex[i];\n"
        "        int dm = u_dot_map[i];\n"
        "        float d = dot(g_pT[i].xyz, dotmap_func(dm, g_t[inp]));\n"
        "        int next_inp = u_input_tex[i+1];\n"
        "        int next_dm = u_dot_map[i+1];\n"
        "        float d_n = dot(g_pT[i+1].xyz, dotmap_func(next_dm, g_t[next_inp]));\n"
        "        float prev_d = 0.0;\n"
        "        if (i >= 2) {\n"
        "            int prev_inp = u_input_tex[i-1];\n"
        "            int prev_dm = u_dot_map[i-1];\n"
        "            prev_d = dot(g_pT[i-1].xyz, dotmap_func(prev_dm, g_t[prev_inp]));\n"
        "        }\n"
        "        vec3 n_v = vec3(prev_d, d, d_n);\n"
        "        uber_apply_border(i, n_v);\n"
        "        if (is_cubemap) return uber_sample_cube(i, n_v);\n"
        "        n_v.xy = remapCubeTo2D(n_v);\n"
        "        return uber_sample_tex2d(i, n_v.xy);\n"
        "    }\n"
        "\n"
        "    case 12: {\n"
        "        int inp = u_input_tex[i];\n"
        "        int dm = u_dot_map[i];\n"
        "        float d = dot(g_pT[i].xyz, dotmap_func(dm, g_t[inp]));\n"
        "        float prev_d1 = 0.0, prev_d2 = 0.0;\n"
        "        if (i >= 2) {\n"
        "            int p1 = u_input_tex[i-2];\n"
        "            int p1m = u_dot_map[i-2];\n"
        "            prev_d1 = dot(g_pT[i-2].xyz, dotmap_func(p1m, g_t[p1]));\n"
        "            int p2 = u_input_tex[i-1];\n"
        "            int p2m = u_dot_map[i-1];\n"
        "            prev_d2 = dot(g_pT[i-1].xyz, dotmap_func(p2m, g_t[p2]));\n"
        "        }\n"
        "        vec3 n_v = vec3(prev_d1, prev_d2, d);\n"
        "        vec3 e_v = vec3(g_pT[i-2].w, g_pT[i-1].w, g_pT[i].w);\n"
        "        vec3 rv = 2.0*n_v*dot(n_v,e_v)/dot(n_v,n_v) - e_v;\n"
        "        uber_apply_border(i, rv);\n"
        "        if (is_cubemap) return uber_sample_cube(i, rv);\n"
        "        rv.xy = remapCubeTo2D(rv);\n"
        "        return uber_sample_tex2d(i, rv.xy);\n"
        "    }\n"
        "\n"
        "    case 13: {\n"
        "        int inp = u_input_tex[i];\n"
        "        int dm = u_dot_map[i];\n"
        "        float d = dot(g_pT[i].xyz, dotmap_func(dm, g_t[inp]));\n"
        "        float prev_d1 = 0.0, prev_d2 = 0.0;\n"
        "        if (i >= 2) {\n"
        "            int p1 = u_input_tex[i-2];\n"
        "            int p1m = u_dot_map[i-2];\n"
        "            prev_d1 = dot(g_pT[i-2].xyz, dotmap_func(p1m, g_t[p1]));\n"
        "            int p2 = u_input_tex[i-1];\n"
        "            int p2m = u_dot_map[i-1];\n"
        "            prev_d2 = dot(g_pT[i-1].xyz, dotmap_func(p2m, g_t[p2]));\n"
        "        }\n"
        "        vec3 dotSTR = vec3(prev_d1, prev_d2, d);\n"
        "        uber_apply_border(i, dotSTR);\n"
        "        if (is_cubemap) return uber_sample_cube(i, dotSTR);\n"
        "        if (is_3d) return uber_sample_3d(i, dotSTR);\n"
        "        return uber_sample_tex2d(i, is_rect ? uber_normalize_tex(i, dotSTR.xy) : dotSTR.xy);\n"
        "    }\n"
        "\n"
        "    case 14: {\n"
        "        int inp = u_input_tex[i];\n"
        "        int dm = u_dot_map[i];\n"
        "        float d = dot(g_pT[i].xyz, dotmap_func(dm, g_t[inp]));\n"
        "        float prev_d1 = 0.0, prev_d2 = 0.0;\n"
        "        if (i >= 2) {\n"
        "            int p1 = u_input_tex[i-2];\n"
        "            int p1m = u_dot_map[i-2];\n"
        "            prev_d1 = dot(g_pT[i-2].xyz, dotmap_func(p1m, g_t[p1]));\n"
        "            int p2 = u_input_tex[i-1];\n"
        "            int p2m = u_dot_map[i-1];\n"
        "            prev_d2 = dot(g_pT[i-1].xyz, dotmap_func(p2m, g_t[p2]));\n"
        "        }\n"
        "        vec3 dotSTR = vec3(prev_d1, prev_d2, d);\n"
        "        uber_apply_border(i, dotSTR);\n"
        "        if (is_cubemap) return uber_sample_cube(i, dotSTR);\n"
        "        dotSTR.xy = remapCubeTo2D(dotSTR);\n"
        "        return uber_sample_tex2d(i, dotSTR.xy);\n"
        "    }\n"
        "\n"
        "    case 15: {\n"
        "        int inp = u_input_tex[i];\n"
        "        vec3 arb = vec3(g_t[inp].ar, 0.0);\n"
        "        uber_apply_border(i, arb);\n"
        "        return uber_sample_tex2d(i, arb.xy);\n"
        "    }\n"
        "\n"
        "    case 16: {\n"
        "        int inp = u_input_tex[i];\n"
        "        vec3 gbb = vec3(g_t[inp].gb, 0.0);\n"
        "        uber_apply_border(i, gbb);\n"
        "        return uber_sample_tex2d(i, gbb.xy);\n"
        "    }\n"
        "\n"
        "    case 17: {\n"
        "        int inp = u_input_tex[i];\n"
        "        int dm = u_dot_map[i];\n"
        "        float d = dot(g_pT[i].xyz, dotmap_func(dm, g_t[inp]));\n"
        "        return vec4(0.0);\n"
        "    }\n"
        "\n"
        "    case 18: {\n"
        "        return vec4(0.0);\n"
        "    }\n"
        "\n"
        "    default: return vec4(0.0);\n"
        "    }\n"
        "}\n"
    );
}

static void uber_psh_append_main(MString *s, GenUberGlslOptions opts)
{
    mstring_append(s, "void main() {\n");

    mstring_append(s,
        "    uint uf = u_uber_flags;\n"
    );
    if (opts.vk_batched) { /* hoist the draw record once */
        mstring_append(s,
            "    {\n"
            "        uvec4 r78 = uber_recs[g_uberRec];\n"
            "        for (int i = 0; i < 4; i++) {\n"
            "            g_uberSlot[i] = int(r78[i] & 0xFFFFu);\n"
            "            g_uberSlotU[i] = int(r78[i] >> 16);\n"
            "        }\n"
            "    }\n"
        );
    }
    mstring_append(s,
        "    bool wclip_excl = (uf & 0x02u) != 0u;\n"
        "    bool z_persp = (uf & 0x10u) != 0u;\n"
        "    bool depth_clip = (uf & 0x08u) != 0u;\n"
        "    bool point_spr = (uf & 0x20u) != 0u;\n"
        "\n"
        "\n"
        "    vec2 coord = gl_FragCoord.xy - 0.5;\n"
        "    if (wclip_excl) {\n"
        "        for (int i = 0; i < 8; i++) {\n"
        "            bool outside = any(bvec4(\n"
        "                lessThan(coord, vec2(clipRegion[i].xy)),\n"
        "                greaterThanEqual(coord, vec2(clipRegion[i].zw))));\n"
        "            if (!outside) discard;\n"
        "        }\n"
        "    } else {\n"
        "        bool contained = false;\n"
        "        for (int i = 0; i < 8; i++) {\n"
        "            bool outside = any(bvec4(\n"
        "                lessThan(coord, vec2(clipRegion[i].xy)),\n"
        "                greaterThanEqual(coord, vec2(clipRegion[i].zw))));\n"
        "            if (!outside) { contained = true; break; }\n"
        "        }\n"
        "        if (!contained) discard;\n"
        "    }\n"
        "\n"
    );

    if (opts.gl_nogs_tri) {
        /* (bit-exact): no GS, but read all 3 triangle vertices
         * via GL_NV_fragment_shader_barycentric (pervertexNV v_vtxPos0[3]) and
         * run the GEOMETRY shader's EXACT rotation + tmz math, so vtxPos0/1/2 +
         * triMZ are bit-identical to what the classic GS would have emitted.
         * The shared area()-based zvalue below is then byte-for-byte the GS
         * path -> zero depth divergence with no GS slow path.
         * (gl_PrimitiveID here == the GS's gl_PrimitiveIDIn for the same tri;
         * pervertexNV v_vtxPos0[i] == the GS's gl_in[i] -> sp_i.) */
        mstring_append(s,
            "    ivec3 nv;\n"
            "    if (u_tri_rot0 == u_tri_rot1) {\n"
            "        nv = get_vertex_order(u_tri_rot0);\n"
            "    } else {\n"
            "        nv = (gl_PrimitiveID & 1) == 0\n"
            "            ? get_vertex_order(u_tri_rot0)\n"
            "            : get_vertex_order(u_tri_rot1);\n"
            "    }\n"
            "    vec4 vtxPos0 = v_vtxPos0[nv[0]];\n"
            "    vec4 vtxPos1 = v_vtxPos0[nv[1]];\n"
            "    vec4 vtxPos2 = v_vtxPos0[nv[2]];\n"
            "    float triMZ;\n"
            "    if (z_persp) {\n"
            "        mat2 m = mat2(vtxPos1.xy - vtxPos0.xy, vtxPos2.xy - vtxPos0.xy);\n"
            "        precise vec2 b = vec2(vtxPos0.w - vtxPos1.w, vtxPos0.w - vtxPos2.w);\n"
            "        b /= vec2(vtxPos1.w, vtxPos2.w) * vtxPos0.w;\n"
            "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
            "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
            "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
            "        triMZ = max(abs(dzx), abs(dzy));\n"
            "    } else {\n"
            "        mat2 m = mat2(vtxPos1.xy - vtxPos0.xy, vtxPos2.xy - vtxPos0.xy);\n"
            "        precise vec2 b = vec2(vtxPos1.z - vtxPos0.z, vtxPos2.z - vtxPos0.z);\n"
            "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
            "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
            "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
            "        triMZ = max(abs(dzx), abs(dzy));\n"
            "    }\n"
            "    triMZ = (isnan(triMZ) || isinf(triMZ)) ? 0.0 : triMZ;\n"
            "\n"
        );
    }
    mstring_append(s,
        "    vec2 unscaled_xy = gl_FragCoord.xy / surfaceScale;\n"
        "    precise float zvalue;\n"
        "    if (z_persp) {\n"
        "        precise float bc0 = area(unscaled_xy, vtxPos1.xy, vtxPos2.xy);\n"
        "        precise float bc1 = area(unscaled_xy, vtxPos2.xy, vtxPos0.xy);\n"
        "        precise float bc2 = area(unscaled_xy, vtxPos0.xy, vtxPos1.xy);\n"
        "        bc0 /= vtxPos0.w;\n"
        "        bc1 /= vtxPos1.w;\n"
        "        bc2 /= vtxPos2.w;\n"
        "        float inv_bcsum = 1.0 / (bc0 + bc1 + bc2);\n"
        "        if (isinf(inv_bcsum)) inv_bcsum = 0.0;\n"
        "        bc1 *= inv_bcsum; bc2 *= inv_bcsum;\n"
        "        zvalue = vtxPos0.w + (bc1*(vtxPos1.w - vtxPos0.w) + bc2*(vtxPos2.w - vtxPos0.w));\n"
        "        if (zvalue > 0.0) {\n"
        "            float zslopeofs = depthFactor*triMZ*zvalue*zvalue;\n"
        "            zvalue += depthOffset;\n"
        "            zvalue += zslopeofs;\n"
        "        } else {\n"
        "            zvalue = uintBitsToFloat(0x7F7FFFFFu);\n"
        "        }\n"
        "        if (isnan(zvalue)) zvalue = uintBitsToFloat(0x7F7FFFFFu);\n"
        "    } else {\n"
        "        precise float bc0 = area(unscaled_xy, vtxPos1.xy, vtxPos2.xy);\n"
        "        precise float bc1 = area(unscaled_xy, vtxPos2.xy, vtxPos0.xy);\n"
        "        precise float bc2 = area(unscaled_xy, vtxPos0.xy, vtxPos1.xy);\n"
        "        float inv_bcsum = 1.0 / (bc0 + bc1 + bc2);\n"
        "        if (isinf(inv_bcsum)) inv_bcsum = 0.0;\n"
        "        bc1 *= inv_bcsum; bc2 *= inv_bcsum;\n"
        "        zvalue = vtxPos0.z + (bc1*(vtxPos1.z - vtxPos0.z) + bc2*(vtxPos2.z - vtxPos0.z));\n"
        "        zvalue += depthOffset;\n"
        "        zvalue += depthFactor*triMZ;\n"
        "    }\n"
        "\n"
    );

    mstring_append(s,
        "    if (depth_clip) {\n"
        "        if (zvalue < clipRange.z || clipRange.w < zvalue) { discard; }\n"
        "    } else {\n"
        "        zvalue = clamp(zvalue, clipRange.z, clipRange.w);\n"
        "    }\n"

        "\n"
        "    vec4 pD0 = vtxD0;\n"
        "    vec4 pD1 = vtxD1;\n"
        "    vec4 pFog = vec4(fogColor.rgb, clamp(vtxFog, 0.0, 1.0));\n"
        "\n"
        /* build the globals directly (no local arrays). */
        "    g_pT[0] = vtxT0;\n"
        "    g_pT[1] = vtxT1;\n"
        "    g_pT[2] = vtxT2;\n"
        "    g_pT[3] = point_spr ? vec4(gl_PointCoord, 1.0, 1.0) : vtxT3;\n"
        "\n"
        "    g_t[0] = vec4(0,0,0,1);\n"
        "    g_t[1] = vec4(0,0,0,1);\n"
        "    g_t[2] = vec4(0,0,0,1);\n"
        "    g_t[3] = vec4(0,0,0,1);\n"
        "    for (int i = 0; i < 4; i++) {\n"
    );

    mstring_append(s,
        "        g_t[i] = uber_fetch_texture(i);\n"
        "\n"
    );
    mstring_append(s,
        "        int mode = (int(u_tex_modes) >> (i*5)) & 0x1F;\n");
    mstring_append(s,
        "        if (mode != 0) {\n"
        "            uint tf = u_tex_flags[i];\n"
        "            if ((tf & 0x04u) != 0u && g_t[i].a == 0.0) { discard; }\n"
        "            int ckm = u_colorkey_mode[i];\n"
        "            if (ckm != 0) {\n"
        "                if (check_color_key(g_t[i], colorKey[i], colorKeyMask[i])) {\n"
        "                    if (ckm == 3) discard;\n"
        "                    else if (ckm == 1) g_t[i].a = 0.0;\n"
        "                    else if (ckm == 2) g_t[i] = vec4(0.0);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "\n"
        "\n"
        "    int num_stages = int(u_combiner_control & 0xFFu);\n"
        ""
        ""
    );
    mstring_append(s,
        "    uint combiner_flags = u_combiner_control >> 8;\n");
    mstring_append(s,
        "    bool mux_msb = (combiner_flags & 0x01u) != 0u;\n"
        "\n"
        "    init_regs(pD0, pD1, pFog, g_t[0], g_t[1], g_t[2], g_t[3]);\n"
        "\n"
        "    int tex0_mode = int(u_tex_modes) & 0x1F;\n"
        "    if (tex0_mode != 0) {\n"
        "        uber_hi[4].a = g_t[0].a;\n"
        "    } else {\n"
        "        uber_hi[4].a = 1.0;\n"
        "    }\n"
        "\n"
        /* fixed bound + early break — the unrollable form for
         * compilers that won't unroll a runtime-bound loop. Iterations
         * executed are identical. */
        "    for (int s = 0; s < 8; s++) {\n"
        "        if (s >= num_stages) break;\n"
        "        run_combiner_stage(s, u_rgb_inputs[s], u_rgb_outputs[s],\n"
        "                           u_alpha_inputs[s], u_alpha_outputs[s],\n"
        "                           combiner_flags, mux_msb);\n"
        "    }\n"
        "\n"
        "    fragColor = uber_final_combiner();\n"
        "\n"
        "    if ((u_uber_flags & 0x01u) != 0u) {\n"
        "        int fragAlpha = int(round(fragColor.a * 255.0));\n"
        /* switch dispatch, conditions identical. */
        "        switch (u_alpha_func) {\n"
        "        case 0: discard;\n"
        "        case 1: if (!(fragAlpha < alphaRef)) discard; break;\n"
        "        case 2: if (!(fragAlpha == alphaRef)) discard; break;\n"
        "        case 3: if (!(fragAlpha <= alphaRef)) discard; break;\n"
        "        case 4: if (!(fragAlpha > alphaRef)) discard; break;\n"
        "        case 5: if (!(fragAlpha != alphaRef)) discard; break;\n"
        "        case 6: if (!(fragAlpha >= alphaRef)) discard; break;\n"
        "        default: break;\n"
        "        }\n"
        "    }\n"
        "\n"
        "    if (u_depth_format == 0) {\n"
        "        gl_FragDepth = floor(zvalue) / 65535.0;\n"
        "    } else if (u_depth_format == 1) {\n"
        "        gl_FragDepth = uintBitsToFloat(floatBitsToUint(floor(zvalue) / 16777216.0) + 1u);\n"
        "    } else {\n"
        "        gl_FragDepth = zvalue / clipRange.y;\n"
        "    }\n"
        "}\n"
    );
}

/* defined below near the VSH header helpers */
static void uber_vtx_header_nogs(MString *s);

MString *pgraph_glsl_gen_uber_psh(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);
    if (opts.gl_nogs_tri) {
        uber_vtx_header_nogs(s);
    } else {
        pgraph_glsl_get_vtx_header(s, opts.vulkan, true, true, false, false);
    }

    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 13) flat in uint g_uberRec;\n");
    }
    mstring_append(s, "layout(location = 0) out vec4 fragColor;\n\n");

    uber_psh_append_uniforms(s, opts);
    uber_psh_append_helpers(s);
    uber_psh_append_texture_sampling(s, opts);
    uber_psh_append_register_combiner(s, opts);
    uber_psh_append_final_combiner(s, opts);
    uber_psh_append_main(s, opts);

    return s;
}

/* ========================================================================
 * Vertex Ubershader
 * ======================================================================== */

/* the VSH→GS interface carries only what a GS reads —
 * the 9 per-vertex varyings + v_vtxPos0 (locations 0-9, qualifiers
 * identical to the full header's). v_vtxPos1/v_vtxPos2/v_triMZ were
 * written by the VSH but read by no GS variant (grep-verified): 9 dead
 * floats per vertex of interface traffic. The GS→PSH leg keeps the full
 * 13-varying header (the PSH consumes them all). */
static void uber_vtx_header_vsh_gs(MString *s, bool location, bool in,
                                   bool array)
{
    static const struct {
        const char *qualifier, *type, *name;
    } attr[] = {
        { "",      "vec4",  "vtxD0"   },
        { "",      "vec4",  "vtxD1"   },
        { "",      "vec4",  "vtxB0"   },
        { "",      "vec4",  "vtxB1"   },
        { "",      "float", "vtxFog"  },
        { "",      "vec4",  "vtxT0"   },
        { "",      "vec4",  "vtxT1"   },
        { "",      "vec4",  "vtxT2"   },
        { "",      "vec4",  "vtxT3"   },
        { "flat ", "vec4",  "vtxPos0" },
    };
    for (int i = 0; i < ARRAY_SIZE(attr); i++) {
        if (location) {
            mstring_append_fmt(s, "layout(location = %d) ", i);
        }
        /* GL drops 'flat' from v_vtxPos0 so the GS-less PSH can
         * read it per-vertex via pervertexNV (flat + pervertexNV is an illegal
         * pair of interpolation qualifiers). GS inputs are never interpolated,
         * so this is a no-op for every GS path; VK keeps flat (no GS-less path
         * there). location == opts.vulkan at all 7 call sites -> backend flag. */
        const char *qualifier = location ? attr[i].qualifier : "";
        mstring_append_fmt(s, "%s%s %s v_%s%s;\n", qualifier,
                           in ? "in" : "out", attr[i].type, attr[i].name,
                           array ? "[]" : "");
    }
}

/* (bit-exact): the GS-less smooth-tri PSH interface. The 9
 * smooth varyings carry the VSH's v_vtx* names (body keeps its vtx* spellings
 * via #define). v_vtxPos0 is pulled in per-vertex via
 * GL_NV_fragment_shader_barycentric (a pervertexNV array [3] -> the same 3
 * screen positions the GS reads as gl_in[]), and the tri-winding uniforms +
 * kahan_det/get_vertex_order let main() reproduce the classic GS's EXACT
 * depth-plane math, so depth is bit-identical to the GS path with no GS. */
static void uber_vtx_header_nogs(MString *s)
{
    static const struct {
        const char *type, *name;
    } attr[] = {
        { "vec4",  "vtxD0"  }, { "vec4",  "vtxD1"  },
        { "vec4",  "vtxB0"  }, { "vec4",  "vtxB1"  },
        { "float", "vtxFog" },
        { "vec4",  "vtxT0"  }, { "vec4",  "vtxT1"  },
        { "vec4",  "vtxT2"  }, { "vec4",  "vtxT3"  },
    };
    for (int i = 0; i < ARRAY_SIZE(attr); i++) {
        mstring_append_fmt(s, "in %s v_%s;\n#define %s v_%s\n",
                           attr[i].type, attr[i].name, attr[i].name,
                           attr[i].name);
    }
    /* Per-vertex screen positions (matches the VSH's flat v_vtxPos0 out by
     * name, exactly as the GS's `in vec4 v_vtxPos0[]` does) + the GS's exact
     * depth helpers, so main() runs the classic plane math without a GS. */
    mstring_append(s,
        /* Bare 'pervertexNV' (no interpolation qualifier): on GL the VSH emits
         * v_vtxPos0 WITHOUT 'flat' (uber_vtx_header_vsh_gs), so this default-
         * interpolation per-vertex input matches. flat + pervertexNV is illegal
         * (mutually-exclusive interpolation qualifiers, NVIDIA error C1040). */
        "pervertexNV in vec4 v_vtxPos0[3];\n"
        "uniform int u_tri_rot0;\n"
        "uniform int u_tri_rot1;\n"
        "float kahan_det(float a, float b, float c, float d) {\n"
        "    precise float cd = c*d;\n"
        "    precise float err = fma(-c, d, cd);\n"
        "    precise float res = fma(a, b, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "ivec3 get_vertex_order(int rot) {\n"
        "    if (rot == 0) return ivec3(0, 1, 2);\n"
        "    if (rot == 1) return ivec3(2, 0, 1);\n"
        "    return ivec3(1, 2, 0);\n"
        "}\n");
}

MString *pgraph_glsl_gen_uber_vsh(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    const char *u = opts.vulkan ? "" : "uniform ";

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = 0, %s) uniform VshUniforms {\n",
            uber_ubo_layout(opts));
    }

    for (int i = 0; i < VshUniform__COUNT; i++) {
        const UniformInfo *info = &VshUniformInfo[i];
        const char *type_str = uniform_element_type_to_str[info->type];
        if (info->count == 1) {
            mstring_append_fmt(s, "%s%s %s;\n", u, type_str, info->name);
        } else {
            mstring_append_fmt(s, "%s%s %s[%zd];\n", u, type_str, info->name,
                               info->count);
        }
    }

    mstring_append_fmt(s,
        "%suint u_uber_vsh_flags;\n"
        "%sint u_skinning_mode;\n"
        "%sint u_fog_mode;\n"
        "%sint u_foggen_mode;\n"
        "%suint u_light_enable_packed;\n"
        "%suint u_texgen_packed[4];\n"
        "%suint u_mat_color_src_packed;\n"
        "%suint u_uniform_attrs;\n"
        "%suint u_compressed_attrs;\n"
        "%suint u_swizzle_attrs;\n"
        "%sfloat u_point_size;\n"
        "%sfloat u_point_params[8];\n"
        "%sint u_surface_scale_factor;\n"
        "%sint u_program_length;\n"
        /* 3 words/instr — w0 is never decoded (only w1..w3). */
        "%suint u_program_data[408];\n",
        u, u, u, u, u, u, u, u, u, u, u, u, u, u, u);

    if (opts.vulkan) {
        mstring_append(s, "};\n");
    }



    uber_vtx_header_vsh_gs(s, opts.vulkan, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "#extension GL_ARB_shader_draw_parameters : require\n"
            "layout(location = 10) flat out uint v_uberRec;\n"
            "layout(push_constant) uniform UberPC { uint uber_rec_base; };\n");
    }

    mstring_append(s,
        "layout(location = 0) in vec4 _v0;\n"
        "layout(location = 1) in vec4 _v1;\n"
        "layout(location = 2) in vec4 _v2;\n"
        "layout(location = 3) in vec4 _v3;\n"
        "layout(location = 4) in vec4 _v4;\n"
        "layout(location = 5) in vec4 _v5;\n"
        "layout(location = 6) in vec4 _v6;\n"
        "layout(location = 7) in vec4 _v7;\n"
        "layout(location = 8) in vec4 _v8;\n"
        "layout(location = 9) in vec4 _v9;\n"
        "layout(location = 10) in vec4 _v10;\n"
        "layout(location = 11) in vec4 _v11;\n"
        "layout(location = 12) in vec4 _v12;\n"
        "layout(location = 13) in vec4 _v13;\n"
        "layout(location = 14) in vec4 _v14;\n"
        "layout(location = 15) in vec4 _v15;\n"
        "\n"
    );

    mstring_append(s,
        GLSL_DEFINE(fogPlane, GLSL_C(NV_IGRAPH_XF_XFCTX_FOG))
        GLSL_DEFINE(texMat0, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T0MAT))
        GLSL_DEFINE(texMat1, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T1MAT))
        GLSL_DEFINE(texMat2, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T2MAT))
        GLSL_DEFINE(texMat3, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_T3MAT))
        GLSL_DEFINE(MVP, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_CMAT0))
        GLSL_DEFINE(modelViewMat0, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT0))
        GLSL_DEFINE(modelViewMat1, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT1))
        GLSL_DEFINE(modelViewMat2, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT2))
        GLSL_DEFINE(modelViewMat3, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_MMAT3))
        GLSL_DEFINE(invModelViewMat0, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT0))
        GLSL_DEFINE(invModelViewMat1, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT1))
        GLSL_DEFINE(invModelViewMat2, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT2))
        GLSL_DEFINE(invModelViewMat3, GLSL_C_MAT4(NV_IGRAPH_XF_XFCTX_IMMAT3))
        GLSL_DEFINE(eyePosition, GLSL_C(NV_IGRAPH_XF_XFCTX_EYEP))
        "\n"
        GLSL_DEFINE(texPlaneS0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 0))
        GLSL_DEFINE(texPlaneT0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 1))
        GLSL_DEFINE(texPlaneR0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 2))
        GLSL_DEFINE(texPlaneQ0, GLSL_C(NV_IGRAPH_XF_XFCTX_TG0MAT + 3))
        "\n"
        GLSL_DEFINE(texPlaneS1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 0))
        GLSL_DEFINE(texPlaneT1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 1))
        GLSL_DEFINE(texPlaneR1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 2))
        GLSL_DEFINE(texPlaneQ1, GLSL_C(NV_IGRAPH_XF_XFCTX_TG1MAT + 3))
        "\n"
        GLSL_DEFINE(texPlaneS2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 0))
        GLSL_DEFINE(texPlaneT2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 1))
        GLSL_DEFINE(texPlaneR2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 2))
        GLSL_DEFINE(texPlaneQ2, GLSL_C(NV_IGRAPH_XF_XFCTX_TG2MAT + 3))
        "\n"
        GLSL_DEFINE(texPlaneS3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 0))
        GLSL_DEFINE(texPlaneT3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 1))
        GLSL_DEFINE(texPlaneR3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 2))
        GLSL_DEFINE(texPlaneQ3, GLSL_C(NV_IGRAPH_XF_XFCTX_TG3MAT + 3))
        "\n"
        GLSL_DEFINE(sceneAmbientColor,
                    GLSL_LTCTXA(NV_IGRAPH_XF_LTCTXA_FR_AMB) ".xyz")
        GLSL_DEFINE(materialEmissionColor,
                    GLSL_LTCTXA(NV_IGRAPH_XF_LTCTXA_CM_COL) ".xyz")
        "\n"
        "float clampAwayZeroInf(float t) {\n"
        "  if (t > 0.0 || floatBitsToUint(t) == 0u) {\n"
        "    t = clamp(t, uintBitsToFloat(0x1F800000u), uintBitsToFloat(0x5F800000u));\n"
        "  } else {\n"
        "    t = clamp(t, uintBitsToFloat(0xDF800000u), uintBitsToFloat(0x9F800000u));\n"
        "  }\n"
        "  return t;\n"
        "}\n"
        "vec4 NaNToOne(vec4 src) {\n"
        "  return mix(src, vec4(1.0), isnan(src));\n"
        "}\n"
        "#define FLOAT_MAX uintBitsToFloat(0x7F7FFFFFu)\n"
        "\n"
        "vec2 roundScreenCoords(vec2 pos) {\n"
        "  return trunc(pos * 16.0) / 16.0;\n"
        "}\n"
        "vec4 NaNToValue(vec4 src, float replacement) {\n"
        "  return mix(src, vec4(replacement), isnan(src));\n"
        "}\n"
        "vec4 nv2a_mul(vec4 a, vec4 b) {\n"
        "  vec4 z = sign(NaNToOne(a)) * sign(NaNToOne(b));\n"
        "  vec4 r = a * b;\n"
        "  if (z.x == 0.0) r.x = 0.0;\n"
        "  if (z.y == 0.0) r.y = 0.0;\n"
        "  if (z.z == 0.0) r.z = 0.0;\n"
        "  if (z.w == 0.0) r.w = 0.0;\n"
        "  return r;\n"
        "}\n"
        ""
        "\n"
    );

    /* the interpreter register files as individual globals with
     * warp-coherent switch accessors (the instruction — hence each operand's
     * register number — is uniform across the warp at every loop iteration),
     * replacing dynamically-indexed local arrays (the local-memory trap).
     * The r>=12 -> oR0 operand
     * fallback is folded into the R accessors' default. */
    mstring_append(s,
        "vec4 g_vR0, g_vR1, g_vR2, g_vR3, g_vR4, g_vR5,\n"
        "     g_vR6, g_vR7, g_vR8, g_vR9, g_vR10, g_vR11;\n"
        "vec4 g_voR0, g_voR1, g_voR2, g_voR3, g_voR4, g_voR5, g_voR6,\n"
        "     g_voR7, g_voR8, g_voR9, g_voR10, g_voR11, g_voR12;\n"
        "vec4 uber_vsh_Rrd(uint i) {\n"
        "    switch (i) {\n"
        "    case 0u: return g_vR0;   case 1u: return g_vR1;\n"
        "    case 2u: return g_vR2;   case 3u: return g_vR3;\n"
        "    case 4u: return g_vR4;   case 5u: return g_vR5;\n"
        "    case 6u: return g_vR6;   case 7u: return g_vR7;\n"
        "    case 8u: return g_vR8;   case 9u: return g_vR9;\n"
        "    case 10u: return g_vR10; case 11u: return g_vR11;\n"
        "    default: return g_voR0;\n"
        "    }\n"
        "}\n"
        "void uber_vsh_Rwr(uint i, vec4 v) {\n"
        "    switch (i) {\n"
        "    case 0u: g_vR0 = v; break;   case 1u: g_vR1 = v; break;\n"
        "    case 2u: g_vR2 = v; break;   case 3u: g_vR3 = v; break;\n"
        "    case 4u: g_vR4 = v; break;   case 5u: g_vR5 = v; break;\n"
        "    case 6u: g_vR6 = v; break;   case 7u: g_vR7 = v; break;\n"
        "    case 8u: g_vR8 = v; break;   case 9u: g_vR9 = v; break;\n"
        "    case 10u: g_vR10 = v; break; case 11u: g_vR11 = v; break;\n"
        "    default: g_voR0 = v; break;\n"
        "    }\n"
        "}\n"
        "vec4 uber_vsh_oRrd(uint i) {\n"
        "    switch (i) {\n"
        "    case 0u: return g_voR0;   case 1u: return g_voR1;\n"
        "    case 2u: return g_voR2;   case 3u: return g_voR3;\n"
        "    case 4u: return g_voR4;   case 5u: return g_voR5;\n"
        "    case 6u: return g_voR6;   case 7u: return g_voR7;\n"
        "    case 8u: return g_voR8;   case 9u: return g_voR9;\n"
        "    case 10u: return g_voR10; case 11u: return g_voR11;\n"
        "    default: return g_voR12;\n"
        "    }\n"
        "}\n"
        "void uber_vsh_oRwr(uint i, vec4 v) {\n"
        "    switch (i) {\n"
        "    case 0u: g_voR0 = v; break;   case 1u: g_voR1 = v; break;\n"
        "    case 2u: g_voR2 = v; break;   case 3u: g_voR3 = v; break;\n"
        "    case 4u: g_voR4 = v; break;   case 5u: g_voR5 = v; break;\n"
        "    case 6u: g_voR6 = v; break;   case 7u: g_voR7 = v; break;\n"
        "    case 8u: g_voR8 = v; break;   case 9u: g_voR9 = v; break;\n"
        "    case 10u: g_voR10 = v; break; case 11u: g_voR11 = v; break;\n"
        "    case 12u: g_voR12 = v; break;\n"
        "    default: break;\n"
        "    }\n"
        "}\n"
        "\n"
    );

    mstring_append(s,
        "void main() {\n"
        /* attrs land in named locals; the special-attr
         * treatment (guarded) is unrolled over them with constant
         * indices — the FF path never touches a dynamically-indexed array.
         * The interpreter reads the treated locals through an
         * inline coherent switch (no _vtx[16] array at all). _uaIdx
         * sequencing matches the old ascending-i loop exactly. */
        "    vec4 v0=_v0,v1=_v1,v2=_v2,v3=_v3;\n"
        "    vec4 v4=_v4,v5=_v5,v6=_v6,v7=_v7;\n"
        "    vec4 v8=_v8,v9=_v9,v10=_v10,v11=_v11;\n"
        "    vec4 v12=_v12,v13=_v13,v14=_v14,v15=_v15;\n"
        "    if ((u_uniform_attrs | u_compressed_attrs | u_swizzle_attrs) != 0u) {\n"
        "        int _uaIdx = 0;\n"
    );
    for (int i = 0; i < 16; i++) {
        mstring_append_fmt(s,
        "        if ((u_uniform_attrs & (1u << %d)) != 0u) {\n"
        "            v%d = inlineValue[_uaIdx];\n"
        "            _uaIdx++;\n"
        "        } else if ((u_compressed_attrs & (1u << %d)) != 0u) {\n"
        "            int cmp%d = floatBitsToInt(v%d.x);\n"
        "            float cx%d = float(bitfieldExtract(cmp%d, 0,  11)) / 1023.0;\n"
        "            float cy%d = float(bitfieldExtract(cmp%d, 11, 11)) / 1023.0;\n"
        "            float cz%d = float(bitfieldExtract(cmp%d, 22, 10)) / 511.0;\n"
        "            v%d = vec4(cx%d, cy%d, cz%d, 1.0);\n"
        "        } else if ((u_swizzle_attrs & (1u << %d)) != 0u) {\n"
        "            v%d = v%d.bgra;\n"
        "        }\n",
        i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
    }
    mstring_append(s,
        "    }\n"
        "\n"
        "    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    precise vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    precise vec4 oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    vec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    vec4 oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    float oPts = 0.0;\n"
        "    float oFog = 0.0;\n"
        "    vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    vec4 oT1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    vec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    vec4 oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "\n"
    );

    mstring_append(s,
        "    bool is_ff = (u_uber_vsh_flags & 1u) != 0u;\n");

    mstring_append(s,
        "    vec4 tPosition = v0;\n"
        "    vec4 tPositionEye = v0;\n"
        "    vec3 tNormal = vec3(0.0);\n"
        "\n"
        "    if (is_ff) {\n"
        "        vec4 pos = v0;\n"
        "        vec3 norm = v2.xyz;\n"
        "        if (u_skinning_mode == 0) {\n"
        "            tPosition = pos * modelViewMat0;\n"
        "            tNormal = (vec4(norm, 0.0) * invModelViewMat0).xyz;\n"
        "        } else {\n"
        "            bool mix_weights = (u_skinning_mode & 1) != 0;\n"
        "            int count = (u_skinning_mode + 3) / 2;\n"
        "            tPosition = vec4(0.0);\n"
        "            tNormal = vec3(0.0);\n"
        "            float weight_n = 1.0;\n"
        "            for (int si = 0; si < count && si < 4; si++) {\n"
        "                float wi;\n"
        "                if (mix_weights) {\n"
        "                    if (si < count - 1) {\n"
        "                        wi = v1[si]; weight_n -= wi;\n"
        "                    } else {\n"
        "                        wi = weight_n;\n"
        "                    }\n"
        "                } else {\n"
        "                    wi = v1[si];\n"
        "                }\n"
        "                mat4 mvm = si==0 ? modelViewMat0 : si==1 ? modelViewMat1 :\n"
        "                           si==2 ? modelViewMat2 : modelViewMat3;\n"
        "                mat4 imv = si==0 ? invModelViewMat0 : si==1 ? invModelViewMat1 :\n"
        "                           si==2 ? invModelViewMat2 : invModelViewMat3;\n"
        "                tPosition += (pos * mvm) * wi;\n"
        "                tNormal += (vec4(norm, 0.0) * imv).xyz * wi;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        bool do_normalize = (u_uber_vsh_flags & 0x100u) != 0u;\n"
        "        if (do_normalize) tNormal = normalize(tNormal);\n"
        "\n"
        "        oD0 = v3;\n"
        "        oD1 = v4;\n"
        "        oB0 = v7;\n"
        "        oB1 = v8;\n"
        "\n"
        "        bool lighting = (u_uber_vsh_flags & 0x200u) != 0u;\n"
        "        if (lighting) {\n"
        "            uint mcs = u_mat_color_src_packed;\n"
        "            uint emis_src = mcs & 3u;\n"
        "            uint amb_src  = (mcs >> 2u) & 3u;\n"
        "            uint diff_src = (mcs >> 4u) & 3u;\n"
        "            uint spec_src = (mcs >> 6u) & 3u;\n"
        "            float alpha_val = diff_src==0u ? material_alpha :\n"
        "                              diff_src==2u ? v4.a : v3.a;\n"
        "            vec3 amb_col = amb_src==0u ? sceneAmbientColor :\n"
        "                          amb_src==2u ? v4.rgb : v3.rgb;\n"
        "            oD0 = vec4(amb_col, alpha_val);\n"
        "            vec3 emis_col = emis_src==0u ? sceneAmbientColor :\n"
        "                           emis_src==2u ? v4.rgb : v3.rgb;\n"
        "            oD0.rgb *= materialEmissionColor;\n"
        "            oD0.rgb += emis_col;\n"
        "            oD1 = vec4(0.0, 0.0, 0.0, v4.a);\n"
        "\n"
        "            bool local_eye = (u_uber_vsh_flags & 0x400u) != 0u;\n"
        "            vec3 VPeye = vec3(0.0, 0.0, 0.0);\n"
        "            if (local_eye) {\n"
        "                VPeye = normalize(eyePosition.xyz/eyePosition.w - tPosition.xyz/tPosition.w);\n"
        "            }\n"
        "\n"
        /* bound the loop by the highest enabled light (from
         * the existing packed mask — no new uniforms). Iterations executed
         * are identical (higher lights were disabled -> continue anyway). */
        "            int uber_max_li = (u_light_enable_packed == 0u)\n"
        "                ? -1 : (findMSB(u_light_enable_packed) >> 1);\n"
        "            for (int li = 0; li <= uber_max_li; li++) {\n"
        "                uint ltype = (u_light_enable_packed >> (li*2u)) & 3u;\n"
        "                if (ltype == 0u) continue;\n"
        "                float attenuation = 1.0;\n"
        "                precise float nDotVP = 0.0;\n"
        "                precise float nDotHV = 0.0;\n"
        "                if (ltype == 1u) {\n"
        "                    vec3 ldir = normalize(lightInfiniteDirection[li]);\n"
        "                    nDotVP = max(0.0, dot(tNormal, ldir));\n"
        "                    nDotHV = local_eye ? max(0.0, dot(tNormal, normalize(ldir + VPeye)))\n"
        "                                      : max(0.0, dot(tNormal, lightInfiniteHalfVector[li]));\n"
        "                } else {\n"
        "                    vec3 tPos = tPosition.xyz / tPosition.w;\n"
        "                    vec3 VP = lightLocalPosition[li] - tPos;\n"
        "                    float d = length(VP);\n"
        "                    if (d > ltc1[" stringify(NV_IGRAPH_XF_LTC1_r0) " + li].x) continue;\n"
        "                    VP = normalize(VP);\n"
        "                    attenuation = 1.0 / (lightLocalAttenuation[li].x\n"
        "                        + lightLocalAttenuation[li].y * d\n"
        "                        + lightLocalAttenuation[li].z * d * d);\n"
        "                    nDotVP = max(0.0, dot(tNormal, VP));\n"
        "                    nDotHV = max(0.0, dot(tNormal, normalize(VP + (local_eye ? VPeye : vec3(0.0)))));\n"
        "                    if (ltype == 3u) {\n"
        "                        vec4 spotDir = ltctxa[" stringify(NV_IGRAPH_XF_LTCTXA_L0_SPT) " + li*2];\n"
        "                        float invScale = 1.0/length(spotDir.xyz);\n"
        "                        if (invScale > 0.0) {\n"
        "                            float cosHalfPhi = -invScale*spotDir.w;\n"
        "                            float cosHalfTheta = invScale + cosHalfPhi;\n"
        "                            float spotDirDotVP = dot(spotDir.xyz, VP);\n"
        "                            float rho = invScale * spotDirDotVP;\n"
        "                            if (rho <= cosHalfPhi) attenuation = 0.0;\n"
        "                            else if (rho <= cosHalfTheta)\n"
        "                                attenuation *= spotDirDotVP + spotDir.w;\n"
        "                        }\n"
        "                    }\n"
        "                }\n"
        "                precise float pf = (nDotVP == 0.0 || nDotHV == 0.0) ? 0.0\n"
        "                           : pow(nDotHV, specularPower);\n"
        "                precise vec3 la = ltctxb[" stringify(NV_IGRAPH_XF_LTCTXB_L0_AMB) " + li*6].xyz * attenuation;\n"
        "                precise vec3 ld = ltctxb[" stringify(NV_IGRAPH_XF_LTCTXB_L0_DIF) " + li*6].xyz * attenuation * nDotVP;\n"
        "                precise vec3 ls = ltctxb[" stringify(NV_IGRAPH_XF_LTCTXB_L0_SPC) " + li*6].xyz * attenuation * pf;\n"
        "                oD0.xyz += la;\n"
        "                oD0.xyz += diff_src==0u ? ld : diff_src==2u ? v4.rgb*ld : v3.rgb*ld;\n"
        "                oD1.xyz += spec_src==0u ? ls : spec_src==2u ? v4.rgb*ls : v3.rgb*ls;\n"
        "            }\n"
        "            oB0 = v7;\n"
        "            oB1 = v8;\n"
        "        }\n"
        "\n"
        "        bool spec_enable = (u_uber_vsh_flags & 4u) != 0u;\n"
        "        if (!spec_enable) {\n"
        "            oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "            oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        } else {\n"
        "            bool sep_spec = (u_uber_vsh_flags & 0x8u) != 0u;\n"
        "            if (!sep_spec) {\n"
        "                if (lighting) {\n"
        "                    oD0.xyz += oD1.xyz;\n"
        "                    oB0.xyz += oB1.xyz;\n"
        "                }\n"
        "                oD1 = v4;\n"
        "                oB1 = v8;\n"
        "            }\n"
        "            if ((u_uber_vsh_flags & 0x10u) != 0u) {\n"
        "                oD1.a = 1.0;\n"
        "                oB1.a = 1.0;\n"
        "            }\n"
        "        }\n"
        "\n"
        "        oT0.x = v9.x;  oT0.y = v9.y;  oT0.z = v9.z;  oT0.w = v9.w;\n"
        "        oT1.x = v10.x; oT1.y = v10.y; oT1.z = v10.z; oT1.w = v10.w;\n"
        "        oT2.x = v11.x; oT2.y = v11.y; oT2.z = v11.z; oT2.w = v11.w;\n"
        "        oT3.x = v12.x; oT3.y = v12.y; oT3.z = v12.z; oT3.w = v12.w;\n"
    );

    /* the texgen block, unrolled at generation time with
     * literal stage/component indices — eliminates the per-vertex oTex[4]
     * local array (the dynamic [ti][tj] stores forced it into local
     * memory), the runtime texplane select chains, and the
     * dynamic tNormal[tj]/r[tj] component reads. Per-component expressions
     * and write order are identical to the rolled form. */
    for (int ti = 0; ti < 4; ti++) {
        static const char comp[4] = { 'x', 'y', 'z', 'w' };
        static const char *const plane[4] = { "S", "T", "R", "Q" };
        mstring_append_fmt(s,
            "        if (u_texgen_packed[%d] != 0u) {\n"
            "            uint tgp = u_texgen_packed[%d];\n",
            ti, ti);
        for (int tj = 0; tj < 4; tj++) {
            mstring_append_fmt(s,
            "            { uint tg = (tgp >> %du) & 0x7u;\n", tj * 3);
            mstring_append_fmt(s,
            "              if (tg == 1u) oT%d.%c = dot(texPlane%s%d, tPosition);\n"
            "              else if (tg == 2u) oT%d.%c = dot(texPlane%s%d, pos);\n",
            ti, comp[tj], plane[tj], ti,
            ti, comp[tj], plane[tj], ti);
            if (tj < 2) {
                mstring_append_fmt(s,
            "              else if (tg == 3u) {\n"
            "                  vec3 u = normalize(tPosition.xyz);\n"
            "                  vec3 r = reflect(u, tNormal);\n"
            "                  float invM = 1.0/(2.0*length(r + vec3(0.0,0.0,1.0)));\n"
            "                  oT%d.%c = r.%c*invM+0.5;\n"
            "              }\n",
                ti, comp[tj], comp[tj]);
            }
            if (tj < 3) {
                mstring_append_fmt(s,
            "              else if (tg == 4u) oT%d.%c = tNormal.%c;\n"
            "              else if (tg == 5u) {\n"
            "                  vec3 u = normalize(tPosition.xyz);\n"
            "                  vec3 r = reflect(u, tNormal);\n"
            "                  oT%d.%c = r.%c;\n"
            "              }\n",
                ti, comp[tj], comp[tj],
                ti, comp[tj], comp[tj]);
            }
            mstring_append(s, "            }\n");
        }
        mstring_append(s, "        }\n");
    }

    mstring_append(s,
        "\n"
        "        if ((u_uber_vsh_flags & 0x800u) != 0u) { precise float _r0=dot(oT0,texMat0[0]); precise float _r1=dot(oT0,texMat0[1]); precise float _r2=dot(oT0,texMat0[2]); precise float _r3=dot(oT0,texMat0[3]); oT0=vec4(_r0,_r1,_r2,_r3); }\n"
        "        if ((u_uber_vsh_flags & 0x1000u) != 0u) { precise float _r0=dot(oT1,texMat1[0]); precise float _r1=dot(oT1,texMat1[1]); precise float _r2=dot(oT1,texMat1[2]); precise float _r3=dot(oT1,texMat1[3]); oT1=vec4(_r0,_r1,_r2,_r3); }\n"
        "        if ((u_uber_vsh_flags & 0x2000u) != 0u) { precise float _r0=dot(oT2,texMat2[0]); precise float _r1=dot(oT2,texMat2[1]); precise float _r2=dot(oT2,texMat2[2]); precise float _r3=dot(oT2,texMat2[3]); oT2=vec4(_r0,_r1,_r2,_r3); }\n"
        "        if ((u_uber_vsh_flags & 0x4000u) != 0u) { precise float _r0=dot(oT3,texMat3[0]); precise float _r1=dot(oT3,texMat3[1]); precise float _r2=dot(oT3,texMat3[2]); precise float _r3=dot(oT3,texMat3[3]); oT3=vec4(_r0,_r1,_r2,_r3); }\n"
        "\n"
        "        tPositionEye = tPosition;\n"
        "        if (u_skinning_mode == 0) tPosition = pos;\n"
        "        oPos = tPosition * MVP;\n"
        "\n"
        "        if ((u_uber_vsh_flags & 0x20u) != 0u) {\n"
        "            precise float d_e = length(v0 * modelViewMat0);\n"
        "            precise float ptMinSize = min(u_point_params[7], 63.875);\n"
        "            precise float ptMaxSize = min(u_point_params[3] + ptMinSize, 63.875);\n"
        "            precise float pt_atten = u_point_params[0] + u_point_params[1] * d_e + u_point_params[2] * d_e * d_e;\n"
        "            precise float pt_inv = 1.0/sqrt(pt_atten) + u_point_params[6];\n"
        "            precise float pt_scaled = pt_inv * u_point_params[3] + u_point_params[7];\n"
        "            oPts = clamp(pt_scaled, ptMinSize, ptMaxSize) * float(u_surface_scale_factor);\n"
        "        } else {\n"
        "            oPts = max(1.0, u_point_size) * float(u_surface_scale_factor);\n"
        "        }\n"
        "    } else {\n"
        /* register files are individual globals (coherent
         * switch accessors at file scope) — no dynamically-indexed local
         * arrays (local-memory trap), no per-vertex 16-vec4 _vtx copy
         * (the array replaced by an inline coherent switch at the read). */
        "        g_vR0 = vec4(0.0); g_vR1 = vec4(0.0); g_vR2 = vec4(0.0);\n"
        "        g_vR3 = vec4(0.0); g_vR4 = vec4(0.0); g_vR5 = vec4(0.0);\n"
        "        g_vR6 = vec4(0.0); g_vR7 = vec4(0.0); g_vR8 = vec4(0.0);\n"
        "        g_vR9 = vec4(0.0); g_vR10 = vec4(0.0); g_vR11 = vec4(0.0);\n"
        "        g_voR0 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR1 = vec4(0.0);\n"
        "        g_voR2 = vec4(0.0);\n"
        "        g_voR3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR4 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR5 = vec4(0.0);\n"
        "        g_voR6 = vec4(0.0);\n"
        "        g_voR7 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR8 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR9 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR10 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR11 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        g_voR12 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        int A0 = 0;\n"
        "\n"
        /* single precomputed bound for the interpreter loop. */
        "        int uber_pc_end = min(int(u_program_length), 136);\n"
        "        for (int pc = 0; pc < uber_pc_end; pc++) {\n"
        "            uint w1 = u_program_data[pc*3+0];\n"
        "            uint w2 = u_program_data[pc*3+1];\n"
        "            uint w3 = u_program_data[pc*3+2];\n"
        "\n"
        "            uint ilu_op = (w1 >> 25u) & 0x7u;\n"
        "            uint mac_op = (w1 >> 21u) & 0xFu;\n"
        "\n"
        "            uint v_idx = (w1 >> 9u) & 0xFu;\n"
        /* the 8-bit field is the pre-resolved unclamped
         * base (packed CPU-side at program upload) — identical value to the
         * old in-shader arithmetic by construction. */
        "            int ci = int((w1 >> 13u) & 0xFFu);\n"
        "            if ((w3 & 2u) != 0u) ci += A0;\n"
        "            ci = clamp(ci, 0, 191);\n"
        /* inline coherent switch (v_idx is warp-uniform) —
         * reads the treated-attr locals directly, no 16-vec4 array build. */
        "            vec4 vtx_v;\n"
        "            switch (v_idx) {\n"
        "            case 0u: vtx_v = v0; break;   case 1u: vtx_v = v1; break;\n"
        "            case 2u: vtx_v = v2; break;   case 3u: vtx_v = v3; break;\n"
        "            case 4u: vtx_v = v4; break;   case 5u: vtx_v = v5; break;\n"
        "            case 6u: vtx_v = v6; break;   case 7u: vtx_v = v7; break;\n"
        "            case 8u: vtx_v = v8; break;   case 9u: vtx_v = v9; break;\n"
        "            case 10u: vtx_v = v10; break; case 11u: vtx_v = v11; break;\n"
        "            case 12u: vtx_v = v12; break; case 13u: vtx_v = v13; break;\n"
        "            case 14u: vtx_v = v14; break; default: vtx_v = v15; break;\n"
        "            }\n"
        "            vec4 const_c = c[ci];\n"
        "\n"
        "            uint a_mux = (w2 >> 26u) & 3u;\n"
        "            uint a_r = (w2 >> 28u) & 0xFu;\n"
        "            vec4 srcA;\n"
        "            if (a_mux == 2u) srcA = vtx_v;\n"
        "            else if (a_mux == 3u) srcA = const_c;\n"
        "            else srcA = uber_vsh_Rrd(a_r);\n" /* >=12 -> oR0 in default */
        "            srcA = vec4(srcA[(w1>>6u)&3u], srcA[(w1>>4u)&3u], srcA[(w1>>2u)&3u], srcA[w1&3u]);\n"
        "            if (((w1 >> 8u) & 1u) != 0u) srcA = -srcA;\n"
        "\n"
        "            uint b_mux = (w2 >> 11u) & 3u;\n"
        "            uint b_r = (w2 >> 13u) & 0xFu;\n"
        "            vec4 srcB;\n"
        "            if (b_mux == 2u) srcB = vtx_v;\n"
        "            else if (b_mux == 3u) srcB = const_c;\n"
        "            else srcB = uber_vsh_Rrd(b_r);\n"
        "            srcB = vec4(srcB[(w2>>23u)&3u], srcB[(w2>>21u)&3u], srcB[(w2>>19u)&3u], srcB[(w2>>17u)&3u]);\n"
        "            if (((w2 >> 25u) & 1u) != 0u) srcB = -srcB;\n"
        "\n"
        "            uint c_mux_s = (w3 >> 28u) & 3u;\n"
        "            uint c_r_s = ((w2 & 3u) << 2u) | ((w3 >> 30u) & 3u);\n"
        "            vec4 srcC;\n"
        "            if (c_mux_s == 2u) srcC = vtx_v;\n"
        "            else if (c_mux_s == 3u) srcC = const_c;\n"
        "            else srcC = uber_vsh_Rrd(c_r_s);\n"
        "            uint csx = (w2 >> 8u) & 3u;\n"
        "            uint csy = (w2 >> 6u) & 3u;\n"
        "            uint csz = (w2 >> 4u) & 3u;\n"
        "            uint csw = (w2 >> 2u) & 3u;\n"
        "            if (ilu_op >= 2u && ilu_op <= 6u) { csy = csx; csz = csx; csw = csx; }\n"
        "            srcC = vec4(srcC[csx], srcC[csy], srcC[csz], srcC[csw]);\n"
        "            if (((w2 >> 10u) & 1u) != 0u) srcC = -srcC;\n"
        "\n"
        "            uint out_r = (w3 >> 20u) & 0xFu;\n"
        "            uint mac_mask = (w3 >> 24u) & 0xFu;\n"
        "            uint ilu_mask = (w3 >> 16u) & 0xFu;\n"
        "            uint o_mask = (w3 >> 12u) & 0xFu;\n"
        "            uint out_mux_s = (w3 >> 2u) & 1u;\n"
        "            uint out_addr = (w3 >> 3u) & 0xFFu;\n"
        "            bool is_paired = (mac_op != 0u && ilu_op != 0u);\n"
        "\n"
        "            vec4 mac_result = vec4(0.0);\n"
        "            bool mac_wrote = false;\n"
        /* switch dispatch (was a 13-deep sequential else-if
         * chain). Case bodies are byte-identical. The ignored ORB bit is
         * deliberately not changed here: a behavior fix would alter pixels for
         * ORB-using content, which is out of scope for this mechanical
         * restructure. */
        "            switch (mac_op) {\n"
        "            case 1u: mac_result = srcA; mac_wrote = true; break;\n"
        "            case 2u: mac_result = nv2a_mul(srcA, srcB); mac_wrote = true; break;\n"
        "            case 3u: mac_result = srcA + srcC; mac_wrote = true; break;\n"
        "            case 4u: mac_result = nv2a_mul(srcA, srcB) + srcC; mac_wrote = true; break;\n"
        "            case 5u: { vec4 m = nv2a_mul(srcA, srcB); mac_result = vec4(m.x+m.y+m.z); mac_wrote = true; break; }\n"
        "            case 6u: { vec4 m = nv2a_mul(vec4(srcA.xyz,1.0), srcB); mac_result = vec4(m.x+m.y+m.z+m.w); mac_wrote = true; break; }\n"
        "            case 7u: { vec4 m = nv2a_mul(srcA, srcB); mac_result = vec4(m.x+m.y+m.z+m.w); mac_wrote = true; break; }\n"
        "            case 8u: mac_result = vec4(1.0, nv2a_mul(srcA, srcB).y, srcA.z, srcB.w); mac_wrote = true; break;\n"
        "            case 9u: mac_result = min(srcA, srcB); mac_wrote = true; break;\n"
        "            case 10u: mac_result = max(srcA, srcB); mac_wrote = true; break;\n"
        "            case 11u: mac_result = vec4(lessThan(srcA, srcB)); mac_wrote = true; break;\n"
        "            case 12u: mac_result = vec4(greaterThanEqual(srcA, srcB)); mac_wrote = true; break;\n"
        "            case 13u: A0 = int(floor(srcA.x + 0.001)); break;\n"
        "            default: break;\n"
        "            }\n"
        "\n"
        "            if (mac_wrote) {\n"
        "                uint em = mac_mask;\n"
        "                if (is_paired && out_r == 1u) em = 0u;\n"
        /* full-mask fast path skips the destination read entirely. */
        "                if (em == 15u) {\n"
        "                    uber_vsh_Rwr(out_r, mac_result);\n"
        "                } else if (em != 0u) {\n"
        "                    vec4 d = uber_vsh_Rrd(out_r);\n"
        "                    if ((em & 8u) != 0u) d.x = mac_result.x;\n"
        "                    if ((em & 4u) != 0u) d.y = mac_result.y;\n"
        "                    if ((em & 2u) != 0u) d.z = mac_result.z;\n"
        "                    if ((em & 1u) != 0u) d.w = mac_result.w;\n"
        "                    uber_vsh_Rwr(out_r, d);\n"
        "                }\n"
        "            }\n"
        "\n"
        "            vec4 ilu_result = vec4(0.0);\n"
        "            bool ilu_wrote = false;\n"
        "            switch (ilu_op) {\n"
        "            case 1u: ilu_result = srcC; ilu_wrote = true; break;\n"
        "            case 2u: ilu_result = vec4(1.0 / srcC.x); ilu_wrote = true; break;\n"
        "            case 3u: ilu_result = vec4(clampAwayZeroInf(1.0 / srcC.x)); ilu_wrote = true; break;\n"
        "            case 4u: { float rv=srcC.x; if(rv==0.0) ilu_result=vec4(1.0/0.0); else if(isinf(rv)) ilu_result=vec4(0.0); else ilu_result=vec4(inversesqrt(abs(rv))); ilu_wrote = true; break; }\n"
        "            case 5u: { float ev=srcC.x; ilu_result = vec4(exp2(floor(ev)), ev-floor(ev), exp2(ev), 1.0); ilu_wrote = true; break; }\n"
        "            case 6u: { float lv=abs(srcC.x); if(lv==0.0) ilu_result=vec4(-1.0/0.0,1.0,-1.0/0.0,1.0); else { float fl=floor(log2(lv)); ilu_result=vec4(fl,lv/exp2(fl),log2(lv),1.0); } ilu_wrote = true; break; }\n"
        "            case 7u: { float lx=max(srcC.x,0.0),ly=max(srcC.y,0.0); float eps=1.0/256.0; float lw=clamp(srcC.w,-(128.0-eps),128.0-eps); ilu_result = vec4(1.0,lx,(lx>0.0)?exp2(lw*log2(ly)):0.0,1.0); ilu_wrote = true; break; }\n"
        "            default: break;\n"
        "            }\n"
        "\n"
        "            if (ilu_wrote) {\n"
        "                uint ir = is_paired ? 1u : out_r;\n"
        "                if (ilu_mask == 15u) {\n" /* fast path */
        "                    uber_vsh_Rwr(ir, ilu_result);\n"
        "                } else if (ilu_mask != 0u) {\n"
        "                    vec4 d = uber_vsh_Rrd(ir);\n"
        "                    if ((ilu_mask & 8u) != 0u) d.x = ilu_result.x;\n"
        "                    if ((ilu_mask & 4u) != 0u) d.y = ilu_result.y;\n"
        "                    if ((ilu_mask & 2u) != 0u) d.z = ilu_result.z;\n"
        "                    if ((ilu_mask & 1u) != 0u) d.w = ilu_result.w;\n"
        "                    uber_vsh_Rwr(ir, d);\n"
        "                }\n"
        "            }\n"
        "\n"
        "            if (o_mask != 0u) {\n"
        "                vec4 ov = (out_mux_s == 0u) ? mac_result : ilu_result;\n"
        "                uint oreg = out_addr & 0xFu;\n"
        "                if (oreg == 5u) {\n"
        "                    g_voR5.x = ov.x;\n"
        "                } else if (oreg < 13u) {\n"
        "                    if (o_mask == 15u) {\n" /* fast path */
        "                        uber_vsh_oRwr(oreg, ov);\n"
        "                    } else {\n"
        "                        vec4 d = uber_vsh_oRrd(oreg);\n"
        "                        if ((o_mask & 8u) != 0u) d.x = ov.x;\n"
        "                        if ((o_mask & 4u) != 0u) d.y = ov.y;\n"
        "                        if ((o_mask & 2u) != 0u) d.z = ov.z;\n"
        "                        if ((o_mask & 1u) != 0u) d.w = ov.w;\n"
        "                        uber_vsh_oRwr(oreg, d);\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "\n"
        "            if ((w3 & 1u) != 0u) break;\n"
        "        }\n"
        "\n"
        "        oPos = g_voR0;\n"
        "        oD0 = g_voR3;\n"
        "        oD1 = g_voR4;\n"
        "        oFog = g_voR5.x;\n"
        "        oPts = g_voR6.x;\n"
        "        oB0 = g_voR7;\n"
        "        oB1 = g_voR8;\n"
        "        oT0 = g_voR9;\n"
        "        oT1 = g_voR10;\n"
        "        oT2 = g_voR11;\n"
        "        oT3 = g_voR12;\n"
        "        if ((u_uber_vsh_flags & 0x20u) == 0u) {\n"
        "            oPts = u_point_size * float(u_surface_scale_factor);\n"
        "        }\n"
        "    }\n"
        "\n"
        "    bool fog_enable = (u_uber_vsh_flags & 2u) != 0u;\n"
        "    if (!fog_enable) {\n"
        "        oFog = 1.0;\n"
        "    } else {\n"
        "        float fogDistance;\n"
        "        if (is_ff) {\n"
        "            if (u_foggen_mode == 0) fogDistance = clamp(v4.a, 0.0, 1.0);\n"
        "            else if (u_foggen_mode == 1) fogDistance = length(tPositionEye.xyz);\n"
        "            else if (u_foggen_mode == 2) fogDistance = dot(fogPlane.xyz, tPositionEye.xyz) + fogPlane.w;\n"
        "            else if (u_foggen_mode == 3) fogDistance = abs(dot(fogPlane.xyz, tPositionEye.xyz) + fogPlane.w);\n"
        "            else fogDistance = v5.x;\n"
        "        } else {\n"
        "            fogDistance = oFog;\n"
        "        }\n"
        "        float fogFactor = 0.0;\n"
        "        float inf_result = 0.0;\n"
        "        float nan_result = 0.0;\n"
        "        if (u_fog_mode == 0 || u_fog_mode == 4) {\n"
        "            fogFactor = fogParam.x + fogDistance * fogParam.y - 1.0;\n"
        "            inf_result = 1.0; nan_result = 1.0;\n"
        "        } else if (u_fog_mode == 1 || u_fog_mode == 5) {\n"
        "            fogFactor = fogParam.x + exp2(fogDistance * fogParam.y * 16.0) - 1.5;\n"
        "            if (u_fog_mode == 1) { inf_result = 1.0; nan_result = 1.0; }\n"
        "        } else {\n"
        "            fogFactor = fogParam.x + exp2(-fogDistance * fogDistance * fogParam.y * fogParam.y * 32.0) - 1.5;\n"
        "        }\n"
        "        if (u_fog_mode >= 4)\n"
        "            fogFactor = abs(fogFactor);\n"
        "        if (isinf(fogDistance)) oFog = inf_result;\n"
        "        else oFog = clamp(NaNToValue(vec4(fogFactor), nan_result).x, -FLOAT_MAX, FLOAT_MAX);\n"
        "    }\n"
        "\n"
        "    v_vtxD0 = clamp(NaNToOne(oD0), 0.0, 1.0);\n"
        "    v_vtxB0 = clamp(NaNToOne(oB0), 0.0, 1.0);\n"
        "    v_vtxFog = oFog;\n"
        "v_vtxT0 = oT0; v_vtxT1 = oT1; v_vtxT2 = oT2; v_vtxT3 = oT3;\n"
    );
    if (opts.vk_batched) {
        mstring_append(s,
            "    v_uberRec = uber_rec_base + uint(gl_DrawIDARB);\n");
    }
    mstring_append(s,
        "    gl_PointSize = oPts;\n"
        "\n"
        "    if ((u_uber_vsh_flags & 4u) != 0u) {\n"
        "        v_vtxD1 = clamp(NaNToOne(oD1), 0.0, 1.0);\n"
        "        v_vtxB1 = clamp(NaNToOne(oB1), 0.0, 1.0);\n"
        "        if ((u_uber_vsh_flags & 0x10u) != 0u) {\n"
        "            v_vtxD1.w = 1.0;\n"
        "            v_vtxB1.w = 1.0;\n"
        "        }\n"
        "    } else {\n"
        "        v_vtxD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "        v_vtxB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "    }\n"
        "\n"
        "    vec4 vtxPos;\n"
        "    if (is_ff) {\n"
        "        oPos.w = clampAwayZeroInf(oPos.w);\n"
        "        oPos.xy /= oPos.w;\n"
        "        oPos.xy += " GLSL_C(NV_IGRAPH_XF_XFCTX_VPOFF) ".xy;\n"
        "        oPos.xy = roundScreenCoords(oPos.xy);\n"
        "        vtxPos = vec4(oPos.xy, oPos.z / oPos.w, oPos.w);\n"
        "        oPos.z = oPos.z / clipRange.y;\n"
        "        oPos.xy = (2.0 * oPos.xy - surfaceSize) / surfaceSize;\n"
        "        oPos.xy *= oPos.w;\n"
        "    } else {\n"
        "        oPos.xy = roundScreenCoords(oPos.xy);\n"
        "        oPos.w = clampAwayZeroInf(oPos.w);\n"
        "        vtxPos = oPos;\n"
        "        oPos.xy = (2.0 * oPos.xy - surfaceSize) / surfaceSize;\n"
        "        oPos.z = oPos.z / clipRange.y;\n"
        "        oPos.xyz *= oPos.w;\n"
        "    }\n"
        "\n"
    );

    if (opts.vulkan) {
        mstring_append(s,
            "    gl_Position = oPos;\n");
    } else {
        mstring_append(s,
            "    gl_Position = vec4(oPos.x, oPos.y, 2.0*oPos.z - oPos.w, oPos.w);\n");
    }

    mstring_append(s,
        /* v_vtxPos1/2 + v_triMZ deleted - no GS reads them */
        "    v_vtxPos0 = vtxPos;\n"
        "}\n"
    );

    return s;
}

/* ========================================================================
 * Geometry Ubershader
 * ======================================================================== */

MString *pgraph_glsl_gen_uber_geom(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    mstring_append(s,
        "layout(triangles) in;\n"
        "layout(triangle_strip, max_vertices = 3) out;\n"
        "\n"
    );

    uber_vtx_header_vsh_gs(s, opts.vulkan, true, true);
    pgraph_glsl_get_vtx_header(s, opts.vulkan, true, false, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = %d, %s) uniform UberGeomUniforms {\n",
            opts.uber_geom_ubo_binding, uber_ubo_layout(opts));
        mstring_append(s,
            "int u_primitive_mode;\n"
            "int u_polygon_front_mode;\n"
            "int u_polygon_back_mode;\n"
            "uint u_uber_geom_flags;\n"
            "int u_tri_rot0;\n"
            "int u_tri_rot1;\n"
            "};\n");
    } else {
        mstring_append(s,
            "uniform int u_primitive_mode;\n"
            "uniform int u_polygon_front_mode;\n"
            "uniform int u_polygon_back_mode;\n"
            "uniform uint u_uber_geom_flags;\n"
            "uniform int u_tri_rot0;\n"
            "uniform int u_tri_rot1;\n");
    }

    mstring_append(s,
        "float kahan_det(float a, float b, float c, float d) {\n"
        "    precise float cd = c*d;\n"
        "    precise float err = fma(-c, d, cd);\n"
        "    precise float res = fma(a, b, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "\n"
        "ivec3 get_vertex_order(int rot) {\n"
        "    if (rot == 0) return ivec3(0, 1, 2);\n"
        "    if (rot == 1) return ivec3(2, 0, 1);\n"
        "    return ivec3(1, 2, 0);\n"
        "}\n"
        "\n"
    );

    mstring_append_fmt(s,
        /* smooth-mode decoded once per primitive (set at
         * the top of main), not per emitted vertex */
        "bool g_smod;\n"
        "void emit_vtx(int index, int prov, vec4 p0, vec4 p1, vec4 p2, float tmz) {\n"
        "    gl_Position = gl_in[index].gl_Position;\n"
        "    gl_PointSize = gl_in[index].gl_PointSize;\n"
        "    int ci = g_smod ? index : prov;\n"
        "    vtxD0 = v_vtxD0[ci];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxD1 = v_vtxD1[ci];\n"
        "    vtxB0 = v_vtxB0[ci];\n"
        "    vtxB1 = v_vtxB1[ci];\n"
        "    vtxFog = v_vtxFog[index];\n"
        "    vtxT0 = v_vtxT0[index];\n"
        "    vtxT1 = v_vtxT1[index];\n"
        "    vtxT2 = v_vtxT2[index];\n"
        "    vtxT3 = v_vtxT3[index];\n"
        "    vtxPos0 = p0;\n"
        "    vtxPos1 = p1;\n"
        "    vtxPos2 = p2;\n"
        "    triMZ = tmz;\n" /* sanitized once in main */
        "    EmitVertex();\n"
        "}\n"
        "\n"
    );

    mstring_append_fmt(s,
        "void main() {\n"
        "    bool z_persp = (u_uber_geom_flags & 4u) != 0u;\n"
        "    bool smod = (u_uber_geom_flags & 1u) != 0u;\n"
        "    g_smod = smod;\n"
        "    bool first_prov = (u_uber_geom_flags & 2u) != 0u;\n"
        "\n"
        "    ivec3 v;\n"
        "    if (u_tri_rot0 == u_tri_rot1) {\n"
        "        v = get_vertex_order(u_tri_rot0);\n"
        "    } else {\n"
        "        v = (gl_PrimitiveIDIn & 1) == 0\n"
        "            ? get_vertex_order(u_tri_rot0)\n"
        "            : get_vertex_order(u_tri_rot1);\n"
        "    }\n"
        "\n"
        "    int prov;\n"
        "    if (u_primitive_mode == %d) {\n"
        "        prov = v[2];\n"
        "    } else if (first_prov) {\n"
        "        prov = v[0];\n"
        "    } else if (u_primitive_mode == %d) {\n"
        "        prov = v[2 - (gl_PrimitiveIDIn & 1)];\n"
        "    } else if (u_primitive_mode == %d) {\n"
        "        prov = v[1];\n"
        "    } else {\n"
        "        prov = v[2];\n"
        "    }\n"
        "\n"
        "    vec4 sp0 = v_vtxPos0[v[0]];\n"
        "    vec4 sp1 = v_vtxPos0[v[1]];\n"
        "    vec4 sp2 = v_vtxPos0[v[2]];\n"
        "\n"
        "    float tmz;\n"
        "    if (z_persp) {\n"
        "        mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "        precise vec2 b = vec2(sp0.w - sp1.w, sp0.w - sp2.w);\n"
        "        b /= vec2(sp1.w, sp2.w) * sp0.w;\n"
        "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "        tmz = max(abs(dzx), abs(dzy));\n"
        "    } else {\n"
        "        mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "        precise vec2 b = vec2(sp1.z - sp0.z, sp2.z - sp0.z);\n"
        "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "        tmz = max(abs(dzx), abs(dzy));\n"
        "    }\n"
        "\n"
        "    tmz = (isnan(tmz) || isinf(tmz)) ? 0.0 : tmz;\n"
        "    emit_vtx(v[0], prov, sp0, sp1, sp2, tmz);\n"
        "    emit_vtx(v[1], prov, sp0, sp1, sp2, tmz);\n"
        "    emit_vtx(v[2], prov, sp0, sp1, sp2, tmz);\n"
        "    EndPrimitive();\n"
        "}\n",
        PRIM_TYPE_POLYGON, PRIM_TYPE_TRIANGLE_STRIP, PRIM_TYPE_TRIANGLE_FAN
    );

    return s;
}

/* NV passthrough tri GS — smooth-shading-only variant (the
 * flat path needs the runtime provoking-vertex select, which passthrough
 * cannot express; the classic tri GS keeps serving flat draws). Per-vertex
 * varyings forward unchanged (locations 0-8, matching the VSH-out/PSH-in
 * smooth qualifiers); vtxPos0-2 + triMZ are per-primitive non-passthrough
 * outputs (flat in the PSH — provoking pick of a replicated value). The
 * rotation + tmz math is byte-identical to the classic GS so the per-prim
 * values match it bit-for-bit. VK-only (locations required). */
/* shared tmz/rotation body, defined below. */
static void uber_geom_tri_passthrough_body(MString *s, bool vk_batched);

MString *pgraph_glsl_gen_uber_geom_tri_passthrough(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts); /* (always vulkan here) */

    mstring_append(s,
        "#extension GL_NV_geometry_shader_passthrough : require\n"
        "layout(triangles) in;\n"
        "\n"
        "layout(passthrough) in gl_PerVertex {\n"
        "    vec4 gl_Position;\n"
        "    float gl_PointSize;\n"
        "} gl_in[];\n"
        "layout(location = 0, passthrough) in vec4 v_vtxD0[];\n"
        "layout(location = 1, passthrough) in vec4 v_vtxD1[];\n"
        "layout(location = 2, passthrough) in vec4 v_vtxB0[];\n"
        "layout(location = 3, passthrough) in vec4 v_vtxB1[];\n"
        "layout(location = 4, passthrough) in float v_vtxFog[];\n"
        "layout(location = 5, passthrough) in vec4 v_vtxT0[];\n"
        "layout(location = 6, passthrough) in vec4 v_vtxT1[];\n"
        "layout(location = 7, passthrough) in vec4 v_vtxT2[];\n"
        "layout(location = 8, passthrough) in vec4 v_vtxT3[];\n"
        "layout(location = 9) in vec4 v_vtxPos0[];\n"
        "\n"
        "layout(location = 9) flat out vec4 vtxPos0;\n"
        "layout(location = 10) flat out vec4 vtxPos1;\n"
        "layout(location = 11) flat out vec4 vtxPos2;\n"
        "layout(location = 12) flat out float triMZ;\n"
        "\n"
    );
    if (opts.vk_batched) { /* regular pair (cannot
        * passthrough-relocate 10 -> 13); written once in main like the
        * vtxPos outs. */
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    mstring_append_fmt(s,
        "layout(binding = %d, %s) uniform UberGeomUniforms {\n"
        "int u_primitive_mode;\n"
        "int u_polygon_front_mode;\n"
        "int u_polygon_back_mode;\n"
        "uint u_uber_geom_flags;\n"
        "int u_tri_rot0;\n"
        "int u_tri_rot1;\n"
        "};\n",
        opts.uber_geom_ubo_binding, uber_ubo_layout(opts));

    uber_geom_tri_passthrough_body(s, opts.vk_batched);

    return s;
}

/* The shared tmz/rotation body of both passthrough tri GS arms. */
static void uber_geom_tri_passthrough_body(MString *s, bool vk_batched)
{
    mstring_append(s,
        "float kahan_det(float a, float b, float c, float d) {\n"
        "    precise float cd = c*d;\n"
        "    precise float err = fma(-c, d, cd);\n"
        "    precise float res = fma(a, b, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "\n"
        "ivec3 get_vertex_order(int rot) {\n"
        "    if (rot == 0) return ivec3(0, 1, 2);\n"
        "    if (rot == 1) return ivec3(2, 0, 1);\n"
        "    return ivec3(1, 2, 0);\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    bool z_persp = (u_uber_geom_flags & 4u) != 0u;\n"
        "\n"
        "    ivec3 v;\n"
        "    if (u_tri_rot0 == u_tri_rot1) {\n"
        "        v = get_vertex_order(u_tri_rot0);\n"
        "    } else {\n"
        "        v = (gl_PrimitiveIDIn & 1) == 0\n"
        "            ? get_vertex_order(u_tri_rot0)\n"
        "            : get_vertex_order(u_tri_rot1);\n"
        "    }\n"
        "\n"
        "    vec4 sp0 = v_vtxPos0[v[0]];\n"
        "    vec4 sp1 = v_vtxPos0[v[1]];\n"
        "    vec4 sp2 = v_vtxPos0[v[2]];\n"
        "\n"
        "    float tmz;\n"
        "    if (z_persp) {\n"
        "        mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "        precise vec2 b = vec2(sp0.w - sp1.w, sp0.w - sp2.w);\n"
        "        b /= vec2(sp1.w, sp2.w) * sp0.w;\n"
        "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "        tmz = max(abs(dzx), abs(dzy));\n"
        "    } else {\n"
        "        mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "        precise vec2 b = vec2(sp1.z - sp0.z, sp2.z - sp0.z);\n"
        "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "        tmz = max(abs(dzx), abs(dzy));\n"
        "    }\n"
        "\n"
        "    vtxPos0 = sp0;\n");
    if (vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxPos1 = sp1;\n"
        "    vtxPos2 = sp2;\n"
        "    triMZ = (isnan(tmz) || isinf(tmz)) ? 0.0 : tmz;\n"
        "}\n"
    );
}

MString *pgraph_glsl_gen_uber_geom_tri_wireframe(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    mstring_append(s,
        "layout(triangles) in;\n"
        "layout(line_strip, max_vertices = 6) out;\n"
        "\n"
    );

    uber_vtx_header_vsh_gs(s, opts.vulkan, true, true);
    pgraph_glsl_get_vtx_header(s, opts.vulkan, true, false, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = %d, %s) uniform UberGeomUniforms {\n",
            opts.uber_geom_ubo_binding, uber_ubo_layout(opts));
        mstring_append(s,
            "int u_primitive_mode;\n"
            "int u_polygon_front_mode;\n"
            "int u_polygon_back_mode;\n"
            "uint u_uber_geom_flags;\n"
            "int u_tri_rot0;\n"
            "int u_tri_rot1;\n"
            "};\n");
    } else {
        mstring_append(s,
            "uniform int u_primitive_mode;\n"
            "uniform int u_polygon_front_mode;\n"
            "uniform int u_polygon_back_mode;\n"
            "uniform uint u_uber_geom_flags;\n"
            "uniform int u_tri_rot0;\n"
            "uniform int u_tri_rot1;\n");
    }

    mstring_append(s,
        "float kahan_det(float a, float b, float c, float d) {\n"
        "    precise float cd = c*d;\n"
        "    precise float err = fma(-c, d, cd);\n"
        "    precise float res = fma(a, b, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "\n"
        "ivec3 get_vertex_order(int rot) {\n"
        "    if (rot == 0) return ivec3(0, 1, 2);\n"
        "    if (rot == 1) return ivec3(2, 0, 1);\n"
        "    return ivec3(1, 2, 0);\n"
        "}\n"
        "\n"
        /* dz pre-sanitized in main; smod decoded once per
         * primitive (g_smod - the pattern this variant had missed). */
        "bool g_smod;\n"
        "void emit_line_seg(int i0, int i1, int prov, float dz) {\n"
        "    vec2 delta = v_vtxPos0[i1].xy - v_vtxPos0[i0].xy;\n"
        "    vec4 p0 = v_vtxPos0[i0];\n"
        "    vec4 p1 = v_vtxPos0[i1];\n"
        "    vec4 p2 = vec4(vec2(-delta.y, delta.x) + p0.xy, p0.zw);\n"
        "    float tmz = dz;\n"
        "\n"
        "    int ci0 = g_smod ? i0 : prov;\n"
        "    gl_Position = gl_in[i0].gl_Position;\n"
        "    gl_PointSize = gl_in[i0].gl_PointSize;\n"
        "    vtxD0 = v_vtxD0[ci0]; vtxD1 = v_vtxD1[ci0];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxB0 = v_vtxB0[ci0]; vtxB1 = v_vtxB1[ci0];\n"
        "    vtxFog = v_vtxFog[i0];\n"
        "    vtxT0 = v_vtxT0[i0]; vtxT1 = v_vtxT1[i0];\n"
        "    vtxT2 = v_vtxT2[i0]; vtxT3 = v_vtxT3[i0];\n"
        "    vtxPos0 = p0; vtxPos1 = p1; vtxPos2 = p2;\n"
        "    triMZ = tmz;\n"
        "    EmitVertex();\n"
        "\n"
        "    int ci1 = g_smod ? i1 : prov;\n"
        "    gl_Position = gl_in[i1].gl_Position;\n"
        "    gl_PointSize = gl_in[i1].gl_PointSize;\n"
        "    vtxD0 = v_vtxD0[ci1]; vtxD1 = v_vtxD1[ci1];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxB0 = v_vtxB0[ci1]; vtxB1 = v_vtxB1[ci1];\n"
        "    vtxFog = v_vtxFog[i1];\n"
        "    vtxT0 = v_vtxT0[i1]; vtxT1 = v_vtxT1[i1];\n"
        "    vtxT2 = v_vtxT2[i1]; vtxT3 = v_vtxT3[i1];\n"
        "    vtxPos0 = p0; vtxPos1 = p1; vtxPos2 = p2;\n"
        "    triMZ = tmz;\n"
        "    EmitVertex();\n"
        "    EndPrimitive();\n"
        "}\n"
        "\n"
    );

    mstring_append_fmt(s,
        "void main() {\n"
        "    bool z_persp = (u_uber_geom_flags & 4u) != 0u;\n"
        "    bool first_prov = (u_uber_geom_flags & 2u) != 0u;\n"
        "\n"
        "    ivec3 v;\n"
        "    if (u_tri_rot0 == u_tri_rot1) {\n"
        "        v = get_vertex_order(u_tri_rot0);\n"
        "    } else {\n"
        "        v = (gl_PrimitiveIDIn & 1) == 0\n"
        "            ? get_vertex_order(u_tri_rot0)\n"
        "            : get_vertex_order(u_tri_rot1);\n"
        "    }\n"
        "\n"
        "    int prov;\n"
        "    if (u_primitive_mode == %d) {\n"
        "        prov = v[2];\n"
        "    } else if (first_prov) {\n"
        "        prov = v[0];\n"
        "    } else if (u_primitive_mode == %d) {\n"
        "        prov = v[2 - (gl_PrimitiveIDIn & 1)];\n"
        "    } else if (u_primitive_mode == %d) {\n"
        "        prov = v[1];\n"
        "    } else {\n"
        "        prov = v[2];\n"
        "    }\n"
        "\n"
        "    vec4 sp0 = v_vtxPos0[v[0]];\n"
        "    vec4 sp1 = v_vtxPos0[v[1]];\n"
        "    vec4 sp2 = v_vtxPos0[v[2]];\n"
        "\n"
        "    float dz;\n"
        "    if (z_persp) {\n"
        "        mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "        precise vec2 b = vec2(sp0.w - sp1.w, sp0.w - sp2.w);\n"
        "        b /= vec2(sp1.w, sp2.w) * sp0.w;\n"
        "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "        dz = max(abs(dzx), abs(dzy));\n"
        "    } else {\n"
        "        mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "        precise vec2 b = vec2(sp1.z - sp0.z, sp2.z - sp0.z);\n"
        "        float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "        float dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        float dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "        dz = max(abs(dzx), abs(dzy));\n"
        "    }\n"
        "\n"
        "    dz = (isnan(dz) || isinf(dz)) ? 0.0 : dz;\n"
        "    g_smod = (u_uber_geom_flags & 1u) != 0u;\n"
        "    emit_line_seg(v[0], v[1], prov, dz);\n"
        "    emit_line_seg(v[1], v[2], prov, dz);\n"
        "    emit_line_seg(v[2], v[0], prov, dz);\n"
        "}\n",
        PRIM_TYPE_POLYGON, PRIM_TYPE_TRIANGLE_STRIP, PRIM_TYPE_TRIANGLE_FAN
    );

    return s;
}

MString *pgraph_glsl_gen_uber_geom_quad(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    mstring_append(s,
        "layout(lines_adjacency) in;\n"
        "layout(triangle_strip, max_vertices = 6) out;\n"
        "\n"
    );

    uber_vtx_header_vsh_gs(s, opts.vulkan, true, true);
    pgraph_glsl_get_vtx_header(s, opts.vulkan, true, false, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = %d, %s) uniform UberGeomUniforms {\n",
            opts.uber_geom_ubo_binding, uber_ubo_layout(opts));
        mstring_append(s,
            "int u_primitive_mode;\n"
            "int u_polygon_front_mode;\n"
            "int u_polygon_back_mode;\n"
            "uint u_uber_geom_flags;\n"
            "int u_tri_rot0;\n"
            "int u_tri_rot1;\n"
            "};\n");
    } else {
        mstring_append(s,
            "uniform int u_primitive_mode;\n"
            "uniform int u_polygon_front_mode;\n"
            "uniform int u_polygon_back_mode;\n"
            "uniform uint u_uber_geom_flags;\n"
            "uniform int u_tri_rot0;\n"
            "uniform int u_tri_rot1;\n");
    }

    mstring_append(s,
        "float kahan_det(float a, float b, float c, float d) {\n"
        "    precise float cd = c*d;\n"
        "    precise float err = fma(-c, d, cd);\n"
        "    precise float res = fma(a, b, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "\n"
        "vec4 calc_triz_dz(vec4 sp0, vec4 sp1, vec4 sp2, bool z_persp) {\n"
        "    mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "    float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "    float dzx, dzy;\n"
        "    if (z_persp) {\n"
        "        precise vec2 b = vec2(sp0.w - sp1.w, sp0.w - sp2.w);\n"
        "        b /= vec2(sp1.w, sp2.w) * sp0.w;\n"
        "        dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "    } else {\n"
        "        precise vec2 b = vec2(sp1.z - sp0.z, sp2.z - sp0.z);\n"
        "        dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "    }\n"
        "    return vec4(max(abs(dzx), abs(dzy)), 0, 0, 0);\n"
        "}\n"
        "\n"
        /* smooth-mode decoded once per primitive (set at
         * the top of main), not per emitted vertex */
        "bool g_smod;\n"
        "void emit_vtx(int index, int prov, vec4 p0, vec4 p1, vec4 p2, float tmz) {\n"
        "    gl_Position = gl_in[index].gl_Position;\n"
        "    gl_PointSize = gl_in[index].gl_PointSize;\n"
        "    int ci = g_smod ? index : prov;\n"
        "    vtxD0 = v_vtxD0[ci];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxD1 = v_vtxD1[ci];\n"
        "    vtxB0 = v_vtxB0[ci];\n"
        "    vtxB1 = v_vtxB1[ci];\n"
        "    vtxFog = v_vtxFog[index];\n"
        "    vtxT0 = v_vtxT0[index];\n"
        "    vtxT1 = v_vtxT1[index];\n"
        "    vtxT2 = v_vtxT2[index];\n"
        "    vtxT3 = v_vtxT3[index];\n"
        "    vtxPos0 = p0;\n"
        "    vtxPos1 = p1;\n"
        "    vtxPos2 = p2;\n"
        "    triMZ = tmz;\n" /* sanitized once in main */
        "    EmitVertex();\n"
        "}\n"
        "\n"
    );

    mstring_append_fmt(s,
        "void main() {\n"
        "    if (u_primitive_mode == %d && (gl_PrimitiveIDIn & 1) != 0) {\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    bool z_persp = (u_uber_geom_flags & 4u) != 0u;\n"
        "    g_smod = (u_uber_geom_flags & 1u) != 0u;\n"
        "    int prov = 3;\n"
        "\n"
        "    int a, b, c, d;\n"
        "    if (u_primitive_mode == %d) {\n"
        "        a = 2; b = 0; c = 1; d = 3;\n"
        "    } else {\n"
        "        a = 0; b = 1; c = 2; d = 3;\n"
        "    }\n"
        "\n"
        "    vec4 sp[4];\n"
        "    for (int i = 0; i < 4; i++) sp[i] = v_vtxPos0[i];\n"
        "\n"
        "    float tmz1 = calc_triz_dz(sp[a], sp[b], sp[c], z_persp).x;\n"
        "    tmz1 = (isnan(tmz1) || isinf(tmz1)) ? 0.0 : tmz1;\n"
        "    emit_vtx(b, prov, sp[a], sp[b], sp[c], tmz1);\n"
        "    emit_vtx(c, prov, sp[a], sp[b], sp[c], tmz1);\n"
        "    emit_vtx(a, prov, sp[a], sp[b], sp[c], tmz1);\n"
        "    EndPrimitive();\n"
        "\n"
        "    float tmz2 = calc_triz_dz(sp[a], sp[c], sp[d], z_persp).x;\n"
        "    tmz2 = (isnan(tmz2) || isinf(tmz2)) ? 0.0 : tmz2;\n"
        "    emit_vtx(c, prov, sp[a], sp[c], sp[d], tmz2);\n"
        "    emit_vtx(d, prov, sp[a], sp[c], sp[d], tmz2);\n"
        "    emit_vtx(a, prov, sp[a], sp[c], sp[d], tmz2);\n"
        "    EndPrimitive();\n"
        "}\n",
        PRIM_TYPE_QUAD_STRIP, PRIM_TYPE_QUAD_STRIP
    );

    return s;
}

MString *pgraph_glsl_gen_uber_geom_quad_wireframe(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    mstring_append(s,
        "layout(lines_adjacency) in;\n"
        "layout(line_strip, max_vertices = 8) out;\n"
        "\n"
    );

    uber_vtx_header_vsh_gs(s, opts.vulkan, true, true);
    pgraph_glsl_get_vtx_header(s, opts.vulkan, true, false, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = %d, %s) uniform UberGeomUniforms {\n",
            opts.uber_geom_ubo_binding, uber_ubo_layout(opts));
        mstring_append(s,
            "int u_primitive_mode;\n"
            "int u_polygon_front_mode;\n"
            "int u_polygon_back_mode;\n"
            "uint u_uber_geom_flags;\n"
            "int u_tri_rot0;\n"
            "int u_tri_rot1;\n"
            "};\n");
    } else {
        mstring_append(s,
            "uniform int u_primitive_mode;\n"
            "uniform int u_polygon_front_mode;\n"
            "uniform int u_polygon_back_mode;\n"
            "uniform uint u_uber_geom_flags;\n"
            "uniform int u_tri_rot0;\n"
            "uniform int u_tri_rot1;\n");
    }

    mstring_append(s,
        "float kahan_det(float a, float b, float c, float d) {\n"
        "    precise float cd = c*d;\n"
        "    precise float err = fma(-c, d, cd);\n"
        "    precise float res = fma(a, b, -cd) + err;\n"
        "    return res;\n"
        "}\n"
        "\n"
        "vec4 calc_triz_dz(vec4 sp0, vec4 sp1, vec4 sp2, bool z_persp) {\n"
        "    mat2 m = mat2(sp1.xy - sp0.xy, sp2.xy - sp0.xy);\n"
        "    float det = kahan_det(m[0].x, m[1].y, m[1].x, m[0].y);\n"
        "    float dzx, dzy;\n"
        "    if (z_persp) {\n"
        "        precise vec2 b = vec2(sp0.w - sp1.w, sp0.w - sp2.w);\n"
        "        b /= vec2(sp1.w, sp2.w) * sp0.w;\n"
        "        dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "    } else {\n"
        "        precise vec2 b = vec2(sp1.z - sp0.z, sp2.z - sp0.z);\n"
        "        dzx = kahan_det(b.x, m[1].y, b.y, m[0].y) / det;\n"
        "        dzy = kahan_det(b.y, m[0].x, b.x, m[1].x) / det;\n"
        "    }\n"
        "    return vec4(max(abs(dzx), abs(dzy)), 0, 0, 0);\n"
        "}\n"
        "\n"
        /* dz pre-sanitized in main; smod once per prim. */
        "bool g_smod;\n"
        "void emit_line_pair(int i0, int i1, int prov, float dz) {\n"
        "    vec2 delta = v_vtxPos0[i1].xy - v_vtxPos0[i0].xy;\n"
        "    vec4 p0 = v_vtxPos0[i0];\n"
        "    vec4 p1 = v_vtxPos0[i1];\n"
        "    vec4 p2 = vec4(vec2(-delta.y, delta.x) + p0.xy, p0.zw);\n"
        "    float tmz = dz;\n"
        "\n"
        "    int ci0 = g_smod ? i0 : prov;\n"
        "    gl_Position = gl_in[i0].gl_Position;\n"
        "    gl_PointSize = gl_in[i0].gl_PointSize;\n"
        "    vtxD0 = v_vtxD0[ci0]; vtxD1 = v_vtxD1[ci0];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxB0 = v_vtxB0[ci0]; vtxB1 = v_vtxB1[ci0];\n"
        "    vtxFog = v_vtxFog[i0];\n"
        "    vtxT0 = v_vtxT0[i0]; vtxT1 = v_vtxT1[i0];\n"
        "    vtxT2 = v_vtxT2[i0]; vtxT3 = v_vtxT3[i0];\n"
        "    vtxPos0 = p0; vtxPos1 = p1; vtxPos2 = p2;\n"
        "    triMZ = tmz;\n"
        "    EmitVertex();\n"
        "\n"
        "    int ci1 = g_smod ? i1 : prov;\n"
        "    gl_Position = gl_in[i1].gl_Position;\n"
        "    gl_PointSize = gl_in[i1].gl_PointSize;\n"
        "    vtxD0 = v_vtxD0[ci1]; vtxD1 = v_vtxD1[ci1];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxB0 = v_vtxB0[ci1]; vtxB1 = v_vtxB1[ci1];\n"
        "    vtxFog = v_vtxFog[i1];\n"
        "    vtxT0 = v_vtxT0[i1]; vtxT1 = v_vtxT1[i1];\n"
        "    vtxT2 = v_vtxT2[i1]; vtxT3 = v_vtxT3[i1];\n"
        "    vtxPos0 = p0; vtxPos1 = p1; vtxPos2 = p2;\n"
        "    triMZ = tmz;\n"
        "    EmitVertex();\n"
        "    EndPrimitive();\n"
        "}\n"
        "\n"
    );

    mstring_append_fmt(s,
        "void main() {\n"
        "    if (u_primitive_mode == %d && (gl_PrimitiveIDIn & 1) != 0) {\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    bool z_persp = (u_uber_geom_flags & 4u) != 0u;\n"
        "    int prov = 3;\n"
        "\n"
        "    int a, b, c, d;\n"
        "    if (u_primitive_mode == %d) {\n"
        "        a = 2; b = 0; c = 1; d = 3;\n"
        "    } else {\n"
        "        a = 0; b = 1; c = 2; d = 3;\n"
        "    }\n"
        "\n"
        "    vec4 sp[4];\n"
        "    for (int i = 0; i < 4; i++) sp[i] = v_vtxPos0[i];\n"
        "\n"
        "    float tmz1 = calc_triz_dz(sp[a], sp[b], sp[c], z_persp).x;\n"
        "    float tmz2 = calc_triz_dz(sp[a], sp[c], sp[d], z_persp).x;\n"
        "    tmz1 = (isnan(tmz1) || isinf(tmz1)) ? 0.0 : tmz1;\n"
        "    tmz2 = (isnan(tmz2) || isinf(tmz2)) ? 0.0 : tmz2;\n"
        "    g_smod = (u_uber_geom_flags & 1u) != 0u;\n"
        "\n"
        "    if (u_primitive_mode == %d) {\n"
        "        emit_line_pair(b, c, prov, tmz1);\n"
        "        emit_line_pair(c, d, prov, tmz2);\n"
        "        emit_line_pair(d, a, prov, tmz2);\n"
        "        emit_line_pair(a, b, prov, tmz1);\n"
        "    } else {\n"
        "        emit_line_pair(a, b, prov, tmz1);\n"
        "        emit_line_pair(b, c, prov, tmz1);\n"
        "        emit_line_pair(c, d, prov, tmz2);\n"
        "        emit_line_pair(d, a, prov, tmz2);\n"
        "    }\n"
        "}\n",
        PRIM_TYPE_QUAD_STRIP, PRIM_TYPE_QUAD_STRIP, PRIM_TYPE_QUAD_STRIP
    );

    return s;
}

MString *pgraph_glsl_gen_uber_geom_line(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    mstring_append(s,
        "layout(lines) in;\n"
        "layout(line_strip, max_vertices = 2) out;\n"
        "\n"
    );

    uber_vtx_header_vsh_gs(s, opts.vulkan, true, true);
    pgraph_glsl_get_vtx_header(s, opts.vulkan, true, false, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    if (opts.vulkan) {
        mstring_append_fmt(s,
            "layout(binding = %d, %s) uniform UberGeomUniforms {\n",
            opts.uber_geom_ubo_binding, uber_ubo_layout(opts));
        mstring_append(s,
            "int u_primitive_mode;\n"
            "int u_polygon_front_mode;\n"
            "int u_polygon_back_mode;\n"
            "uint u_uber_geom_flags;\n"
            "int u_tri_rot0;\n"
            "int u_tri_rot1;\n"
            "};\n");
    } else {
        mstring_append(s,
            "uniform int u_primitive_mode;\n"
            "uniform int u_polygon_front_mode;\n"
            "uniform int u_polygon_back_mode;\n"
            "uniform uint u_uber_geom_flags;\n"
            "uniform int u_tri_rot0;\n"
            "uniform int u_tri_rot1;\n");
    }

    mstring_append_fmt(s,
        "void main() {\n"
        "    bool smod = (u_uber_geom_flags & 1u) != 0u;\n"
        "    bool first_prov = (u_uber_geom_flags & 2u) != 0u;\n"
        "    int prov = (u_primitive_mode == %d) ? 0 : (first_prov ? 0 : 1);\n",
        PRIM_TYPE_POLYGON);
    mstring_append(s,
        "\n"
        "    vec2 delta = v_vtxPos0[1].xy - v_vtxPos0[0].xy;\n"
        "    vec2 v2 = vec2(-delta.y, delta.x) + v_vtxPos0[0].xy;\n"
        "    vec4 vp0 = v_vtxPos0[0];\n"
        "    vec4 vp1 = v_vtxPos0[1];\n"
        "    vec4 vp2 = vec4(v2, v_vtxPos0[0].zw);\n"
        "\n"
        "    int c0 = smod ? 0 : prov;\n"
        "    int c1 = smod ? 1 : prov;\n"
        "\n"
        "    gl_Position = gl_in[0].gl_Position;\n"
        "    vtxD0 = v_vtxD0[c0];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxD1 = v_vtxD1[c0];\n"
        "    vtxB0 = v_vtxB0[c0];\n"
        "    vtxB1 = v_vtxB1[c0];\n"
        "    vtxFog = v_vtxFog[0];\n"
        "    vtxT0 = v_vtxT0[0]; vtxT1 = v_vtxT1[0];\n"
        "    vtxT2 = v_vtxT2[0]; vtxT3 = v_vtxT3[0];\n"
        "    vtxPos0 = vp0; vtxPos1 = vp1; vtxPos2 = vp2;\n"
        "    triMZ = 0.0;\n"
        "    EmitVertex();\n"
        "\n"
        "    gl_Position = gl_in[1].gl_Position;\n"
        "    vtxD0 = v_vtxD0[c1];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxD1 = v_vtxD1[c1];\n"
        "    vtxB0 = v_vtxB0[c1];\n"
        "    vtxB1 = v_vtxB1[c1];\n"
        "    vtxFog = v_vtxFog[1];\n"
        "    vtxT0 = v_vtxT0[1]; vtxT1 = v_vtxT1[1];\n"
        "    vtxT2 = v_vtxT2[1]; vtxT3 = v_vtxT3[1];\n"
        "    vtxPos0 = vp0; vtxPos1 = vp1; vtxPos2 = vp2;\n"
        "    triMZ = 0.0;\n"
        "    EmitVertex();\n"
        "    EndPrimitive();\n"
        "}\n"
    );

    return s;
}

MString *pgraph_glsl_gen_uber_geom_points(GenUberGlslOptions opts)
{
    MString *s = mstring_new();
    uber_glsl_prologue(s, opts);

    mstring_append(s,
        "layout(points) in;\n"
        "layout(points, max_vertices = 1) out;\n"
        "\n"
    );

    uber_vtx_header_vsh_gs(s, opts.vulkan, true, true);
    pgraph_glsl_get_vtx_header(s, opts.vulkan, true, false, false, false);
    if (opts.vk_batched) {
        mstring_append(s,
            "layout(location = 10) flat in uint v_uberRec[];\n"
            "layout(location = 13) flat out uint g_uberRec;\n");
    }

    mstring_append(s,
        "void main() {\n"
        "    gl_Position = gl_in[0].gl_Position;\n"
        "    gl_PointSize = gl_in[0].gl_PointSize;\n"
        "    vtxD0 = v_vtxD0[0];\n");
    if (opts.vk_batched) {
        mstring_append(s, "    g_uberRec = v_uberRec[0];\n");
    }
    mstring_append(s,
        "    vtxD1 = v_vtxD1[0];\n"
        "    vtxB0 = v_vtxB0[0];\n"
        "    vtxB1 = v_vtxB1[0];\n"
        "    vtxFog = v_vtxFog[0];\n"
        "    vtxT0 = v_vtxT0[0]; vtxT1 = v_vtxT1[0];\n"
        "    vtxT2 = v_vtxT2[0]; vtxT3 = v_vtxT3[0];\n"
        "    vtxPos0 = v_vtxPos0[0];\n"
        "    vtxPos1 = v_vtxPos0[0];\n"
        "    vtxPos2 = v_vtxPos0[0];\n"
        "    triMZ = 0.0;\n"
        "    EmitVertex();\n"
        "    EndPrimitive();\n"
        "}\n"
    );

    return s;
}

/* ========================================================================
 * Uniform Fill Functions
 * ======================================================================== */

void pgraph_uber_set_psh_uniforms(PGRAPHState *pg, UberPshUniforms *u)
{
    memset(u, 0, sizeof(*u));

    /* draw_id field removed entirely (nothing sets it any more;
     * the GLSL never read it). */
    u->combiner_control = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL);
    u->shader_stage_program = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
    u->other_stage_input = pgraph_reg_r(pg, NV_PGRAPH_SHADERCTL);
    u->final_inputs_0 = pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG0);
    u->final_inputs_1 = pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG1);

    int num_stages = u->combiner_control & 0xFF;
    for (int i = 0; i < num_stages; i++) {
        u->rgb_inputs[i] = pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4);
        u->rgb_outputs[i] = pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4);
        u->alpha_inputs[i] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4);
        u->alpha_outputs[i] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4);
    }

    u->uber_flags = 0;
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE) {
        u->uber_flags |= UBER_FLAG_ALPHA_TEST;
    }
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE)
        u->uber_flags |= UBER_FLAG_WINDOW_CLIP_EXCL;
    if (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3), NV_PGRAPH_CONTROL_3_SHADEMODE) ==
        NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH)
        u->uber_flags |= UBER_FLAG_SMOOTH_SHADING;
    if (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ZCOMPRESSOCCLUDE),
                 NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN) ==
        NV_PGRAPH_ZCOMPRESSOCCLUDE_ZCLAMP_EN_CULL)
        u->uber_flags |= UBER_FLAG_DEPTH_CLIPPING;
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE)
        u->uber_flags |= UBER_FLAG_Z_PERSPECTIVE;
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_POINTSMOOTHENABLE)
        u->uber_flags |= UBER_FLAG_POINT_SPRITE;

    u->alpha_func = (int32_t)GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                                       NV_PGRAPH_CONTROL_0_ALPHAFUNC);

    u->shadow_depth_func = (int32_t)GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SHADOWCTL),
                                              NV_PGRAPH_SHADOWCTL_SHADOW_ZFUNC);

    unsigned int z_format = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                     NV_PGRAPH_SETUPRASTER_Z_FORMAT);
    switch (pg->surface_shape.zeta_format) {
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        u->depth_format = z_format ? 3 : 0;
        break;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
        u->depth_format = z_format ? 2 : 1;
        break;
    default:
        u->depth_format = 0;
        break;
    }

    u->tex_modes = u->shader_stage_program;

    for (int i = 0; i < 4; i++) {
        uint32_t ctl_0 = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
        bool enabled = pgraph_is_texture_stage_active(pg, i) &&
                       (ctl_0 & NV_PGRAPH_TEXCTL0_0_ENABLE);

        u->tex_flags[i] = 0;
        int dim = 0; /* setter-local; the matching GLSL uniform no longer exists */
        (void)dim;
        u->colorkey_mode[i] = 0;
        u->conv_tex[i] = 0;
        /* backend-agnostic zero; the VK bind path overwrites
         * these with the bound 2D slot's dims right after this setter. */
        u->tex_size[i][0] = 0;
        u->tex_size[i][1] = 0;
        u->border_logical_size[i][0] = 0.0f;
        u->border_logical_size[i][1] = 0.0f;
        u->border_logical_size[i][2] = 0.0f;
        u->border_inv_real_size[i][0] = 0.0f;
        u->border_inv_real_size[i][1] = 0.0f;
        u->border_inv_real_size[i][2] = 0.0f;

        if (!enabled) continue;

        uint32_t tex_fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
        dim = GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY);

        unsigned int color_format = GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_COLOR);
        BasicColorFormatInfo f = kelvin_color_format_info_map[color_format];

        if (f.linear) u->tex_flags[i] |= UBER_TEX_FLAG_RECT;
        if (ctl_0 & NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN)
            u->tex_flags[i] |= UBER_TEX_FLAG_ALPHAKILL;
        if (color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED ||
            color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT)
            u->tex_flags[i] |= UBER_TEX_FLAG_X8Y24;
        if (GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE))
            u->tex_flags[i] |= UBER_TEX_FLAG_CUBEMAP;
        if (f.depth)
            u->tex_flags[i] |= UBER_TEX_FLAG_SHADOW;
        if (dim == 3)
            u->tex_flags[i] |= UBER_TEX_FLAG_3D;

        u->colorkey_mode[i] = ctl_0 & NV_PGRAPH_TEXCTL0_0_COLORKEYMODE;

        uint32_t border_source = GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_BORDER_SOURCE);
        if (border_source != NV_PGRAPH_TEXFMT0_BORDER_SOURCE_COLOR &&
            !f.linear && !GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_CUBEMAPENABLE)) {
            unsigned int rw = 1 << GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_U);
            unsigned int rh = 1 << GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
            unsigned int rd = 1 << GET_MASK(tex_fmt, NV_PGRAPH_TEXFMT0_BASE_SIZE_P);
            u->border_logical_size[i][0] = rw;
            u->border_logical_size[i][1] = rh;
            u->border_logical_size[i][2] = rd;
            u->border_inv_real_size[i][0] = rw < 8 ? 0.0625f : 1.0f / (rw * 2.0f);
            u->border_inv_real_size[i][1] = rh < 8 ? 0.0625f : 1.0f / (rh * 2.0f);
            u->border_inv_real_size[i][2] = rd < 8 ? 0.0625f : 1.0f / (rd * 2.0f);
        }

        uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4);
        unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
        if (min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0) {
            u->conv_tex[i] = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_CONVOLUTION_KERNEL);
        }
    }

    u->compare_mode_packed = pgraph_reg_r(pg, NV_PGRAPH_SHADERCLIPMODE);

    u->dot_map[0] = 0;
    u->dot_map[1] = (u->other_stage_input >> 0) & 0xF;
    u->dot_map[2] = (u->other_stage_input >> 4) & 0xF;
    u->dot_map[3] = (u->other_stage_input >> 8) & 0xF;

    u->input_tex[0] = -1;
    u->input_tex[1] = 0;
    u->input_tex[2] = (u->other_stage_input >> 16) & 0xF;
    u->input_tex[3] = (u->other_stage_input >> 20) & 0xF;

}

void pgraph_uber_set_vsh_uniforms(PGRAPHState *pg, const VshState *vsh,
                                  UberVshUniforms *u)
{
    /* full memset kept — GL uploads the whole array every
     * programmable-or-FF draw; skipping or bounding the GL upload had a
     * measurable driver-side penalty when the per-draw uniform pattern
     * changes. The bounded-staging win lives on the VK side only (staging is
     * bounded by program_length; see upload_uber_vsh_uniforms_vk). */
    memset(u, 0, sizeof(*u));

    u->uber_vsh_flags = 0;
    if (vsh->is_fixed_function) u->uber_vsh_flags |= UBER_VSH_FLAG_IS_FF;
    if (vsh->fog_enable) u->uber_vsh_flags |= UBER_VSH_FLAG_FOG_ENABLE;
    if (vsh->specular_enable) u->uber_vsh_flags |= UBER_VSH_FLAG_SPEC_ENABLE;
    if (vsh->separate_specular) u->uber_vsh_flags |= UBER_VSH_FLAG_SEP_SPEC;
    if (vsh->ignore_specular_alpha) u->uber_vsh_flags |= UBER_VSH_FLAG_IGN_SPEC_A;
    if (vsh->point_params_enable) u->uber_vsh_flags |= UBER_VSH_FLAG_PT_PARAMS;
    if (vsh->smooth_shading) u->uber_vsh_flags |= UBER_VSH_FLAG_SMOOTH;
    if (vsh->z_perspective) u->uber_vsh_flags |= UBER_VSH_FLAG_Z_PERSP;

    u->fog_mode = vsh->fog_enable ? (int32_t)vsh->fog_mode : 0;

    if (vsh->is_fixed_function) {
        if (vsh->fixed_function.normalization)
            u->uber_vsh_flags |= UBER_VSH_FLAG_NORMALIZE;
        if (vsh->fixed_function.lighting)
            u->uber_vsh_flags |= UBER_VSH_FLAG_LIGHTING;
        if (vsh->fixed_function.local_eye)
            u->uber_vsh_flags |= UBER_VSH_FLAG_LOCAL_EYE;
        if (vsh->fixed_function.texture_matrix_enable[0])
            u->uber_vsh_flags |= UBER_VSH_FLAG_TEXMAT0;
        if (vsh->fixed_function.texture_matrix_enable[1])
            u->uber_vsh_flags |= UBER_VSH_FLAG_TEXMAT1;
        if (vsh->fixed_function.texture_matrix_enable[2])
            u->uber_vsh_flags |= UBER_VSH_FLAG_TEXMAT2;
        if (vsh->fixed_function.texture_matrix_enable[3])
            u->uber_vsh_flags |= UBER_VSH_FLAG_TEXMAT3;

        u->skinning_mode = (int32_t)vsh->fixed_function.skinning;
        u->foggen_mode = (int32_t)vsh->fixed_function.foggen;

        for (int i = 0; i < NV2A_MAX_LIGHTS; i++) {
            u->light_enable_packed |= ((uint32_t)vsh->fixed_function.light[i] & 0x3) << (i * 2);
        }

        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                u->texgen_packed[i] |= ((uint32_t)vsh->fixed_function.texgen[i][j] & 0x7) << (j * 3);
            }
        }

        u->mat_color_src_packed =
            ((uint32_t)vsh->fixed_function.emission_src) |
            ((uint32_t)vsh->fixed_function.ambient_src << 2) |
            ((uint32_t)vsh->fixed_function.diffuse_src << 4) |
            ((uint32_t)vsh->fixed_function.specular_src << 6);
    } else {
        u->program_length = vsh->programmable.program_length;
        /* pack 3 of 4 instruction words (w0 is never decoded
         * by the interpreter — it reads w1..w3 only). */
        for (int i = 0; i < u->program_length; i++) {
            uint32_t w1 = vsh->programmable.program_data[i][1];
            /* pre-resolve the c[] index BASE at upload time
             * (this path runs only when the program changes). The 8-bit
             * c_field at w1[20:13] is replaced by the UNCLAMPED base
             * ((hi3-3)*32 + lo5 + 96, range 0..255 — fits the same 8 bits),
             * so the per-vertex GLSL keeps only the A0 add + clamp. The
             * clamp must stay post-A0 (clamping the base first would change
             * results for out-of-range bases with negative A0). Same array,
             * same copy shape — safe. */
            uint32_t cf = (w1 >> 13) & 0xFF;
            int cibase =
                (((int)((cf >> 5) & 7) - 3) * 32) + (int)(cf & 31) + 96;
            w1 = (w1 & ~(0xFFu << 13)) | (((uint32_t)cibase & 0xFFu) << 13);
            u->program_data[i * 3 + 0] = w1;
            u->program_data[i * 3 + 1] = vsh->programmable.program_data[i][2];
            u->program_data[i * 3 + 2] = vsh->programmable.program_data[i][3];
        }
    }

    u->uniform_attrs = pg->uniform_attrs;
    u->compressed_attrs = pg->compressed_attrs;
    u->swizzle_attrs = pg->swizzle_attrs;
    u->point_size = vsh->point_size <= 0.f ? 1.f : vsh->point_size;
    memcpy(u->point_params, vsh->point_params, sizeof(u->point_params));
    u->surface_scale_factor = vsh->surface_scale_factor;

}

void pgraph_uber_set_geom_uniforms(PGRAPHState *pg, const GeomState *geom,
                                   UberGeomUniforms *u)
{
    memset(u, 0, sizeof(*u));
    u->primitive_mode = (int32_t)geom->primitive_mode;
    u->polygon_front_mode = (int32_t)geom->polygon_front_mode;
    u->polygon_back_mode = (int32_t)geom->polygon_back_mode;
    u->uber_geom_flags = 0;
    if (geom->smooth_shading) u->uber_geom_flags |= UBER_GEOM_FLAG_SMOOTH;
    if (geom->first_vertex_is_provoking) u->uber_geom_flags |= UBER_GEOM_FLAG_FIRST_PROVOKING;
    if (geom->z_perspective) u->uber_geom_flags |= UBER_GEOM_FLAG_Z_PERSP;
    u->tri_rot0 = geom->tri_rot0;
    u->tri_rot1 = geom->tri_rot1;
}
