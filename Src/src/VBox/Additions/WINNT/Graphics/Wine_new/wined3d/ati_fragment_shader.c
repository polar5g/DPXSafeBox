/*
 * Fixed function pipeline replacement using GL_ATI_fragment_shader
 *
 * Copyright 2008 Stefan Dösinger(for CodeWeavers)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * Oracle LGPL Disclaimer: For the avoidance of doubt, except that if any license choice
 * other than GPL or LGPL is available it will apply instead, Oracle elects to use only
 * the Lesser General Public License version 2.1 (LGPLv2) at this time for any software where
 * a choice of LGPL license versions is made available with the language indicating
 * that LGPLv2 or any later version may be used, or where a choice of which version
 * of the LGPL is applied is otherwise unspecified.
 */

#include "config.h"
#include "wine/port.h"

#include <math.h>
#include <stdio.h>

#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d_shader);
WINE_DECLARE_DEBUG_CHANNEL(d3d);

/* Context activation for state handlers is done by the caller. */

/* Some private defines, Constant associations, etc.
 * Env bump matrix and per stage constant should be independent,
 * a stage that bump maps can't read the per state constant
 */
#define ATI_FFP_CONST_BUMPMAT(i) (GL_CON_0_ATI + i)
#define ATI_FFP_CONST_CONSTANT0 GL_CON_0_ATI
#define ATI_FFP_CONST_CONSTANT1 GL_CON_1_ATI
#define ATI_FFP_CONST_CONSTANT2 GL_CON_2_ATI
#define ATI_FFP_CONST_CONSTANT3 GL_CON_3_ATI
#define ATI_FFP_CONST_CONSTANT4 GL_CON_4_ATI
#define ATI_FFP_CONST_CONSTANT5 GL_CON_5_ATI
#define ATI_FFP_CONST_TFACTOR   GL_CON_6_ATI

/* GL_ATI_fragment_shader specific fixed function pipeline description. "Inherits" from the common one */
struct atifs_ffp_desc
{
    struct ffp_frag_desc parent;
    GLuint shader;
    unsigned int num_textures_used;
};

struct atifs_private_data
{
    struct wine_rb_tree fragment_shaders; /* A rb-tree to track fragment pipeline replacement shaders */
};

static const char *debug_dstmod(GLuint mod) {
    switch(mod) {
        case GL_NONE:               return "GL_NONE";
        case GL_2X_BIT_ATI:         return "GL_2X_BIT_ATI";
        case GL_4X_BIT_ATI:         return "GL_4X_BIT_ATI";
        case GL_8X_BIT_ATI:         return "GL_8X_BIT_ATI";
        case GL_HALF_BIT_ATI:       return "GL_HALF_BIT_ATI";
        case GL_QUARTER_BIT_ATI:    return "GL_QUARTER_BIT_ATI";
        case GL_EIGHTH_BIT_ATI:     return "GL_EIGHTH_BIT_ATI";
        case GL_SATURATE_BIT_ATI:   return "GL_SATURATE_BIT_ATI";
        default:                    return "Unexpected modifier\n";
    }
}

static const char *debug_argmod(GLuint mod) {
    switch(mod) {
        case GL_NONE:
            return "GL_NONE";

        case GL_2X_BIT_ATI:
            return "GL_2X_BIT_ATI";
        case GL_COMP_BIT_ATI:
            return "GL_COMP_BIT_ATI";
        case GL_NEGATE_BIT_ATI:
            return "GL_NEGATE_BIT_ATI";
        case GL_BIAS_BIT_ATI:
            return "GL_BIAS_BIT_ATI";

        case GL_2X_BIT_ATI | GL_COMP_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_COMP_BIT_ATI";
        case GL_2X_BIT_ATI | GL_NEGATE_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_NEGATE_BIT_ATI";
        case GL_2X_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_BIAS_BIT_ATI";
        case GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI:
            return "GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI";
        case GL_COMP_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_COMP_BIT_ATI | GL_BIAS_BIT_ATI";
        case GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI";

        case GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI";
        case GL_2X_BIT_ATI | GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI";
        case GL_2X_BIT_ATI | GL_COMP_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_COMP_BIT_ATI | GL_BIAS_BIT_ATI";
        case GL_2X_BIT_ATI | GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI";

        case GL_2X_BIT_ATI | GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI:
            return "GL_2X_BIT_ATI | GL_COMP_BIT_ATI | GL_NEGATE_BIT_ATI | GL_BIAS_BIT_ATI";

        default:
            return "Unexpected argmod combination\n";
    }
}
static const char *debug_register(GLuint reg) {
    switch(reg) {
        case GL_REG_0_ATI:                  return "GL_REG_0_ATI";
        case GL_REG_1_ATI:                  return "GL_REG_1_ATI";
        case GL_REG_2_ATI:                  return "GL_REG_2_ATI";
        case GL_REG_3_ATI:                  return "GL_REG_3_ATI";
        case GL_REG_4_ATI:                  return "GL_REG_4_ATI";
        case GL_REG_5_ATI:                  return "GL_REG_5_ATI";

        case GL_CON_0_ATI:                  return "GL_CON_0_ATI";
        case GL_CON_1_ATI:                  return "GL_CON_1_ATI";
        case GL_CON_2_ATI:                  return "GL_CON_2_ATI";
        case GL_CON_3_ATI:                  return "GL_CON_3_ATI";
        case GL_CON_4_ATI:                  return "GL_CON_4_ATI";
        case GL_CON_5_ATI:                  return "GL_CON_5_ATI";
        case GL_CON_6_ATI:                  return "GL_CON_6_ATI";
        case GL_CON_7_ATI:                  return "GL_CON_7_ATI";

        case GL_ZERO:                       return "GL_ZERO";
        case GL_ONE:                        return "GL_ONE";
        case GL_PRIMARY_COLOR:              return "GL_PRIMARY_COLOR";
        case GL_SECONDARY_INTERPOLATOR_ATI: return "GL_SECONDARY_INTERPOLATOR_ATI";

        default:                            return "Unknown register\n";
    }
}

static const char *debug_swizzle(GLuint swizzle) {
    switch(swizzle) {
        case GL_SWIZZLE_STR_ATI:        return "GL_SWIZZLE_STR_ATI";
        case GL_SWIZZLE_STQ_ATI:        return "GL_SWIZZLE_STQ_ATI";
        case GL_SWIZZLE_STR_DR_ATI:     return "GL_SWIZZLE_STR_DR_ATI";
        case GL_SWIZZLE_STQ_DQ_ATI:     return "GL_SWIZZLE_STQ_DQ_ATI";
        default:                        return "unknown swizzle";
    }
}

static const char *debug_rep(GLuint rep) {
    switch(rep) {
        case GL_NONE:                   return "GL_NONE";
        case GL_RED:                    return "GL_RED";
        case GL_GREEN:                  return "GL_GREEN";
        case GL_BLUE:                   return "GL_BLUE";
        case GL_ALPHA:                  return "GL_ALPHA";
        default:                        return "unknown argrep";
    }
}

