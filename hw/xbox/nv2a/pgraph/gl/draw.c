/*
 * Geforce NV2A PGRAPH OpenGL Renderer
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

#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "debug.h"
#include "renderer.h"


void pgraph_gl_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    pgraph_gl_uber_submit_md(r);

    NV2A_DPRINTF("---------PRE CLEAR ------\n");
    pg->clearing = true;

    GLbitfield gl_mask = 0;

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    if (write_zeta) {
        GLint gl_clear_stencil;
        GLfloat gl_clear_depth;
        pgraph_get_clear_depth_stencil_value(pg, &gl_clear_depth,
                                             &gl_clear_stencil);

        if (parameter & NV097_CLEAR_SURFACE_Z) {
            gl_mask |= GL_DEPTH_BUFFER_BIT;
            glDepthMask(GL_TRUE);
            glClearDepth(gl_clear_depth);
        }
        if (parameter & NV097_CLEAR_SURFACE_STENCIL) {
            gl_mask |= GL_STENCIL_BUFFER_BIT;
            glStencilMask(0xff);
            glClearStencil(gl_clear_stencil);
        }
    }
    if (write_color) {
        gl_mask |= GL_COLOR_BUFFER_BIT;
        glColorMask((parameter & NV097_CLEAR_SURFACE_R)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_G)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_B)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_A)
                         ? GL_TRUE : GL_FALSE);

        GLfloat rgba[4];
        pgraph_get_clear_color(pg, rgba);
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    pgraph_gl_surface_update(d, true, write_color, write_zeta);

    /* FIXME: Needs confirmation */
    unsigned int xmin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_DPRINTF(
        "------------------CLEAR 0x%x %d,%d - %d,%d  %x---------------\n",
        parameter, xmin, ymin, xmax, ymax,
        d->pgraph.regs_[NV_PGRAPH_COLORCLEARVALUE]);

    unsigned int scissor_width = xmax - xmin + 1,
                 scissor_height = ymax - ymin + 1;
    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    NV2A_DPRINTF("Translated clear rect to %d,%d - %d,%d\n", xmin, ymin,
                 xmin + scissor_width - 1, ymin + scissor_height - 1);

    bool full_clear = !xmin && !ymin &&
                      scissor_width >= pg->surface_binding_dim.width &&
                      scissor_height >= pg->surface_binding_dim.height;

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    /* FIXME: Respect window clip?!?! */
    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

    /* Dither */
    /* FIXME: Maybe also disable it here? + GL implementation dependent */
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & NV_PGRAPH_CONTROL_0_DITHERENABLE) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glClear(gl_mask);

    glDisable(GL_SCISSOR_TEST);

    /* the clear wrote masks/scissor raw — the per-value
     * begin-state shadow no longer self-heals via full re-emits, so foreign
     * writers must invalidate it explicitly. (Also closes a latent memcmp-hit
     * hole: a clobber followed by an unchanged-state draw could ride stale
     * masks.) */
    r->uber_begin_state_valid = false;

    pgraph_gl_set_surface_dirty(pg, write_color, write_zeta);

    if (r->color_binding) {
        r->color_binding->cleared = full_clear && write_color;
    }
    if (r->zeta_binding) {
        r->zeta_binding->cleared = full_clear && write_zeta;
    }
    
    pg->clearing = false;
}