static const char *debug_op(GLuint op) {
    switch(op) {
        case GL_MOV_ATI:                return "GL_MOV_ATI";
        case GL_ADD_ATI:                return "GL_ADD_ATI";
        case GL_MUL_ATI:                return "GL_MUL_ATI";
        case GL_SUB_ATI:                return "GL_SUB_ATI";
        case GL_DOT3_ATI:               return "GL_DOT3_ATI";
        case GL_DOT4_ATI:               return "GL_DOT4_ATI";
        case GL_MAD_ATI:                return "GL_MAD_ATI";
        case GL_LERP_ATI:               return "GL_LERP_ATI";
        case GL_CND_ATI:                return "GL_CND_ATI";
        case GL_CND0_ATI:               return "GL_CND0_ATI";
        case GL_DOT2_ADD_ATI:           return "GL_DOT2_ADD_ATI";
        default:                        return "unexpected op";
    }
}

static const char *debug_mask(GLuint mask) {
    switch(mask) {
        case GL_NONE:                           return "GL_NONE";
        case GL_RED_BIT_ATI:                    return "GL_RED_BIT_ATI";
        case GL_GREEN_BIT_ATI:                  return "GL_GREEN_BIT_ATI";
        case GL_BLUE_BIT_ATI:                   return "GL_BLUE_BIT_ATI";
        case GL_RED_BIT_ATI | GL_GREEN_BIT_ATI: return "GL_RED_BIT_ATI | GL_GREEN_BIT_ATI";
        case GL_RED_BIT_ATI | GL_BLUE_BIT_ATI:  return "GL_RED_BIT_ATI | GL_BLUE_BIT_ATI";
        case GL_GREEN_BIT_ATI | GL_BLUE_BIT_ATI:return "GL_GREEN_BIT_ATI | GL_BLUE_BIT_ATI";
        case GL_RED_BIT_ATI | GL_GREEN_BIT_ATI | GL_BLUE_BIT_ATI:return "GL_RED_BIT_ATI | GL_GREEN_BIT_ATI | GL_BLUE_BIT_ATI";
        default:                                return "Unexpected writemask";
    }
}

static void wrap_op1(const struct wined3d_gl_info *gl_info, GLuint op, GLuint dst, GLuint dstMask, GLuint dstMod,
        GLuint arg1, GLuint arg1Rep, GLuint arg1Mod)
{
    if(dstMask == GL_ALPHA) {
        TRACE("glAlphaFragmentOp1ATI(%s, %s, %s, %s, %s, %s)\n", debug_op(op), debug_register(dst), debug_dstmod(dstMod),
              debug_register(arg1), debug_rep(arg1Rep), debug_argmod(arg1Mod));
        GL_EXTCALL(glAlphaFragmentOp1ATI(op, dst, dstMod, arg1, arg1Rep, arg1Mod));
    } else {
        TRACE("glColorFragmentOp1ATI(%s, %s, %s, %s, %s, %s, %s)\n", debug_op(op), debug_register(dst),
              debug_mask(dstMask), debug_dstmod(dstMod),
              debug_register(arg1), debug_rep(arg1Rep), debug_argmod(arg1Mod));
        GL_EXTCALL(glColorFragmentOp1ATI(op, dst, dstMask, dstMod, arg1, arg1Rep, arg1Mod));
    }
}

static void wrap_op2(const struct wined3d_gl_info *gl_info, GLuint op, GLuint dst, GLuint dstMask, GLuint dstMod,
        GLuint arg1, GLuint arg1Rep, GLuint arg1Mod, GLuint arg2, GLuint arg2Rep, GLuint arg2Mod)
{
    if(dstMask == GL_ALPHA) {
        TRACE("glAlphaFragmentOp2ATI(%s, %s, %s, %s, %s, %s, %s, %s, %s)\n", debug_op(op), debug_register(dst), debug_dstmod(dstMod),
              debug_register(arg1), debug_rep(arg1Rep), debug_argmod(arg1Mod),
              debug_register(arg2), debug_rep(arg2Rep), debug_argmod(arg2Mod));
        GL_EXTCALL(glAlphaFragmentOp2ATI(op, dst, dstMod, arg1, arg1Rep, arg1Mod, arg2, arg2Rep, arg2Mod));
    } else {
        TRACE("glColorFragmentOp2ATI(%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)\n", debug_op(op), debug_register(dst),
              debug_mask(dstMask), debug_dstmod(dstMod),
              debug_register(arg1), debug_rep(arg1Rep), debug_argmod(arg1Mod),
              debug_register(arg2), debug_rep(arg2Rep), debug_argmod(arg2Mod));
        GL_EXTCALL(glColorFragmentOp2ATI(op, dst, dstMask, dstMod, arg1, arg1Rep, arg1Mod, arg2, arg2Rep, arg2Mod));
    }
}

static void wrap_op3(const struct wined3d_gl_info *gl_info, GLuint op, GLuint dst, GLuint dstMask, GLuint dstMod,
        GLuint arg1, GLuint arg1Rep, GLuint arg1Mod, GLuint arg2, GLuint arg2Rep, GLuint arg2Mod,
        GLuint arg3, GLuint arg3Rep, GLuint arg3Mod)
{
    if(dstMask == GL_ALPHA) {
        /* Leave some free space to fit "GL_NONE, " in to align most alpha and color op lines */
        TRACE("glAlphaFragmentOp3ATI(%s, %s,          %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)\n", debug_op(op), debug_register(dst), debug_dstmod(dstMod),
              debug_register(arg1), debug_rep(arg1Rep), debug_argmod(arg1Mod),
              debug_register(arg2), debug_rep(arg2Rep), debug_argmod(arg2Mod),
              debug_register(arg3), debug_rep(arg3Rep), debug_argmod(arg3Mod));
        GL_EXTCALL(glAlphaFragmentOp3ATI(op, dst, dstMod,
                                         arg1, arg1Rep, arg1Mod,
                                         arg2, arg2Rep, arg2Mod,
                                         arg3, arg3Rep, arg3Mod));
    } else {
        TRACE("glColorFragmentOp3ATI(%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)\n", debug_op(op), debug_register(dst),
              debug_mask(dstMask), debug_dstmod(dstMod),
              debug_register(arg1), debug_rep(arg1Rep), debug_argmod(arg1Mod),
              debug_register(arg2), debug_rep(arg2Rep), debug_argmod(arg2Mod),
              debug_register(arg3), debug_rep(arg3Rep), debug_argmod(arg3Mod));
        GL_EXTCALL(glColorFragmentOp3ATI(op, dst, dstMask, dstMod,
                                         arg1, arg1Rep, arg1Mod,
                                         arg2, arg2Rep, arg2Mod,
                                         arg3, arg3Rep, arg3Mod));
    }
}

static GLuint register_for_arg(DWORD arg, const struct wined3d_gl_info *gl_info,
        unsigned int stage, GLuint *mod, GLuint *rep, GLuint tmparg)
{
    GLenum ret;

    if(mod) *mod = GL_NONE;
    if(arg == ARG_UNUSED)
    {
        if (rep) *rep = GL_NONE;
        return -1; /* This is the marker for unused registers */
    }

    switch(arg & WINED3DTA_SELECTMASK) {
        case WINED3DTA_DIFFUSE:
            ret = GL_PRIMARY_COLOR;
            break;

        case WINED3DTA_CURRENT:
            /* Note that using GL_REG_0_ATI for the passed on register is safe because
             * texture0 is read at stage0, so in the worst case it is read in the
             * instruction writing to reg0. Afterwards texture0 is not used any longer.
             * If we're reading from current
             */
            ret = stage ? GL_REG_0_ATI : GL_PRIMARY_COLOR;
            break;

        case WINED3DTA_TEXTURE:
            ret = GL_REG_0_ATI + stage;
            break;

        case WINED3DTA_TFACTOR:
            ret = ATI_FFP_CONST_TFACTOR;
            break;

        case WINED3DTA_SPECULAR:
            ret = GL_SECONDARY_INTERPOLATOR_ATI;
            break;

        case WINED3DTA_TEMP:
            ret = tmparg;
            break;

        case WINED3DTA_CONSTANT:
            FIXME("Unhandled source argument WINED3DTA_TEMP\n");
            ret = GL_CON_0_ATI;
            break;

        default:
            FIXME("Unknown source argument %d\n", arg);
            ret = GL_ZERO;
    }

    if(arg & WINED3DTA_COMPLEMENT) {
        if(mod) *mod |= GL_COMP_BIT_ATI;
    }
    if(arg & WINED3DTA_ALPHAREPLICATE) {
        if(rep) *rep = GL_ALPHA;
    } else {
        if(rep) *rep = GL_NONE;
    }
    return ret;
}

static GLuint find_tmpreg(const struct texture_stage_op op[MAX_TEXTURES])
{
    int lowest_read = -1;
    int lowest_write = -1;
    int i;
    BOOL tex_used[MAX_TEXTURES];

    memset(tex_used, 0, sizeof(tex_used));
    for (i = 0; i < MAX_TEXTURES; ++i)
    {
        if (op[i].cop == WINED3D_TOP_DISABLE)
            break;

        if(lowest_read == -1 &&
          (op[i].carg1 == WINED3DTA_TEMP || op[i].carg2 == WINED3DTA_TEMP || op[i].carg0 == WINED3DTA_TEMP ||
           op[i].aarg1 == WINED3DTA_TEMP || op[i].aarg2 == WINED3DTA_TEMP || op[i].aarg0 == WINED3DTA_TEMP)) {
            lowest_read = i;
        }

        if(lowest_write == -1 && op[i].dst == tempreg) {
            lowest_write = i;
        }

        if(op[i].carg1 == WINED3DTA_TEXTURE || op[i].carg2 == WINED3DTA_TEXTURE || op[i].carg0 == WINED3DTA_TEXTURE ||
           op[i].aarg1 == WINED3DTA_TEXTURE || op[i].aarg2 == WINED3DTA_TEXTURE || op[i].aarg0 == WINED3DTA_TEXTURE) {
            tex_used[i] = TRUE;
        }
    }

    /* Temp reg not read? We don't need it, return GL_NONE */
    if(lowest_read == -1) return GL_NONE;

    if(lowest_write >= lowest_read) {
        FIXME("Temp register read before being written\n");
    }

    if(lowest_write == -1) {
        /* This needs a test. Maybe we are supposed to return 0.0/0.0/0.0/0.0, or fail drawprim, or whatever */
        FIXME("Temp register read without being written\n");
        return GL_REG_1_ATI;
    } else if(lowest_write >= 1) {
        /* If we're writing to the temp reg at earliest in stage 1, we can use register 1 for the temp result.
         * there may be texture data stored in reg 1, but we do not need it any longer since stage 1 already
         * read it
         */
        return GL_REG_1_ATI;
    } else {
        /* Search for a free texture register. We have 6 registers available. GL_REG_0_ATI is already used
         * for the regular result
         */
        for(i = 1; i < 6; i++) {
            if(!tex_used[i]) {
                return GL_REG_0_ATI + i;
            }
        }
        /* What to do here? Report it in ValidateDevice? */
        FIXME("Could not find a register for the temporary register\n");
        return 0;
    }
}