void pgraph_gl_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_GL_DGROUP_BEGIN("NV097_SET_BEGIN_END: 0x%x", pg->primitive_mode);

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

    pgraph_gl_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        return;
    }

    assert(r->color_binding || r->zeta_binding);

    pgraph_gl_bind_textures(d);

    pgraph_gl_bind_shaders(pg);

    /* + the raw state-call block, value-shadowed.
     * All values are computed register-side into a struct; one memcmp
     * gates the ~25 calls. A struct change is the begin path's "first
     * real GL mutation" -> submit any deferred multidraw first (the GL
     * state at that instant still equals the recorded state). Spec path
     * unchanged below. */
    if (r->uber_active) {
        struct UberGlBeginState bs;
        memset(&bs, 0, sizeof(bs));
        bs.cmask_r = mask_red ? GL_TRUE : GL_FALSE;
        bs.cmask_g = mask_green ? GL_TRUE : GL_FALSE;
        bs.cmask_b = mask_blue ? GL_TRUE : GL_FALSE;
        bs.cmask_a = mask_alpha ? GL_TRUE : GL_FALSE;
        bs.depth_mask =
            (control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE) ? GL_TRUE : GL_FALSE;
        bs.stencil_mask = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                   NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        bs.blend_en = pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN;
        if (bs.blend_en) {
            uint32_t sfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                        NV_PGRAPH_BLEND_SFACTOR);
            uint32_t dfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                        NV_PGRAPH_BLEND_DFACTOR);
            assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
            assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
            bs.blend_sfactor = pgraph_blend_factor_gl_map[sfactor];
            bs.blend_dfactor = pgraph_blend_factor_gl_map[dfactor];
            uint32_t equation = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                         NV_PGRAPH_BLEND_EQN);
            assert(equation < ARRAY_SIZE(pgraph_blend_equation_gl_map));
            bs.blend_equation = pgraph_blend_equation_gl_map[equation];
            uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
            pgraph_argb_pack32_to_rgba_float(blend_color, bs.blend_color);
        } else if (r->uber_begin_state_valid) {
            /* carry the last-emitted params while disabled so
             * the per-value shadow mirrors the retained GL context — zeroed
             * params alias legitimate zero values (GL_ZERO factors, a
             * 0,0,0,0 blend color) and would skip a required re-emit on
             * re-enable. Also removes spurious outer-memcmp misses on
             * enable->disable transitions. */
            bs.blend_sfactor = r->uber_begin_state.blend_sfactor;
            bs.blend_dfactor = r->uber_begin_state.blend_dfactor;
            bs.blend_equation = r->uber_begin_state.blend_equation;
            memcpy(bs.blend_color, r->uber_begin_state.blend_color,
                   sizeof(bs.blend_color));
        }
        bool uber_line_mode =
            r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE;
        bs.cull_en = !uber_line_mode &&
                     (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                      NV_PGRAPH_SETUPRASTER_CULLENABLE);
        if (bs.cull_en) {
            uint32_t cull_face =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                         NV_PGRAPH_SETUPRASTER_CULLCTRL);
            assert(cull_face < ARRAY_SIZE(pgraph_cull_face_gl_map));
            bs.cull_face = pgraph_cull_face_gl_map[cull_face];
        }
        bs.front_face = (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                         NV_PGRAPH_SETUPRASTER_FRONTFACE) ? GL_CW : GL_CCW;
        bs.depth_test_en = depth_test;
        if (depth_test) {
            uint32_t depth_func =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                         NV_PGRAPH_CONTROL_0_ZFUNC);
            assert(depth_func < ARRAY_SIZE(pgraph_depth_func_gl_map));
            bs.depth_func = pgraph_depth_func_gl_map[depth_func];
        }
        bs.stencil_en = stencil_test;
        if (stencil_test) {
            uint32_t stencil_func =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                         NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
            assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_gl_map));
            bs.stencil_func = pgraph_stencil_func_gl_map[stencil_func];
            bs.stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                      NV_PGRAPH_CONTROL_1_STENCIL_REF);
            bs.stencil_func_mask =
                GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                         NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                        NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                         NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);
            assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
            assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
            assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_gl_map));
            bs.stencil_op_fail = pgraph_stencil_op_gl_map[op_fail];
            bs.stencil_op_zfail = pgraph_stencil_op_gl_map[op_zfail];
            bs.stencil_op_zpass = pgraph_stencil_op_gl_map[op_zpass];
        }
        bs.dither_en = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
                       NV_PGRAPH_CONTROL_0_DITHERENABLE;
        bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
                                      NV_PGRAPH_ANTIALIASING_ENABLE);
        bs.line_smooth_en =
            !anti_aliasing && (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                               NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE);
        bs.line_width = bs.line_smooth_en ?
            MIN(r->supported_smooth_line_width_range[1],
                pg->surface_scale_factor) :
            MIN(r->supported_aliased_line_width_range[1],
                pg->surface_scale_factor);
        bs.poly_smooth_en =
            !anti_aliasing && (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                               NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE);
        {
            enum ShaderPolygonMode pmode =
                r->shader_binding->state.geom.polygon_front_mode;
            bs.polygon_mode = (pmode == POLY_MODE_LINE)  ? GL_LINE :
                              (pmode == POLY_MODE_POINT) ? GL_POINT : GL_FILL;
        }
        {
            unsigned int vp_width = pg->surface_binding_dim.width,
                         vp_height = pg->surface_binding_dim.height;
            pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
            bs.vp_w = vp_width;
            bs.vp_h = vp_height;
        }
        {
            unsigned int xmin = pg->surface_shape.clip_x,
                         ymin = pg->surface_shape.clip_y;
            unsigned int scissor_width = pg->surface_shape.clip_width,
                         scissor_height = pg->surface_shape.clip_height;
            pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
            pgraph_apply_anti_aliasing_factor(pg, &scissor_width,
                                              &scissor_height);
            pgraph_apply_scaling_factor(pg, &xmin, &ymin);
            pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);
            bs.sc_x = xmin;
            bs.sc_y = ymin;
            bs.sc_w = scissor_width;
            bs.sc_h = scissor_height;
        }

        if (!r->uber_begin_state_valid ||
            memcmp(&bs, &r->uber_begin_state, sizeof(bs)) != 0) {
            pgraph_gl_uber_submit_md(r);
            /* within a dirty struct, emit only the groups
             * whose values moved — a one-field change re-emitted all ~25
             * calls (and each GL state change re-triggers driver draw-time
             * validation). */
            bool force_all = !r->uber_begin_state_valid;
            const struct UberGlBeginState *pv = &r->uber_begin_state;
#define STATE_CH(fld) (force_all || pv->fld != bs.fld)
            if (force_all) {
                glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
                glDisable(GL_POLYGON_OFFSET_FILL);
                glDisable(GL_POLYGON_OFFSET_LINE);
                glDisable(GL_POLYGON_OFFSET_POINT);
                glEnable(GL_DEPTH_CLAMP);
                glEnable(GL_PROGRAM_POINT_SIZE);
                glEnable(GL_SCISSOR_TEST);
            }
            if (STATE_CH(cmask_r) || STATE_CH(cmask_g) || STATE_CH(cmask_b) ||
                STATE_CH(cmask_a)) {
                glColorMask(bs.cmask_r, bs.cmask_g, bs.cmask_b, bs.cmask_a);
            }
            if (STATE_CH(depth_mask)) {
                glDepthMask(bs.depth_mask);
            }
            if (STATE_CH(stencil_mask)) {
                glStencilMask(bs.stencil_mask);
            }
            if (bs.blend_en) {
                if (STATE_CH(blend_en)) {
                    glEnable(GL_BLEND);
                }
                /* Disabled-state zeroes the param fields (memset), so a
                 * re-enable with equal params still compares unequal vs the
                 * zeroed previous — params re-emit exactly when needed. */
                if (STATE_CH(blend_sfactor) || STATE_CH(blend_dfactor)) {
                    glBlendFunc(bs.blend_sfactor, bs.blend_dfactor);
                }
                if (STATE_CH(blend_equation)) {
                    glBlendEquation(bs.blend_equation);
                }
                if (STATE_CH(blend_color[0]) || STATE_CH(blend_color[1]) ||
                    STATE_CH(blend_color[2]) || STATE_CH(blend_color[3])) {
                    glBlendColor(bs.blend_color[0], bs.blend_color[1],
                                 bs.blend_color[2], bs.blend_color[3]);
                }
            } else if (STATE_CH(blend_en)) {
                glDisable(GL_BLEND);
            }
            if (bs.cull_en) {
                if (STATE_CH(cull_face)) {
                    glCullFace(bs.cull_face);
                }
                if (STATE_CH(cull_en)) {
                    glEnable(GL_CULL_FACE);
                }
            } else if (STATE_CH(cull_en)) {
                glDisable(GL_CULL_FACE);
            }
            if (STATE_CH(front_face)) {
                glFrontFace(bs.front_face);
            }
            if (bs.depth_test_en) {
                if (STATE_CH(depth_test_en)) {
                    glEnable(GL_DEPTH_TEST);
                }
                if (STATE_CH(depth_func)) {
                    glDepthFunc(bs.depth_func);
                }
            } else if (STATE_CH(depth_test_en)) {
                glDisable(GL_DEPTH_TEST);
            }
            if (bs.stencil_en) {
                if (STATE_CH(stencil_en)) {
                    glEnable(GL_STENCIL_TEST);
                }
                if (STATE_CH(stencil_func) || STATE_CH(stencil_ref) ||
                    STATE_CH(stencil_func_mask)) {
                    glStencilFunc(bs.stencil_func, bs.stencil_ref,
                                  bs.stencil_func_mask);
                }
                if (STATE_CH(stencil_op_fail) || STATE_CH(stencil_op_zfail) ||
                    STATE_CH(stencil_op_zpass)) {
                    glStencilOp(bs.stencil_op_fail, bs.stencil_op_zfail,
                                bs.stencil_op_zpass);
                }
            } else if (STATE_CH(stencil_en)) {
                glDisable(GL_STENCIL_TEST);
            }
            if (STATE_CH(dither_en)) {
                if (bs.dither_en) {
                    glEnable(GL_DITHER);
                } else {
                    glDisable(GL_DITHER);
                }
            }
            if (STATE_CH(line_smooth_en)) {
                if (bs.line_smooth_en) {
                    glEnable(GL_LINE_SMOOTH);
                } else {
                    glDisable(GL_LINE_SMOOTH);
                }
            }
            if (STATE_CH(line_width)) {
                glLineWidth(bs.line_width);
            }
            if (STATE_CH(poly_smooth_en)) {
                if (bs.poly_smooth_en) {
                    glEnable(GL_POLYGON_SMOOTH);
                } else {
                    glDisable(GL_POLYGON_SMOOTH);
                }
            }
            if (STATE_CH(polygon_mode)) {
                glPolygonMode(GL_FRONT_AND_BACK, bs.polygon_mode);
            }
            if (STATE_CH(vp_w) || STATE_CH(vp_h)) {
                glViewport(0, 0, bs.vp_w, bs.vp_h);
            }
            if (STATE_CH(sc_x) || STATE_CH(sc_y) || STATE_CH(sc_w) ||
                STATE_CH(sc_h)) {
                glScissor(bs.sc_x, bs.sc_y, bs.sc_w, bs.sc_h);
            }
#undef STATE_CH
            r->uber_begin_state = bs;
            r->uber_begin_state_valid = true;
        }
    } else {
        glColorMask(mask_red, mask_green, mask_blue, mask_alpha);
        glDepthMask(!!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE));
        glStencilMask(GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                               NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE));

        if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
            glEnable(GL_BLEND);
            uint32_t sfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                        NV_PGRAPH_BLEND_SFACTOR);
            uint32_t dfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                        NV_PGRAPH_BLEND_DFACTOR);
            assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
            assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
            glBlendFunc(pgraph_blend_factor_gl_map[sfactor],
                        pgraph_blend_factor_gl_map[dfactor]);

            uint32_t equation = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                         NV_PGRAPH_BLEND_EQN);
            assert(equation < ARRAY_SIZE(pgraph_blend_equation_gl_map));
            glBlendEquation(pgraph_blend_equation_gl_map[equation]);

            uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
            float gl_blend_color[4];
            pgraph_argb_pack32_to_rgba_float(blend_color, gl_blend_color);
            glBlendColor(gl_blend_color[0], gl_blend_color[1], gl_blend_color[2],
                         gl_blend_color[3]);
        } else {
            glDisable(GL_BLEND);
        }

        /* Face culling */
        bool uber_line_mode = r->uber_active &&
            r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE;
        if (!uber_line_mode && (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
                & NV_PGRAPH_SETUPRASTER_CULLENABLE)) {
            uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                          NV_PGRAPH_SETUPRASTER_CULLCTRL);
            assert(cull_face < ARRAY_SIZE(pgraph_cull_face_gl_map));
            glCullFace(pgraph_cull_face_gl_map[cull_face]);
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }

        /* Front-face select */
        /* Winding is reverse here because clip-space y-coordinates are inverted */
        glFrontFace(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
                        & NV_PGRAPH_SETUPRASTER_FRONTFACE
                            ? GL_CW : GL_CCW);

        /* Polygon offset is handled in geometry and fragment shaders explicitly */
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_POLYGON_OFFSET_LINE);
        glDisable(GL_POLYGON_OFFSET_POINT);

        /* Depth testing */
        if (depth_test) {
            glEnable(GL_DEPTH_TEST);

            uint32_t depth_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                                           NV_PGRAPH_CONTROL_0_ZFUNC);
            assert(depth_func < ARRAY_SIZE(pgraph_depth_func_gl_map));
            glDepthFunc(pgraph_depth_func_gl_map[depth_func]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        glEnable(GL_DEPTH_CLAMP);

        /* Set first vertex convention to match Vulkan default */
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);

        if (stencil_test) {
            glEnable(GL_STENCIL_TEST);

            uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
            uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);
            uint32_t func_mask = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
            uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
            uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
            uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

            assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_gl_map));
            assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
            assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
            assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_gl_map));

            glStencilFunc(
                pgraph_stencil_func_gl_map[stencil_func],
                stencil_ref,
                func_mask);

            glStencilOp(
                pgraph_stencil_op_gl_map[op_fail],
                pgraph_stencil_op_gl_map[op_zfail],
                pgraph_stencil_op_gl_map[op_zpass]);

        } else {
            glDisable(GL_STENCIL_TEST);
        }

        /* Dither */
        /* FIXME: GL implementation dependent */
        if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
                NV_PGRAPH_CONTROL_0_DITHERENABLE) {
            glEnable(GL_DITHER);
        } else {
            glDisable(GL_DITHER);
        }

        glEnable(GL_PROGRAM_POINT_SIZE);

        bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING), NV_PGRAPH_ANTIALIASING_ENABLE);

        /* Edge Antialiasing */
        if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                                  NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
            glEnable(GL_LINE_SMOOTH);
            glLineWidth(MIN(r->supported_smooth_line_width_range[1], pg->surface_scale_factor));
        } else {
            glDisable(GL_LINE_SMOOTH);
            glLineWidth(MIN(r->supported_aliased_line_width_range[1], pg->surface_scale_factor));
        }
        if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                                  NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
            glEnable(GL_POLYGON_SMOOTH);
        } else {
            glDisable(GL_POLYGON_SMOOTH);
        }

        if (r->uber_active) {
            enum ShaderPolygonMode pmode = r->shader_binding->state.geom.polygon_front_mode;
            if (pmode == POLY_MODE_LINE) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            } else if (pmode == POLY_MODE_POINT) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
            } else {
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            }
        }

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
        glViewport(0, 0, vp_width, vp_height);

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
    }

    /* Visibility testing */
    if (pg->zpass_pixel_count_enable) {
        pgraph_gl_uber_submit_md(r); /* query brackets */
        r->gl_zpass_pixel_count_query_count++;
        r->gl_zpass_pixel_count_queries = (GLuint*)g_realloc(
            r->gl_zpass_pixel_count_queries,
            sizeof(GLuint) * r->gl_zpass_pixel_count_query_count);

        GLuint gl_query;
        glGenQueries(1, &gl_query);
        r->gl_zpass_pixel_count_queries[
            r->gl_zpass_pixel_count_query_count - 1] = gl_query;
        glBeginQuery(GL_SAMPLES_PASSED, gl_query);
    }
}