static GLuint gen_ati_shader(const struct texture_stage_op op[MAX_TEXTURES], const struct wined3d_gl_info *gl_info)
{
    GLuint ret = GL_EXTCALL(glGenFragmentShadersATI(1));
    unsigned int stage;
    GLuint arg0, arg1, arg2, extrarg;
    GLuint dstmod, argmod0, argmod1, argmod2, argmodextra;
    GLuint rep0, rep1, rep2;
    GLuint swizzle;
    GLuint tmparg = find_tmpreg(op);
    GLuint dstreg;

    if(!ret) {
        ERR("Failed to generate a GL_ATI_fragment_shader shader id\n");
        return 0;
    }
    GL_EXTCALL(glBindFragmentShaderATI(ret));
    checkGLcall("GL_EXTCALL(glBindFragmentShaderATI(ret))");

    TRACE("glBeginFragmentShaderATI()\n");
    GL_EXTCALL(glBeginFragmentShaderATI());
    checkGLcall("GL_EXTCALL(glBeginFragmentShaderATI())");

    /* Pass 1: Generate sampling instructions for perturbation maps */
    for (stage = 0; stage < gl_info->limits.textures; ++stage)
    {
        if (op[stage].cop == WINED3D_TOP_DISABLE)
            break;
        if (op[stage].cop != WINED3D_TOP_BUMPENVMAP
                && op[stage].cop != WINED3D_TOP_BUMPENVMAP_LUMINANCE)
            continue;

        TRACE("glSampleMapATI(GL_REG_%d_ATI, GL_TEXTURE_%d_ARB, GL_SWIZZLE_STR_ATI)\n",
              stage, stage);
        GL_EXTCALL(glSampleMapATI(GL_REG_0_ATI + stage,
                   GL_TEXTURE0_ARB + stage,
                   GL_SWIZZLE_STR_ATI));
        if(op[stage + 1].projected == proj_none) {
            swizzle = GL_SWIZZLE_STR_ATI;
        } else if(op[stage + 1].projected == proj_count4) {
            swizzle = GL_SWIZZLE_STQ_DQ_ATI;
        } else {
            swizzle = GL_SWIZZLE_STR_DR_ATI;
        }
        TRACE("glPassTexCoordATI(GL_REG_%d_ATI, GL_TEXTURE_%d_ARB, %s)\n",
              stage + 1, stage + 1, debug_swizzle(swizzle));
        GL_EXTCALL(glPassTexCoordATI(GL_REG_0_ATI + stage + 1,
                   GL_TEXTURE0_ARB + stage + 1,
                   swizzle));
    }

    /* Pass 2: Generate perturbation calculations */
    for (stage = 0; stage < gl_info->limits.textures; ++stage)
    {
        GLuint argmodextra_x, argmodextra_y;
        struct color_fixup_desc fixup;

        if (op[stage].cop == WINED3D_TOP_DISABLE)
            break;
        if (op[stage].cop != WINED3D_TOP_BUMPENVMAP
                && op[stage].cop != WINED3D_TOP_BUMPENVMAP_LUMINANCE)
            continue;

        fixup = op[stage].color_fixup;
        if (fixup.x_source != CHANNEL_SOURCE_X || fixup.y_source != CHANNEL_SOURCE_Y)
        {
            FIXME("Swizzles not implemented\n");
            argmodextra_x = GL_NONE;
            argmodextra_y = GL_NONE;
        }
        else
        {
            /* Nice thing, we get the color correction for free :-) */
            argmodextra_x = fixup.x_sign_fixup ? GL_2X_BIT_ATI | GL_BIAS_BIT_ATI : GL_NONE;
            argmodextra_y = fixup.y_sign_fixup ? GL_2X_BIT_ATI | GL_BIAS_BIT_ATI : GL_NONE;
        }

        wrap_op3(gl_info, GL_DOT2_ADD_ATI, GL_REG_0_ATI + stage + 1, GL_RED_BIT_ATI, GL_NONE,
                 GL_REG_0_ATI + stage, GL_NONE, argmodextra_x,
                 ATI_FFP_CONST_BUMPMAT(stage), GL_NONE, GL_2X_BIT_ATI | GL_BIAS_BIT_ATI,
                 GL_REG_0_ATI + stage + 1, GL_RED, GL_NONE);

        /* Don't use GL_DOT2_ADD_ATI here because we cannot configure it to read the blue and alpha
         * component of the bump matrix. Instead do this with two MADs:
         *
         * coord.a = tex.r * bump.b + coord.g
         * coord.g = tex.g * bump.a + coord.a
         *
         * The first instruction writes to alpha so it can be coissued with the above DOT2_ADD.
         * coord.a is unused. If the perturbed texture is projected, this was already handled
         * in the glPassTexCoordATI above.
         */
        wrap_op3(gl_info, GL_MAD_ATI, GL_REG_0_ATI + stage + 1, GL_ALPHA, GL_NONE,
                 GL_REG_0_ATI + stage, GL_RED, argmodextra_y,
                 ATI_FFP_CONST_BUMPMAT(stage), GL_BLUE, GL_2X_BIT_ATI | GL_BIAS_BIT_ATI,
                 GL_REG_0_ATI + stage + 1, GL_GREEN, GL_NONE);
        wrap_op3(gl_info, GL_MAD_ATI, GL_REG_0_ATI + stage + 1, GL_GREEN_BIT_ATI, GL_NONE,
                 GL_REG_0_ATI + stage, GL_GREEN, argmodextra_y,
                 ATI_FFP_CONST_BUMPMAT(stage), GL_ALPHA, GL_2X_BIT_ATI | GL_BIAS_BIT_ATI,
                 GL_REG_0_ATI + stage + 1, GL_ALPHA, GL_NONE);
    }

    /* Pass 3: Generate sampling instructions for regular textures */
    for (stage = 0; stage < gl_info->limits.textures; ++stage)
    {
        if (op[stage].cop == WINED3D_TOP_DISABLE)
            break;

        if(op[stage].projected == proj_none) {
            swizzle = GL_SWIZZLE_STR_ATI;
        } else if(op[stage].projected == proj_count3) {
            swizzle = GL_SWIZZLE_STR_DR_ATI;
        } else {
            swizzle = GL_SWIZZLE_STQ_DQ_ATI;
        }

        if ((op[stage].carg0 & WINED3DTA_SELECTMASK) == WINED3DTA_TEXTURE
                || (op[stage].carg1 & WINED3DTA_SELECTMASK) == WINED3DTA_TEXTURE
                || (op[stage].carg2 & WINED3DTA_SELECTMASK) == WINED3DTA_TEXTURE
                || (op[stage].aarg0 & WINED3DTA_SELECTMASK) == WINED3DTA_TEXTURE
                || (op[stage].aarg1 & WINED3DTA_SELECTMASK) == WINED3DTA_TEXTURE
                || (op[stage].aarg2 & WINED3DTA_SELECTMASK) == WINED3DTA_TEXTURE
                || op[stage].cop == WINED3D_TOP_BLEND_TEXTURE_ALPHA)
        {
            if (stage > 0
                    && (op[stage - 1].cop == WINED3D_TOP_BUMPENVMAP
                    || op[stage - 1].cop == WINED3D_TOP_BUMPENVMAP_LUMINANCE))
            {
                TRACE("glSampleMapATI(GL_REG_%d_ATI, GL_REG_%d_ATI, GL_SWIZZLE_STR_ATI)\n",
                      stage, stage);
                GL_EXTCALL(glSampleMapATI(GL_REG_0_ATI + stage,
                           GL_REG_0_ATI + stage,
                           GL_SWIZZLE_STR_ATI));
            } else {
                TRACE("glSampleMapATI(GL_REG_%d_ATI, GL_TEXTURE_%d_ARB, %s)\n",
                    stage, stage, debug_swizzle(swizzle));
                GL_EXTCALL(glSampleMapATI(GL_REG_0_ATI + stage,
                                        GL_TEXTURE0_ARB + stage,
                                        swizzle));
            }
        }
    }

    /* Pass 4: Generate the arithmetic instructions */
    for (stage = 0; stage < MAX_TEXTURES; ++stage)
    {
        if (op[stage].cop == WINED3D_TOP_DISABLE)
        {
            if (!stage)
            {
                /* Handle complete texture disabling gracefully */
                wrap_op1(gl_info, GL_MOV_ATI, GL_REG_0_ATI, GL_NONE, GL_NONE,
                         GL_PRIMARY_COLOR, GL_NONE, GL_NONE);
                wrap_op1(gl_info, GL_MOV_ATI, GL_REG_0_ATI, GL_ALPHA, GL_NONE,
                         GL_PRIMARY_COLOR, GL_NONE, GL_NONE);
            }
            break;
        }

        if(op[stage].dst == tempreg) {
            /* If we're writing to D3DTA_TEMP, but never reading from it we don't have to write there in the first place.
             * skip the entire stage, this saves some GPU time
             */
            if(tmparg == GL_NONE) continue;

            dstreg = tmparg;
        } else {
            dstreg = GL_REG_0_ATI;
        }

        arg0 = register_for_arg(op[stage].carg0, gl_info, stage, &argmod0, &rep0, tmparg);
        arg1 = register_for_arg(op[stage].carg1, gl_info, stage, &argmod1, &rep1, tmparg);
        arg2 = register_for_arg(op[stage].carg2, gl_info, stage, &argmod2, &rep2, tmparg);
        dstmod = GL_NONE;
        argmodextra = GL_NONE;
        extrarg = GL_NONE;

        switch (op[stage].cop)
        {
            case WINED3D_TOP_SELECT_ARG2:
                arg1 = arg2;
                argmod1 = argmod2;
                rep1 = rep2;
                /* fall through */
            case WINED3D_TOP_SELECT_ARG1:
                wrap_op1(gl_info, GL_MOV_ATI, dstreg, GL_NONE, GL_NONE,
                         arg1, rep1, argmod1);
                break;

            case WINED3D_TOP_MODULATE_4X:
                if(dstmod == GL_NONE) dstmod = GL_4X_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_MODULATE_2X:
                if(dstmod == GL_NONE) dstmod = GL_2X_BIT_ATI;
                dstmod |= GL_SATURATE_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_MODULATE:
                wrap_op2(gl_info, GL_MUL_ATI, dstreg, GL_NONE, dstmod,
                         arg1, rep1, argmod1,
                         arg2, rep2, argmod2);
                break;

            case WINED3D_TOP_ADD_SIGNED_2X:
                dstmod = GL_2X_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_ADD_SIGNED:
                argmodextra = GL_BIAS_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_ADD:
                dstmod |= GL_SATURATE_BIT_ATI;
                wrap_op2(gl_info, GL_ADD_ATI, GL_REG_0_ATI, GL_NONE, dstmod,
                         arg1, rep1, argmod1,
                         arg2, rep2, argmodextra | argmod2);
                break;

            case WINED3D_TOP_SUBTRACT:
                dstmod |= GL_SATURATE_BIT_ATI;
                wrap_op2(gl_info, GL_SUB_ATI, dstreg, GL_NONE, dstmod,
                         arg1, rep1, argmod1,
                         arg2, rep2, argmod2);
                break;

            case WINED3D_TOP_ADD_SMOOTH:
                argmodextra = argmod1 & GL_COMP_BIT_ATI ? argmod1 & ~GL_COMP_BIT_ATI : argmod1 | GL_COMP_BIT_ATI;
                /* Dst = arg1 + * arg2(1 -arg 1)
                 *     = arg2 * (1 - arg1) + arg1
                 */
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_NONE, GL_SATURATE_BIT_ATI,
                         arg2, rep2, argmod2,
                         arg1, rep1, argmodextra,
                         arg1, rep1, argmod1);
                break;

            case WINED3D_TOP_BLEND_CURRENT_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_CURRENT, gl_info, stage, NULL, NULL, -1);
                /* fall through */
            case WINED3D_TOP_BLEND_FACTOR_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_TFACTOR, gl_info, stage, NULL, NULL, -1);
                /* fall through */
            case WINED3D_TOP_BLEND_TEXTURE_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_TEXTURE, gl_info, stage, NULL, NULL, -1);
                /* fall through */
            case WINED3D_TOP_BLEND_DIFFUSE_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_DIFFUSE, gl_info, stage, NULL, NULL, -1);
                wrap_op3(gl_info, GL_LERP_ATI, dstreg, GL_NONE, GL_NONE,
                         extrarg, GL_ALPHA, GL_NONE,
                         arg1, rep1, argmod1,
                         arg2, rep2, argmod2);
                break;

            case WINED3D_TOP_BLEND_TEXTURE_ALPHA_PM:
                arg0 = register_for_arg(WINED3DTA_TEXTURE, gl_info, stage, NULL, NULL, -1);
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_NONE, GL_NONE,
                         arg2, rep2,  argmod2,
                         arg0, GL_ALPHA, GL_COMP_BIT_ATI,
                         arg1, rep1,  argmod1);
                break;

            /* D3DTOP_PREMODULATE ???? */

            case WINED3D_TOP_MODULATE_INVALPHA_ADD_COLOR:
                argmodextra = argmod1 & GL_COMP_BIT_ATI ? argmod1 & ~GL_COMP_BIT_ATI : argmod1 | GL_COMP_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_MODULATE_ALPHA_ADD_COLOR:
                if (!argmodextra)
                    argmodextra = argmod1;
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_NONE, GL_SATURATE_BIT_ATI,
                         arg2, rep2,  argmod2,
                         arg1, GL_ALPHA, argmodextra,
                         arg1, rep1,  argmod1);
                break;

            case WINED3D_TOP_MODULATE_INVCOLOR_ADD_ALPHA:
                argmodextra = argmod1 & GL_COMP_BIT_ATI ? argmod1 & ~GL_COMP_BIT_ATI : argmod1 | GL_COMP_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_MODULATE_COLOR_ADD_ALPHA:
                if (!argmodextra)
                    argmodextra = argmod1;
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_NONE, GL_SATURATE_BIT_ATI,
                         arg2, rep2,  argmod2,
                         arg1, rep1,  argmodextra,
                         arg1, GL_ALPHA, argmod1);
                break;

            case WINED3D_TOP_DOTPRODUCT3:
                wrap_op2(gl_info, GL_DOT3_ATI, dstreg, GL_NONE, GL_4X_BIT_ATI | GL_SATURATE_BIT_ATI,
                         arg1, rep1, argmod1 | GL_BIAS_BIT_ATI,
                         arg2, rep2, argmod2 | GL_BIAS_BIT_ATI);
                break;

            case WINED3D_TOP_MULTIPLY_ADD:
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_NONE, GL_SATURATE_BIT_ATI,
                         arg1, rep1, argmod1,
                         arg2, rep2, argmod2,
                         arg0, rep0, argmod0);
                break;

            case WINED3D_TOP_LERP:
                wrap_op3(gl_info, GL_LERP_ATI, dstreg, GL_NONE, GL_NONE,
                         arg0, rep0, argmod0,
                         arg1, rep1, argmod1,
                         arg2, rep2, argmod2);
                break;

            case WINED3D_TOP_BUMPENVMAP:
            case WINED3D_TOP_BUMPENVMAP_LUMINANCE:
                /* Those are handled in the first pass of the shader(generation pass 1 and 2) already */
                break;

            default: FIXME("Unhandled color operation %d on stage %d\n", op[stage].cop, stage);
        }

        arg0 = register_for_arg(op[stage].aarg0, gl_info, stage, &argmod0, NULL, tmparg);
        arg1 = register_for_arg(op[stage].aarg1, gl_info, stage, &argmod1, NULL, tmparg);
        arg2 = register_for_arg(op[stage].aarg2, gl_info, stage, &argmod2, NULL, tmparg);
        dstmod = GL_NONE;
        argmodextra = GL_NONE;
        extrarg = GL_NONE;

        switch (op[stage].aop)
        {
            case WINED3D_TOP_DISABLE:
                /* Get the primary color to the output if on stage 0, otherwise leave register 0 untouched */
                if (!stage)
                {
                    wrap_op1(gl_info, GL_MOV_ATI, GL_REG_0_ATI, GL_ALPHA, GL_NONE,
                             GL_PRIMARY_COLOR, GL_NONE, GL_NONE);
                }
                break;

            case WINED3D_TOP_SELECT_ARG2:
                arg1 = arg2;
                argmod1 = argmod2;
                /* fall through */
            case WINED3D_TOP_SELECT_ARG1:
                wrap_op1(gl_info, GL_MOV_ATI, dstreg, GL_ALPHA, GL_NONE,
                         arg1, GL_NONE, argmod1);
                break;

            case WINED3D_TOP_MODULATE_4X:
                if (dstmod == GL_NONE)
                    dstmod = GL_4X_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_MODULATE_2X:
                if (dstmod == GL_NONE)
                    dstmod = GL_2X_BIT_ATI;
                dstmod |= GL_SATURATE_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_MODULATE:
                wrap_op2(gl_info, GL_MUL_ATI, dstreg, GL_ALPHA, dstmod,
                         arg1, GL_NONE, argmod1,
                         arg2, GL_NONE, argmod2);
                break;

            case WINED3D_TOP_ADD_SIGNED_2X:
                dstmod = GL_2X_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_ADD_SIGNED:
                argmodextra = GL_BIAS_BIT_ATI;
                /* fall through */
            case WINED3D_TOP_ADD:
                dstmod |= GL_SATURATE_BIT_ATI;
                wrap_op2(gl_info, GL_ADD_ATI, dstreg, GL_ALPHA, dstmod,
                         arg1, GL_NONE, argmod1,
                         arg2, GL_NONE, argmodextra | argmod2);
                break;

            case WINED3D_TOP_SUBTRACT:
                dstmod |= GL_SATURATE_BIT_ATI;
                wrap_op2(gl_info, GL_SUB_ATI, dstreg, GL_ALPHA, dstmod,
                         arg1, GL_NONE, argmod1,
                         arg2, GL_NONE, argmod2);
                break;

            case WINED3D_TOP_ADD_SMOOTH:
                argmodextra = argmod1 & GL_COMP_BIT_ATI ? argmod1 & ~GL_COMP_BIT_ATI : argmod1 | GL_COMP_BIT_ATI;
                /* Dst = arg1 + * arg2(1 -arg 1)
                 *     = arg2 * (1 - arg1) + arg1
                 */
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_ALPHA, GL_SATURATE_BIT_ATI,
                         arg2, GL_NONE, argmod2,
                         arg1, GL_NONE, argmodextra,
                         arg1, GL_NONE, argmod1);
                break;

            case WINED3D_TOP_BLEND_CURRENT_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_CURRENT, gl_info, stage, NULL, NULL, -1);
                /* fall through */
            case WINED3D_TOP_BLEND_FACTOR_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_TFACTOR, gl_info, stage, NULL, NULL, -1);
                /* fall through */
            case WINED3D_TOP_BLEND_TEXTURE_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_TEXTURE, gl_info, stage, NULL, NULL, -1);
                /* fall through */
            case WINED3D_TOP_BLEND_DIFFUSE_ALPHA:
                if (extrarg == GL_NONE)
                    extrarg = register_for_arg(WINED3DTA_DIFFUSE, gl_info, stage, NULL, NULL, -1);
                wrap_op3(gl_info, GL_LERP_ATI, dstreg, GL_ALPHA, GL_NONE,
                         extrarg, GL_ALPHA, GL_NONE,
                         arg1, GL_NONE, argmod1,
                         arg2, GL_NONE, argmod2);
                break;

            case WINED3D_TOP_BLEND_TEXTURE_ALPHA_PM:
                arg0 = register_for_arg(WINED3DTA_TEXTURE, gl_info, stage, NULL, NULL, -1);
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_ALPHA, GL_NONE,
                         arg2, GL_NONE,  argmod2,
                         arg0, GL_ALPHA, GL_COMP_BIT_ATI,
                         arg1, GL_NONE,  argmod1);
                break;

            /* D3DTOP_PREMODULATE ???? */

            case WINED3D_TOP_DOTPRODUCT3:
                wrap_op2(gl_info, GL_DOT3_ATI, dstreg, GL_ALPHA, GL_4X_BIT_ATI | GL_SATURATE_BIT_ATI,
                         arg1, GL_NONE, argmod1 | GL_BIAS_BIT_ATI,
                         arg2, GL_NONE, argmod2 | GL_BIAS_BIT_ATI);
                break;

            case WINED3D_TOP_MULTIPLY_ADD:
                wrap_op3(gl_info, GL_MAD_ATI, dstreg, GL_ALPHA, GL_SATURATE_BIT_ATI,
                         arg1, GL_NONE, argmod1,
                         arg2, GL_NONE, argmod2,
                         arg0, GL_NONE, argmod0);
                break;

            case WINED3D_TOP_LERP:
                wrap_op3(gl_info, GL_LERP_ATI, dstreg, GL_ALPHA, GL_SATURATE_BIT_ATI,
                         arg1, GL_NONE, argmod1,
                         arg2, GL_NONE, argmod2,
                         arg0, GL_NONE, argmod0);
                break;

            case WINED3D_TOP_MODULATE_INVALPHA_ADD_COLOR:
            case WINED3D_TOP_MODULATE_ALPHA_ADD_COLOR:
            case WINED3D_TOP_MODULATE_COLOR_ADD_ALPHA:
            case WINED3D_TOP_MODULATE_INVCOLOR_ADD_ALPHA:
            case WINED3D_TOP_BUMPENVMAP:
            case WINED3D_TOP_BUMPENVMAP_LUMINANCE:
                ERR("Application uses an invalid alpha operation\n");
                break;

            default: FIXME("Unhandled alpha operation %d on stage %d\n", op[stage].aop, stage);
        }
    }

    TRACE("glEndFragmentShaderATI()\n");
    GL_EXTCALL(glEndFragmentShaderATI());
    checkGLcall("GL_EXTCALL(glEndFragmentShaderATI())");
    return ret;
}

static void set_tex_op_atifs(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_device *device = context->swapchain->device;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    const struct wined3d_d3d_info *d3d_info = context->d3d_info;
    const struct atifs_ffp_desc *desc;
    struct ffp_frag_settings settings;
    struct atifs_private_data *priv = device->fragment_priv;
    DWORD mapped_stage;
    unsigned int i;

    gen_ffp_frag_op(context, state, &settings, TRUE);
    desc = (const struct atifs_ffp_desc *)find_ffp_frag_shader(&priv->fragment_shaders, &settings);
    if(!desc) {
        struct atifs_ffp_desc *new_desc = HeapAlloc(GetProcessHeap(), 0, sizeof(*new_desc));
        if (!new_desc)
        {
            ERR("Out of memory\n");
            return;
        }
        new_desc->num_textures_used = 0;
        for (i = 0; i < d3d_info->limits.ffp_blend_stages; ++i)
        {
            if (settings.op[i].cop == WINED3D_TOP_DISABLE)
                break;
            new_desc->num_textures_used = i + 1;
        }

        new_desc->parent.settings = settings;
        new_desc->shader = gen_ati_shader(settings.op, gl_info);
        add_ffp_frag_shader(&priv->fragment_shaders, &new_desc->parent);
        TRACE("Allocated fixed function replacement shader descriptor %p\n", new_desc);
        desc = new_desc;
    }

    /* GL_ATI_fragment_shader depends on the GL_TEXTURE_xD enable settings. Update the texture stages
     * used by this shader
     */
    for (i = 0; i < desc->num_textures_used; ++i)
    {
        mapped_stage = device->texUnitMap[i];
        if (mapped_stage != WINED3D_UNMAPPED_STAGE)
        {
            context_active_texture(context, gl_info, mapped_stage);
            texture_activate_dimensions(state->textures[i], gl_info);
        }
    }

    GL_EXTCALL(glBindFragmentShaderATI(desc->shader));
}