void pgraph_gl_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

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
        NV2A_GL_DGROUP_END();
        return;
    }

    pgraph_gl_flush_draw(d);

    /* End of visibility testing */
    if (pg->zpass_pixel_count_enable) {
        nv2a_profile_inc_counter(NV2A_PROF_QUERY);
        glEndQuery(GL_SAMPLES_PASSED);
    }

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_gl_set_surface_dirty(pg, color_write, depth_test || stencil_test);
    NV2A_GL_DGROUP_END();
}

void pgraph_gl_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return;
    }
    assert(r->shader_binding);

    if (pg->draw_arrays_length) {
        NV2A_GL_DPRINTF(false, "Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        pgraph_gl_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                      pg->draw_arrays_max_count - 1,
                                      false, 0,
                                      pg->draw_arrays_max_count - 1);
        if (r->uber_active) {
            /* defer — append this flush's ranges; the next
             * real GL mutation anywhere submits them under the still-equal
             * state. Mode change / capacity / an active query submits here.
             */
            if (r->uber_md_count &&
                (r->uber_md_mode != r->shader_binding->gl_primitive_mode ||
                 r->uber_md_count + pg->draw_arrays_length >
                     (int)ARRAY_SIZE(r->uber_md_start))) {
                pgraph_gl_uber_submit_md(r);
            }
            r->uber_md_mode = r->shader_binding->gl_primitive_mode;
            memcpy(&r->uber_md_start[r->uber_md_count], pg->draw_arrays_start,
                   pg->draw_arrays_length * sizeof(r->uber_md_start[0]));
            memcpy(&r->uber_md_counts[r->uber_md_count], pg->draw_arrays_count,
                   pg->draw_arrays_length * sizeof(r->uber_md_counts[0]));
            r->uber_md_count += pg->draw_arrays_length;
            if (pg->zpass_pixel_count_enable) {
                pgraph_gl_uber_submit_md(r);
            }
        } else {
            glMultiDrawArrays(r->shader_binding->gl_primitive_mode,
                              pg->draw_arrays_start,
                              pg->draw_arrays_count,
                              pg->draw_arrays_length);
        }
    } else if (pg->inline_elements_length) {
        pgraph_gl_uber_submit_md(r); /* order vs deferred */
        NV2A_GL_DPRINTF(false, "Inline Elements");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i=0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }

        pgraph_gl_bind_vertex_attributes(
                d, min_element, max_element, false, 0,
                pg->inline_elements[pg->inline_elements_length - 1]);

        VertexKey k;
        memset(&k, 0, sizeof(VertexKey));
        k.count = pg->inline_elements_length;
        k.gl_type = GL_UNSIGNED_INT;
        k.gl_normalize = GL_FALSE;
        k.stride = sizeof(uint32_t);
        uint64_t h = fast_hash((uint8_t*)pg->inline_elements,
                               pg->inline_elements_length * 4);

        LruNode *node = lru_lookup(&r->element_cache, h, &k);
        VertexLruNode *found = container_of(node, VertexLruNode, node);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, found->gl_buffer);
        if (!found->initialized) {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         pg->inline_elements_length * 4,
                         pg->inline_elements, GL_STATIC_DRAW);
            found->initialized = true;
        } else {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY);
        }
        glDrawElements(r->shader_binding->gl_primitive_mode,
                       pg->inline_elements_length, GL_UNSIGNED_INT,
                       (void *)0);
    } else if (pg->inline_buffer_length) {
        pgraph_gl_uber_submit_md(r); /* order vs deferred */
        NV2A_GL_DPRINTF(false, "Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        if (pg->compressed_attrs) {
            pg->compressed_attrs = 0;
            pgraph_gl_bind_shaders(pg);
        }

        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->inline_buffer_populated) {
                nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
                pgraph_gl_uber_bind_array_buffer(r, r->gl_inline_buffer[i]);
                glBufferData(GL_ARRAY_BUFFER,
                             pg->inline_buffer_length * sizeof(float) * 4,
                             attr->inline_buffer, GL_STREAM_DRAW);
                pgraph_gl_uber_attr_pointer(r, i, r->gl_inline_buffer[i],
                                            4, GL_FLOAT, GL_FALSE, false,
                                            0, 0);
                attr->inline_buffer_populated = false;
                memcpy(attr->inline_value,
                       attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                       sizeof(attr->inline_value));
            } else {
                pgraph_gl_uber_attr_disable_generic(
                    r, i, attr->inline_value);
            }
        }

        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, pg->inline_buffer_length);
    } else if (pg->inline_array_length) {
        pgraph_gl_uber_submit_md(r); /* order vs deferred */
        NV2A_GL_DPRINTF(false, "Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        unsigned int index_count = pgraph_gl_bind_inline_array(d);
        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, index_count);
    } else {
        NV2A_GL_DPRINTF(true, "EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }

    if (r->uber_active) {
        /* polygon mode is owned by the begin-state shadow now
         * (the per-flush FILL reset would both bust the shadow and mutate
         * raster state under a deferred multidraw). */
        /* removed per-draw glGetError drain+report (GPU sync per draw) */
        /* consume the dirty-reg map per draw (mirrors
         * vk/draw.c) so the bind-time dirty-skip can ever pass. GL has no
         * other pgraph_is_reg_dirty consumer (verified); spec path untouched
         * by the uber_active gate. */
        pgraph_clear_dirty_reg_map(pg);
    }

}