static void state_texfactor_atifs(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    float col[4];

    D3DCOLORTOGLFLOAT4(state->render_states[WINED3D_RS_TEXTUREFACTOR], col);
    GL_EXTCALL(glSetFragmentShaderConstantATI(ATI_FFP_CONST_TFACTOR, col));
    checkGLcall("glSetFragmentShaderConstantATI(ATI_FFP_CONST_TFACTOR, col)");
}

static void set_bumpmat(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    DWORD stage = (state_id - STATE_TEXTURESTAGE(0, 0)) / (WINED3D_HIGHEST_TEXTURE_STATE + 1);
    const struct wined3d_gl_info *gl_info = context->gl_info;
    float mat[2][2];

    mat[0][0] = *((float *)&state->texture_states[stage][WINED3D_TSS_BUMPENV_MAT00]);
    mat[1][0] = *((float *)&state->texture_states[stage][WINED3D_TSS_BUMPENV_MAT01]);
    mat[0][1] = *((float *)&state->texture_states[stage][WINED3D_TSS_BUMPENV_MAT10]);
    mat[1][1] = *((float *)&state->texture_states[stage][WINED3D_TSS_BUMPENV_MAT11]);
    /* GL_ATI_fragment_shader allows only constants from 0.0 to 1.0, but the bumpmat
     * constants can be in any range. While they should stay between [-1.0 and 1.0] because
     * Shader Model 1.x pixel shaders are clamped to that range negative values are used occasionally,
     * for example by our d3d9 test. So to get negative values scale -1;1 to 0;1 and undo that in the
     * shader(it is free). This might potentially reduce precision. However, if the hardware does
     * support proper floats it shouldn't, and if it doesn't we can't get anything better anyway
     */
    mat[0][0] = (mat[0][0] + 1.0f) * 0.5f;
    mat[1][0] = (mat[1][0] + 1.0f) * 0.5f;
    mat[0][1] = (mat[0][1] + 1.0f) * 0.5f;
    mat[1][1] = (mat[1][1] + 1.0f) * 0.5f;
    GL_EXTCALL(glSetFragmentShaderConstantATI(ATI_FFP_CONST_BUMPMAT(stage), (float *) mat));
    checkGLcall("glSetFragmentShaderConstantATI(ATI_FFP_CONST_BUMPMAT(stage), mat)");
}

static void textransform(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (!isStateDirty(context, STATE_PIXELSHADER))
        set_tex_op_atifs(context, state, state_id);
}

static void atifs_apply_pixelshader(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    context->last_was_pshader = use_ps(state);
    /* The ATIFS code does not support pixel shaders currently, but we have to
     * provide a state handler to call shader_select to select a vertex shader
     * if one is applied because the vertex shader state may defer calling the
     * shader backend if the pshader state is dirty.
     *
     * In theory the application should not be able to mark the pixel shader
     * dirty because it cannot create a shader, and thus has no way to set the
     * state to something != NULL. However, a different pipeline part may link
     * a different state to its pixelshader handler, thus a pshader state
     * exists and can be dirtified. Also the pshader is always dirtified at
     * startup, and blitting disables all shaders and dirtifies all shader
     * states. If atifs can deal with this it keeps the rest of the code
     * simpler. */
    context->select_shader = 1;
    context->load_constants = 1;
}

static void atifs_srgbwriteenable(struct wined3d_context *context, const struct wined3d_state *state, DWORD state_id)
{
    if (state->render_states[WINED3D_RS_SRGBWRITEENABLE])
        WARN("sRGB writes are not supported by this fragment pipe.\n");
}

static const struct StateEntryTemplate atifs_fragmentstate_template[] = {
    {STATE_RENDER(WINED3D_RS_TEXTUREFACTOR),              { STATE_RENDER(WINED3D_RS_TEXTUREFACTOR),             state_texfactor_atifs   }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGCOLOR),                   { STATE_RENDER(WINED3D_RS_FOGCOLOR),                  state_fogcolor          }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGDENSITY),                 { STATE_RENDER(WINED3D_RS_FOGDENSITY),                state_fogdensity        }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGENABLE),                  { STATE_RENDER(WINED3D_RS_FOGENABLE),                 state_fog_fragpart      }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGTABLEMODE),               { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGVERTEXMODE),              { STATE_RENDER(WINED3D_RS_FOGENABLE),                 NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGSTART),                   { STATE_RENDER(WINED3D_RS_FOGSTART),                  state_fogstartend       }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_FOGEND),                     { STATE_RENDER(WINED3D_RS_FOGSTART),                  NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_RENDER(WINED3D_RS_SRGBWRITEENABLE),            { STATE_RENDER(WINED3D_RS_SRGBWRITEENABLE),           atifs_srgbwriteenable   }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        set_tex_op_atifs        }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(0, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(1, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(2, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(3, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(4, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(5, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(6, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_COLOR_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_COLOR_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_COLOR_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_COLOR_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_ALPHA_OP),         { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_ALPHA_ARG1),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_ALPHA_ARG2),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_ALPHA_ARG0),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_RESULT_ARG),       { STATE_TEXTURESTAGE(0, WINED3D_TSS_COLOR_OP),        NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT00),    { STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT00),   set_bumpmat             }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT01),    { STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT10),    { STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT11),    { STATE_TEXTURESTAGE(7, WINED3D_TSS_BUMPENV_MAT00),   NULL                    }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(0),                                   { STATE_SAMPLER(0),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(1),                                   { STATE_SAMPLER(1),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(2),                                   { STATE_SAMPLER(2),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(3),                                   { STATE_SAMPLER(3),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(4),                                   { STATE_SAMPLER(4),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(5),                                   { STATE_SAMPLER(5),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(6),                                   { STATE_SAMPLER(6),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    { STATE_SAMPLER(7),                                   { STATE_SAMPLER(7),                                   sampler_texdim          }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(0,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(0, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(1,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(1, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(2,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(2, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(3,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(3, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(4,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(4, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(5,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(5, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(6,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(6, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_TEXTURESTAGE(7,WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), {STATE_TEXTURESTAGE(7, WINED3D_TSS_TEXTURE_TRANSFORM_FLAGS), textransform      }, WINED3D_GL_EXT_NONE             },
    {STATE_PIXELSHADER,                                   { STATE_PIXELSHADER,                                  atifs_apply_pixelshader }, WINED3D_GL_EXT_NONE             },
    {0 /* Terminate */,                                   { 0,                                                  0                       }, WINED3D_GL_EXT_NONE             },
};

/* Context activation is done by the caller. */
static void atifs_enable(const struct wined3d_gl_info *gl_info, BOOL enable)
{
    if (enable)
    {
        gl_info->gl_ops.gl.p_glEnable(GL_FRAGMENT_SHADER_ATI);
        checkGLcall("glEnable(GL_FRAGMENT_SHADER_ATI)");
    }
    else
    {
        gl_info->gl_ops.gl.p_glDisable(GL_FRAGMENT_SHADER_ATI);
        checkGLcall("glDisable(GL_FRAGMENT_SHADER_ATI)");
    }
}

static void atifs_get_caps(const struct wined3d_gl_info *gl_info, struct fragment_caps *caps)
{
    caps->wined3d_caps = WINED3D_FRAGMENT_CAP_PROJ_CONTROL;
    caps->PrimitiveMiscCaps = WINED3DPMISCCAPS_TSSARGTEMP;
    caps->TextureOpCaps =  WINED3DTEXOPCAPS_DISABLE                     |
                           WINED3DTEXOPCAPS_SELECTARG1                  |
                           WINED3DTEXOPCAPS_SELECTARG2                  |
                           WINED3DTEXOPCAPS_MODULATE4X                  |
                           WINED3DTEXOPCAPS_MODULATE2X                  |
                           WINED3DTEXOPCAPS_MODULATE                    |
                           WINED3DTEXOPCAPS_ADDSIGNED2X                 |
                           WINED3DTEXOPCAPS_ADDSIGNED                   |
                           WINED3DTEXOPCAPS_ADD                         |
                           WINED3DTEXOPCAPS_SUBTRACT                    |
                           WINED3DTEXOPCAPS_ADDSMOOTH                   |
                           WINED3DTEXOPCAPS_BLENDCURRENTALPHA           |
                           WINED3DTEXOPCAPS_BLENDFACTORALPHA            |
                           WINED3DTEXOPCAPS_BLENDTEXTUREALPHA           |
                           WINED3DTEXOPCAPS_BLENDDIFFUSEALPHA           |
                           WINED3DTEXOPCAPS_BLENDTEXTUREALPHAPM         |
                           WINED3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR      |
                           WINED3DTEXOPCAPS_MODULATECOLOR_ADDALPHA      |
                           WINED3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA   |
                           WINED3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR   |
                           WINED3DTEXOPCAPS_DOTPRODUCT3                 |
                           WINED3DTEXOPCAPS_MULTIPLYADD                 |
                           WINED3DTEXOPCAPS_LERP                        |
                           WINED3DTEXOPCAPS_BUMPENVMAP;

    /* TODO: Implement WINED3DTEXOPCAPS_BUMPENVMAPLUMINANCE
    and WINED3DTEXOPCAPS_PREMODULATE */

    /* GL_ATI_fragment_shader always supports 6 textures, which was the limit on r200 cards
     * which this extension is exclusively focused on(later cards have GL_ARB_fragment_program).
     * If the current card has more than 8 fixed function textures in OpenGL's regular fixed
     * function pipeline then the ATI_fragment_shader backend imposes a stricter limit. This
     * shouldn't be too hard since Nvidia cards have a limit of 4 textures with the default ffp
     * pipeline, and almost all games are happy with that. We can however support up to 8
     * texture stages because we have a 2nd pass limit of 8 instructions, and per stage we use
     * only 1 instruction.
     *
     * The proper fix for this is not to use GL_ATI_fragment_shader on cards newer than the
     * r200 series and use an ARB or GLSL shader instead
     */
    caps->MaxTextureBlendStages   = 8;
    caps->MaxSimultaneousTextures = 6;
}

static void *atifs_alloc(const struct wined3d_shader_backend_ops *shader_backend, void *shader_priv)
{
    struct atifs_private_data *priv;

    if (!(priv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*priv))))
        return NULL;

    if (wine_rb_init(&priv->fragment_shaders, &wined3d_ffp_frag_program_rb_functions) == -1)
    {
        ERR("Failed to initialize rbtree.\n");
        HeapFree(GetProcessHeap(), 0, priv);
        return NULL;
    }

    return priv;
}

/* Context activation is done by the caller. */
static void atifs_free_ffpshader(struct wine_rb_entry *entry, void *cb_ctx)
{
    const struct wined3d_gl_info *gl_info = cb_ctx;
    struct atifs_ffp_desc *entry_ati = WINE_RB_ENTRY_VALUE(entry, struct atifs_ffp_desc, parent.entry);

    GL_EXTCALL(glDeleteFragmentShaderATI(entry_ati->shader));
    checkGLcall("glDeleteFragmentShaderATI(entry->shader)");
    HeapFree(GetProcessHeap(), 0, entry_ati);
}

/* Context activation is done by the caller. */
static void atifs_free(struct wined3d_device *device)
{
    struct atifs_private_data *priv = device->fragment_priv;

    wine_rb_destroy(&priv->fragment_shaders, atifs_free_ffpshader, &device->adapter->gl_info);

    HeapFree(GetProcessHeap(), 0, priv);
    device->fragment_priv = NULL;
}

static BOOL atifs_color_fixup_supported(struct color_fixup_desc fixup)
{
    if (TRACE_ON(d3d_shader) && TRACE_ON(d3d))
    {
        TRACE("Checking support for fixup:\n");
        dump_color_fixup_desc(fixup);
    }

    /* We only support sign fixup of the first two channels. */
    if (fixup.x_source == CHANNEL_SOURCE_X && fixup.y_source == CHANNEL_SOURCE_Y
            && fixup.z_source == CHANNEL_SOURCE_Z && fixup.w_source == CHANNEL_SOURCE_W
            && !fixup.z_sign_fixup && !fixup.w_sign_fixup)
    {
        TRACE("[OK]\n");
        return TRUE;
    }

    TRACE("[FAILED]\n");
    return FALSE;
}

const struct fragment_pipeline atifs_fragment_pipeline = {
    atifs_enable,
    atifs_get_caps,
    atifs_alloc,
    atifs_free,
    atifs_color_fixup_supported,
    atifs_fragmentstate_template,
};
