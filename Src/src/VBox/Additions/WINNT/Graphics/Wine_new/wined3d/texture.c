/*
 * Copyright 2002-2005 Jason Edmeades
 * Copyright 2002-2005 Raphael Junqueira
 * Copyright 2005 Oliver Stieber
 * Copyright 2007-2009, 2013 Stefan Dösinger for CodeWeavers
 * Copyright 2009-2011 Henri Verbeet for CodeWeavers
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
#include "wined3d_private.h"

#ifdef VBOX_WITH_WDDM
# include "../../common/VBoxVideoTools.h"
#endif

WINE_DEFAULT_DEBUG_CHANNEL(d3d_texture);

static HRESULT wined3d_texture_init(struct wined3d_texture *texture, const struct wined3d_texture_ops *texture_ops,
        UINT layer_count, UINT level_count, const struct wined3d_resource_desc *desc, struct wined3d_device *device,
        void *parent, const struct wined3d_parent_ops *parent_ops, const struct wined3d_resource_ops *resource_ops
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
        )
{
    const struct wined3d_format *format = wined3d_get_format(&device->adapter->gl_info, desc->format);
    HRESULT hr;

    TRACE("texture %p, texture_ops %p, layer_count %u, level_count %u, resource_type %s, format %s, "
            "multisample_type %#x, multisample_quality %#x, usage %s, pool %s, width %u, height %u, depth %u, "
            "device %p, parent %p, parent_ops %p, resource_ops %p.\n",
            texture, texture_ops, layer_count, level_count, debug_d3dresourcetype(desc->resource_type),
            debug_d3dformat(desc->format), desc->multisample_type, desc->multisample_quality,
            debug_d3dusage(desc->usage), debug_d3dpool(desc->pool), desc->width, desc->height, desc->depth,
            device, parent, parent_ops, resource_ops);

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = resource_init(&texture->resource, device, desc->resource_type, format,
            desc->multisample_type, desc->multisample_quality, desc->usage, desc->pool,
            desc->width, desc->height, desc->depth, 0, parent, parent_ops, resource_ops,
            shared_handle, pavClientMem ? pavClientMem[0] : NULL)))
#else
    if (FAILED(hr = resource_init(&texture->resource, device, desc->resource_type, format,
            desc->multisample_type, desc->multisample_quality, desc->usage, desc->pool,
            desc->width, desc->height, desc->depth, 0, parent, parent_ops, resource_ops)))
#endif
    {
        WARN("Failed to initialize resource, returning %#x\n", hr);
        return hr;
    }

    texture->texture_ops = texture_ops;
    texture->sub_resources = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            level_count * layer_count * sizeof(*texture->sub_resources));
    if (!texture->sub_resources)
    {
        ERR("Failed to allocate sub-resource array.\n");
        resource_cleanup(&texture->resource);
        return E_OUTOFMEMORY;
    }

    texture->layer_count = layer_count;
    texture->level_count = level_count;
    texture->filter_type = (desc->usage & WINED3DUSAGE_AUTOGENMIPMAP) ? WINED3D_TEXF_LINEAR : WINED3D_TEXF_NONE;
    texture->lod = 0;
    texture->texture_rgb.dirty = TRUE;
    texture->texture_srgb.dirty = TRUE;
    texture->flags = WINED3D_TEXTURE_POW2_MAT_IDENT;

    if (texture->resource.format->flags & WINED3DFMT_FLAG_FILTERING)
    {
        texture->min_mip_lookup = minMipLookup;
        texture->mag_lookup = magLookup;
    }
    else
    {
        texture->min_mip_lookup = minMipLookup_noFilter;
        texture->mag_lookup = magLookup_noFilter;
    }

    return WINED3D_OK;
}

/* A GL context is provided by the caller */
static void gltexture_delete(const struct wined3d_gl_info *gl_info, struct gl_texture *tex)
{
#ifdef VBOX_WITH_WDDM
    texture_gl_delete(tex->name);
#else
    gl_info->gl_ops.gl.p_glDeleteTextures(1, &tex->name);
#endif
    tex->name = 0;
}

static void wined3d_texture_unload(struct wined3d_texture *texture)
{
    struct wined3d_device *device = texture->resource.device;
    struct wined3d_context *context = NULL;

    if (texture->texture_rgb.name || texture->texture_srgb.name)
    {
        context = context_acquire(device, NULL);
    }

    if (texture->texture_rgb.name)
        gltexture_delete(context->gl_info, &texture->texture_rgb);

    if (texture->texture_srgb.name)
        gltexture_delete(context->gl_info, &texture->texture_srgb);

    if (context) context_release(context);

    wined3d_texture_set_dirty(texture, TRUE);

    resource_unload(&texture->resource);
}

static void wined3d_texture_cleanup(struct wined3d_texture *texture)
{
    UINT sub_count = texture->level_count * texture->layer_count;
    UINT i;

    TRACE("texture %p.\n", texture);

#ifdef VBOX_WITH_WINE_FIX_TEXCLEAR
    /* make texture unload first, because otherwise we may fail on context_acquire done for texture cleanup
     * because the swapchain's surfaces might be destroyed and we may fail to select any render target in context_acquire */
    wined3d_texture_unload(texture);
#endif

    for (i = 0; i < sub_count; ++i)
    {
        struct wined3d_resource *sub_resource = texture->sub_resources[i];

        if (sub_resource)
            texture->texture_ops->texture_sub_resource_cleanup(sub_resource);
    }

#ifndef VBOX_WITH_WINE_FIX_TEXCLEAR
    wined3d_texture_unload(texture);
#endif
    HeapFree(GetProcessHeap(), 0, texture->sub_resources);
    resource_cleanup(&texture->resource);
}

void wined3d_texture_set_dirty(struct wined3d_texture *texture, BOOL dirty)
{
    texture->texture_rgb.dirty = dirty;
    texture->texture_srgb.dirty = dirty;
}

/* Context activation is done by the caller. */
static HRESULT wined3d_texture_bind(struct wined3d_texture *texture,
        struct wined3d_context *context, BOOL srgb, BOOL *set_surface_desc)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    struct gl_texture *gl_tex;
    BOOL new_texture = FALSE;
    HRESULT hr = WINED3D_OK;
    GLenum target;

    TRACE("texture %p, context %p, srgb %#x, set_surface_desc %p.\n", texture, context, srgb, set_surface_desc);

    /* sRGB mode cache for preload() calls outside drawprim. */
    if (srgb)
        texture->flags |= WINED3D_TEXTURE_IS_SRGB;
    else
        texture->flags &= ~WINED3D_TEXTURE_IS_SRGB;

    gl_tex = wined3d_texture_get_gl_texture(texture, context->gl_info, srgb);
    target = texture->target;

    /* Generate a texture name if we don't already have one. */
    if (!gl_tex->name)
    {
#ifdef VBOX_WITH_WDDM
        if (VBOXSHRC_IS_SHARED_OPENED(texture))
        {
            ERR("should not be here!");
            gl_tex->name = (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture);
            pglChromiumParameteriCR(GL_RCUSAGE_TEXTURE_SET_CR, gl_tex->name);
            TRACE("Assigned shared texture %d\n", gl_tex->name);
        }
        else
#endif
        {
#ifdef VBOX_WITH_WDDM
            new_texture = TRUE;
#endif
            *set_surface_desc = TRUE;
            gl_info->gl_ops.gl.p_glGenTextures(1, &gl_tex->name);
            checkGLcall("glGenTextures");
            TRACE("Generated texture %d.\n", gl_tex->name);
#ifdef VBOX_WITH_WDDM
            if (VBOXSHRC_IS_SHARED(texture))
            {
                VBOXSHRC_SET_SHAREHANDLE(texture, gl_tex->name);
            }
#endif
        }
#ifndef VBOX
        if (texture->resource.pool == WINED3D_POOL_DEFAULT)
        {
            /* Tell OpenGL to try and keep this texture in video ram (well mostly). */
            GLclampf tmp = 0.9f;
            gl_info->gl_ops.gl.p_glPrioritizeTextures(1, &gl_tex->name, &tmp);
        }
#else
        /* chromium code on host fails to resolve texture name to texture obj,
         * most likely because the texture does not get created until it is bound
         * @todo: investigate */
#endif
        /* Initialise the state of the texture object to the OpenGL defaults,
         * not the D3D defaults. */
        gl_tex->states[WINED3DTEXSTA_ADDRESSU] = WINED3D_TADDRESS_WRAP;
        gl_tex->states[WINED3DTEXSTA_ADDRESSV] = WINED3D_TADDRESS_WRAP;
        gl_tex->states[WINED3DTEXSTA_ADDRESSW] = WINED3D_TADDRESS_WRAP;
        gl_tex->states[WINED3DTEXSTA_BORDERCOLOR] = 0;
        gl_tex->states[WINED3DTEXSTA_MAGFILTER] = WINED3D_TEXF_LINEAR;
        gl_tex->states[WINED3DTEXSTA_MINFILTER] = WINED3D_TEXF_POINT; /* GL_NEAREST_MIPMAP_LINEAR */
        gl_tex->states[WINED3DTEXSTA_MIPFILTER] = WINED3D_TEXF_LINEAR; /* GL_NEAREST_MIPMAP_LINEAR */
        gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL] = 0;
        gl_tex->states[WINED3DTEXSTA_MAXANISOTROPY] = 1;
        if (context->gl_info->supported[EXT_TEXTURE_SRGB_DECODE])
            gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = TRUE;
        else
            gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = srgb;
        gl_tex->states[WINED3DTEXSTA_SHADOW] = FALSE;
        wined3d_texture_set_dirty(texture, TRUE);
#ifndef VBOX_WITH_WDDM
        new_texture = TRUE;
#endif

#ifdef VBOX_WITH_WDDM
        if (new_texture
                && texture->resource.usage & WINED3DUSAGE_AUTOGENMIPMAP)
#else
        if (texture->resource.usage & WINED3DUSAGE_AUTOGENMIPMAP)
#endif
        {
            /* This means double binding the texture at creation, but keeps
             * the code simpler all in all, and the run-time path free from
             * additional checks. */
            context_bind_texture(context, target, gl_tex->name);
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_GENERATE_MIPMAP_SGIS, GL_TRUE);
            checkGLcall("glTexParameteri(target, GL_GENERATE_MIPMAP_SGIS, GL_TRUE)");
        }
    }
    else
    {
        *set_surface_desc = FALSE;
    }

    if (gl_tex->name)
    {
        context_bind_texture(context, target, gl_tex->name);
        if (new_texture)
        {
            /* For a new texture we have to set the texture levels after
             * binding the texture. Beware that texture rectangles do not
             * support mipmapping, but set the maxmiplevel if we're relying
             * on the partial GL_ARB_texture_non_power_of_two emulation with
             * texture rectangles. (I.e., do not care about cond_np2 here,
             * just look for GL_TEXTURE_RECTANGLE_ARB.) */
            if (target != GL_TEXTURE_RECTANGLE_ARB)
            {
                TRACE("Setting GL_TEXTURE_MAX_LEVEL to %u.\n", texture->level_count - 1);
                gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, texture->level_count - 1);
                checkGLcall("glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, texture->level_count)");
            }
            if (target == GL_TEXTURE_CUBE_MAP_ARB)
            {
                /* Cubemaps are always set to clamp, regardless of the sampler state. */
                gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            }
        }
    }
    else
    {
        ERR("This texture doesn't have an OpenGL texture assigned to it.\n");
        hr = WINED3DERR_INVALIDCALL;
    }

    return hr;
}

/* Context activation is done by the caller. */
static void apply_wrap(const struct wined3d_gl_info *gl_info, GLenum target,
        enum wined3d_texture_address d3d_wrap, GLenum param, BOOL cond_np2)
{
    GLint gl_wrap;

    if (d3d_wrap < WINED3D_TADDRESS_WRAP || d3d_wrap > WINED3D_TADDRESS_MIRROR_ONCE)
    {
        FIXME("Unrecognized or unsupported texture address mode %#x.\n", d3d_wrap);
        return;
    }

    /* Cubemaps are always set to clamp, regardless of the sampler state. */
    if (target == GL_TEXTURE_CUBE_MAP_ARB
            || (cond_np2 && d3d_wrap == WINED3D_TADDRESS_WRAP))
        gl_wrap = GL_CLAMP_TO_EDGE;
    else
        gl_wrap = gl_info->wrap_lookup[d3d_wrap - WINED3D_TADDRESS_WRAP];

    TRACE("Setting param %#x to %#x for target %#x.\n", param, gl_wrap, target);
    gl_info->gl_ops.gl.p_glTexParameteri(target, param, gl_wrap);
    checkGLcall("glTexParameteri(target, param, gl_wrap)");
}

/* Context activation is done by the caller (state handler). */
void wined3d_texture_apply_state_changes(struct wined3d_texture *texture,
        const DWORD sampler_states[WINED3D_HIGHEST_SAMPLER_STATE + 1],
        const struct wined3d_gl_info *gl_info)
{
    BOOL cond_np2 = texture->flags & WINED3D_TEXTURE_COND_NP2;
    GLenum target = texture->target;
    struct gl_texture *gl_tex;
    DWORD state;
    DWORD aniso;

    TRACE("texture %p, sampler_states %p.\n", texture, sampler_states);

    gl_tex = wined3d_texture_get_gl_texture(texture, gl_info,
            texture->flags & WINED3D_TEXTURE_IS_SRGB);

    /* This function relies on the correct texture being bound and loaded. */

    if (sampler_states[WINED3D_SAMP_ADDRESS_U] != gl_tex->states[WINED3DTEXSTA_ADDRESSU])
    {
        state = sampler_states[WINED3D_SAMP_ADDRESS_U];
        apply_wrap(gl_info, target, state, GL_TEXTURE_WRAP_S, cond_np2);
        gl_tex->states[WINED3DTEXSTA_ADDRESSU] = state;
    }

    if (sampler_states[WINED3D_SAMP_ADDRESS_V] != gl_tex->states[WINED3DTEXSTA_ADDRESSV])
    {
        state = sampler_states[WINED3D_SAMP_ADDRESS_V];
        apply_wrap(gl_info, target, state, GL_TEXTURE_WRAP_T, cond_np2);
        gl_tex->states[WINED3DTEXSTA_ADDRESSV] = state;
    }

    if (sampler_states[WINED3D_SAMP_ADDRESS_W] != gl_tex->states[WINED3DTEXSTA_ADDRESSW])
    {
        state = sampler_states[WINED3D_SAMP_ADDRESS_W];
        apply_wrap(gl_info, target, state, GL_TEXTURE_WRAP_R, cond_np2);
        gl_tex->states[WINED3DTEXSTA_ADDRESSW] = state;
    }

    if (sampler_states[WINED3D_SAMP_BORDER_COLOR] != gl_tex->states[WINED3DTEXSTA_BORDERCOLOR])
    {
        float col[4];

        state = sampler_states[WINED3D_SAMP_BORDER_COLOR];
        D3DCOLORTOGLFLOAT4(state, col);
        TRACE("Setting border color for %#x to %#x.\n", target, state);
        gl_info->gl_ops.gl.p_glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, &col[0]);
        checkGLcall("glTexParameterfv(..., GL_TEXTURE_BORDER_COLOR, ...)");
        gl_tex->states[WINED3DTEXSTA_BORDERCOLOR] = state;
    }

    if (sampler_states[WINED3D_SAMP_MAG_FILTER] != gl_tex->states[WINED3DTEXSTA_MAGFILTER])
    {
        GLint gl_value;

        state = sampler_states[WINED3D_SAMP_MAG_FILTER];
        if (state > WINED3D_TEXF_ANISOTROPIC)
            FIXME("Unrecognized or unsupported MAGFILTER* value %d.\n", state);

        gl_value = wined3d_gl_mag_filter(texture->mag_lookup,
                min(max(state, WINED3D_TEXF_POINT), WINED3D_TEXF_LINEAR));
        TRACE("ValueMAG=%#x setting MAGFILTER to %#x.\n", state, gl_value);
        gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MAG_FILTER, gl_value);

        gl_tex->states[WINED3DTEXSTA_MAGFILTER] = state;
    }

    if ((sampler_states[WINED3D_SAMP_MIN_FILTER] != gl_tex->states[WINED3DTEXSTA_MINFILTER]
            || sampler_states[WINED3D_SAMP_MIP_FILTER] != gl_tex->states[WINED3DTEXSTA_MIPFILTER]
            || sampler_states[WINED3D_SAMP_MAX_MIP_LEVEL] != gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL]))
    {
        GLint gl_value;

        gl_tex->states[WINED3DTEXSTA_MIPFILTER] = sampler_states[WINED3D_SAMP_MIP_FILTER];
        gl_tex->states[WINED3DTEXSTA_MINFILTER] = sampler_states[WINED3D_SAMP_MIN_FILTER];
        gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL] = sampler_states[WINED3D_SAMP_MAX_MIP_LEVEL];

        if (gl_tex->states[WINED3DTEXSTA_MINFILTER] > WINED3D_TEXF_ANISOTROPIC
            || gl_tex->states[WINED3DTEXSTA_MIPFILTER] > WINED3D_TEXF_ANISOTROPIC)
        {
            FIXME("Unrecognized or unsupported MIN_FILTER value %#x MIP_FILTER value %#x.\n",
                  gl_tex->states[WINED3DTEXSTA_MINFILTER],
                  gl_tex->states[WINED3DTEXSTA_MIPFILTER]);
        }
        gl_value = wined3d_gl_min_mip_filter(texture->min_mip_lookup,
                min(max(sampler_states[WINED3D_SAMP_MIN_FILTER], WINED3D_TEXF_POINT), WINED3D_TEXF_LINEAR),
                min(max(sampler_states[WINED3D_SAMP_MIP_FILTER], WINED3D_TEXF_NONE), WINED3D_TEXF_LINEAR));

        TRACE("ValueMIN=%#x, ValueMIP=%#x, setting MINFILTER to %#x.\n",
              sampler_states[WINED3D_SAMP_MIN_FILTER],
              sampler_states[WINED3D_SAMP_MIP_FILTER], gl_value);
        gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, gl_value);
        checkGLcall("glTexParameter GL_TEXTURE_MIN_FILTER, ...");

        if (!cond_np2)
        {
            if (gl_tex->states[WINED3DTEXSTA_MIPFILTER] == WINED3D_TEXF_NONE)
                gl_value = texture->lod;
            else if (gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL] >= texture->level_count)
                gl_value = texture->level_count - 1;
            else if (gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL] < texture->lod)
                /* texture->lod is already clamped in the setter. */
                gl_value = texture->lod;
            else
                gl_value = gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL];

            /* Note that WINED3D_SAMP_MAX_MIP_LEVEL specifies the largest mipmap
             * (default 0), while GL_TEXTURE_MAX_LEVEL specifies the smallest
             * mimap used (default 1000). So WINED3D_SAMP_MAX_MIP_LEVEL
             * corresponds to GL_TEXTURE_BASE_LEVEL. */
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, gl_value);
        }
    }

    if ((gl_tex->states[WINED3DTEXSTA_MAGFILTER] != WINED3D_TEXF_ANISOTROPIC
            && gl_tex->states[WINED3DTEXSTA_MINFILTER] != WINED3D_TEXF_ANISOTROPIC
            && gl_tex->states[WINED3DTEXSTA_MIPFILTER] != WINED3D_TEXF_ANISOTROPIC)
            || cond_np2)
        aniso = 1;
    else
        aniso = sampler_states[WINED3D_SAMP_MAX_ANISOTROPY];

    if (gl_tex->states[WINED3DTEXSTA_MAXANISOTROPY] != aniso)
    {
        if (gl_info->supported[EXT_TEXTURE_FILTER_ANISOTROPIC])
        {
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
            checkGLcall("glTexParameteri(GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso)");
        }
        else
        {
            WARN("Anisotropic filtering not supported.\n");
        }
        gl_tex->states[WINED3DTEXSTA_MAXANISOTROPY] = aniso;
    }

    /* These should always be the same unless EXT_texture_sRGB_decode is supported. */
    if (sampler_states[WINED3D_SAMP_SRGB_TEXTURE] != gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE])
    {
        gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_SRGB_DECODE_EXT,
                sampler_states[WINED3D_SAMP_SRGB_TEXTURE] ? GL_DECODE_EXT : GL_SKIP_DECODE_EXT);
        checkGLcall("glTexParameteri(GL_TEXTURE_SRGB_DECODE_EXT)");
        gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = sampler_states[WINED3D_SAMP_SRGB_TEXTURE];
    }

    if (!(texture->resource.format->flags & WINED3DFMT_FLAG_SHADOW)
            != !gl_tex->states[WINED3DTEXSTA_SHADOW])
    {
        if (texture->resource.format->flags & WINED3DFMT_FLAG_SHADOW)
        {
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_DEPTH_TEXTURE_MODE_ARB, GL_LUMINANCE);
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE_ARB);
            checkGLcall("glTexParameteri(target, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE_ARB)");
            gl_tex->states[WINED3DTEXSTA_SHADOW] = TRUE;
        }
        else
        {
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_COMPARE_MODE_ARB, GL_NONE);
            checkGLcall("glTexParameteri(target, GL_TEXTURE_COMPARE_MODE_ARB, GL_NONE)");
            gl_tex->states[WINED3DTEXSTA_SHADOW] = FALSE;
        }
    }
}

ULONG CDECL wined3d_texture_incref(struct wined3d_texture *texture)
{
    ULONG refcount = InterlockedIncrement(&texture->resource.ref);

    TRACE("%p increasing refcount to %u.\n", texture, refcount);

    return refcount;
}

/* Do not call while under the GL lock. */
ULONG CDECL wined3d_texture_decref(struct wined3d_texture *texture)
{
    ULONG refcount = InterlockedDecrement(&texture->resource.ref);

    TRACE("%p decreasing refcount to %u.\n", texture, refcount);

    if (!refcount)
    {
        wined3d_texture_cleanup(texture);
        texture->resource.parent_ops->wined3d_object_destroyed(texture->resource.parent);
        HeapFree(GetProcessHeap(), 0, texture);
    }

    return refcount;
}

struct wined3d_resource * CDECL wined3d_texture_get_resource(struct wined3d_texture *texture)
{
    TRACE("texture %p.\n", texture);

    return &texture->resource;
}

DWORD CDECL wined3d_texture_set_priority(struct wined3d_texture *texture, DWORD priority)
{
    return resource_set_priority(&texture->resource, priority);
}

DWORD CDECL wined3d_texture_get_priority(const struct wined3d_texture *texture)
{
    return resource_get_priority(&texture->resource);
}

/* Do not call while under the GL lock. */
void CDECL wined3d_texture_preload(struct wined3d_texture *texture)
{
    texture->texture_ops->texture_preload(texture, SRGB_ANY);
}

void * CDECL wined3d_texture_get_parent(const struct wined3d_texture *texture)
{
    TRACE("texture %p.\n", texture);

    return texture->resource.parent;
}

DWORD CDECL wined3d_texture_set_lod(struct wined3d_texture *texture, DWORD lod)
{
    DWORD old = texture->lod;

    TRACE("texture %p, lod %u.\n", texture, lod);

    /* The d3d9:texture test shows that SetLOD is ignored on non-managed
     * textures. The call always returns 0, and GetLOD always returns 0. */
    if (texture->resource.pool != WINED3D_POOL_MANAGED)
    {
        TRACE("Ignoring SetLOD on %s texture, returning 0.\n", debug_d3dpool(texture->resource.pool));
        return 0;
    }

    if (lod >= texture->level_count)
        lod = texture->level_count - 1;

    if (texture->lod != lod)
    {
        texture->lod = lod;

        texture->texture_rgb.states[WINED3DTEXSTA_MAXMIPLEVEL] = ~0U;
        texture->texture_srgb.states[WINED3DTEXSTA_MAXMIPLEVEL] = ~0U;
        if (texture->resource.bind_count)
            device_invalidate_state(texture->resource.device, STATE_SAMPLER(texture->sampler));
    }

    return old;
}

DWORD CDECL wined3d_texture_get_lod(const struct wined3d_texture *texture)
{
    TRACE("texture %p, returning %u.\n", texture, texture->lod);

    return texture->lod;
}

DWORD CDECL wined3d_texture_get_level_count(const struct wined3d_texture *texture)
{
    TRACE("texture %p, returning %u.\n", texture, texture->level_count);

    return texture->level_count;
}

HRESULT CDECL wined3d_texture_set_autogen_filter_type(struct wined3d_texture *texture,
        enum wined3d_texture_filter_type filter_type)
{
    FIXME("texture %p, filter_type %s stub!\n", texture, debug_d3dtexturefiltertype(filter_type));

    if (!(texture->resource.usage & WINED3DUSAGE_AUTOGENMIPMAP))
    {
        WARN("Texture doesn't have AUTOGENMIPMAP usage.\n");
        return WINED3DERR_INVALIDCALL;
    }

    texture->filter_type = filter_type;

    return WINED3D_OK;
}

enum wined3d_texture_filter_type CDECL wined3d_texture_get_autogen_filter_type(const struct wined3d_texture *texture)
{
    TRACE("texture %p.\n", texture);

    return texture->filter_type;
}

void CDECL wined3d_texture_generate_mipmaps(struct wined3d_texture *texture)
{
    /* TODO: Implement filters using GL_SGI_generate_mipmaps. */
    FIXME("texture %p stub!\n", texture);
}

struct wined3d_resource * CDECL wined3d_texture_get_sub_resource(struct wined3d_texture *texture,
        UINT sub_resource_idx)
{
    UINT sub_count = texture->level_count * texture->layer_count;

    TRACE("texture %p, sub_resource_idx %u.\n", texture, sub_resource_idx);

    if (sub_resource_idx >= sub_count)
    {
        WARN("sub_resource_idx %u >= sub_count %u.\n", sub_resource_idx, sub_count);
        return NULL;
    }

    return texture->sub_resources[sub_resource_idx];
}

HRESULT CDECL wined3d_texture_add_dirty_region(struct wined3d_texture *texture,
        UINT layer, const struct wined3d_box *dirty_region)
{
    struct wined3d_resource *sub_resource;

    TRACE("texture %p, layer %u, dirty_region %p.\n", texture, layer, dirty_region);

    if (!(sub_resource = wined3d_texture_get_sub_resource(texture, layer * texture->level_count)))
    {
        WARN("Failed to get sub-resource.\n");
        return WINED3DERR_INVALIDCALL;
    }

    wined3d_texture_set_dirty(texture, TRUE);
    texture->texture_ops->texture_sub_resource_add_dirty_region(sub_resource, dirty_region);

    return WINED3D_OK;
}

/* Context activation is done by the caller. */
static HRESULT texture2d_bind(struct wined3d_texture *texture,
        struct wined3d_context *context, BOOL srgb)
{
    const struct wined3d_gl_info *gl_info = context->gl_info;
    BOOL set_gl_texture_desc;
    HRESULT hr;

    TRACE("texture %p, context %p, srgb %#x.\n", texture, context, srgb);

    hr = wined3d_texture_bind(texture, context, srgb, &set_gl_texture_desc);
    if (set_gl_texture_desc && SUCCEEDED(hr))
    {
        UINT sub_count = texture->level_count * texture->layer_count;
        BOOL srgb_tex = !context->gl_info->supported[EXT_TEXTURE_SRGB_DECODE]
                && (texture->flags & WINED3D_TEXTURE_IS_SRGB);
        struct gl_texture *gl_tex;
        UINT i;

        gl_tex = wined3d_texture_get_gl_texture(texture, context->gl_info, srgb_tex);

        for (i = 0; i < sub_count; ++i)
        {
            struct wined3d_surface *surface = surface_from_resource(texture->sub_resources[i]);
            surface_set_texture_name(surface, gl_tex->name, srgb_tex);
        }

        /* Conditinal non power of two textures use a different clamping
         * default. If we're using the GL_WINE_normalized_texrect partial
         * driver emulation, we're dealing with a GL_TEXTURE_2D texture which
         * has the address mode set to repeat - something that prevents us
         * from hitting the accelerated codepath. Thus manually set the GL
         * state. The same applies to filtering. Even if the texture has only
         * one mip level, the default LINEAR_MIPMAP_LINEAR filter causes a SW
         * fallback on macos. */
        if (texture->flags & WINED3D_TEXTURE_COND_NP2)
        {
#ifdef VBOX_WITH_WDDM
            Assert(!VBOXSHRC_IS_SHARED_OPENED(texture));
            if (!VBOXSHRC_IS_SHARED_OPENED(texture))
#endif
            {
            GLenum target = texture->target;

            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            checkGLcall("glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)");
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            checkGLcall("glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)");
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            checkGLcall("glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST)");
            gl_info->gl_ops.gl.p_glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            checkGLcall("glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST)");
            }
            gl_tex->states[WINED3DTEXSTA_ADDRESSU] = WINED3D_TADDRESS_CLAMP;
            gl_tex->states[WINED3DTEXSTA_ADDRESSV] = WINED3D_TADDRESS_CLAMP;
            gl_tex->states[WINED3DTEXSTA_MAGFILTER] = WINED3D_TEXF_POINT;
            gl_tex->states[WINED3DTEXSTA_MINFILTER] = WINED3D_TEXF_POINT;
            gl_tex->states[WINED3DTEXSTA_MIPFILTER] = WINED3D_TEXF_NONE;
        }
    }

    return hr;
}

static BOOL texture_srgb_mode(const struct wined3d_texture *texture, enum WINED3DSRGB srgb)
{
    switch (srgb)
    {
        case SRGB_RGB:
            return FALSE;

        case SRGB_SRGB:
            return TRUE;

        default:
            return texture->flags & WINED3D_TEXTURE_IS_SRGB;
    }
}

/* Do not call while under the GL lock. */
static void texture2d_preload(struct wined3d_texture *texture, enum WINED3DSRGB srgb)
{
    UINT sub_count = texture->level_count * texture->layer_count;
    struct wined3d_device *device = texture->resource.device;
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    struct wined3d_context *context = NULL;
    struct gl_texture *gl_tex;
    BOOL srgb_mode;
    UINT i;

    TRACE("texture %p, srgb %#x.\n", texture, srgb);

    srgb_mode = texture_srgb_mode(texture, srgb);
    gl_tex = wined3d_texture_get_gl_texture(texture, gl_info, srgb_mode);

    if (!device->isInDraw)
    {
        /* No danger of recursive calls, context_acquire() sets isInDraw to TRUE
         * when loading offscreen render targets into the texture. */
        context = context_acquire(device, NULL);
    }

    if (gl_tex->dirty)
    {
        /* Reload the surfaces if the texture is marked dirty. */
        for (i = 0; i < sub_count; ++i)
        {
            surface_load(surface_from_resource(texture->sub_resources[i]), srgb_mode);
        }
    }
    else
    {
        TRACE("Texture %p not dirty, nothing to do.\n", texture);
    }

    /* No longer dirty. */
    gl_tex->dirty = FALSE;

    if (context) context_release(context);
}

static void texture2d_sub_resource_add_dirty_region(struct wined3d_resource *sub_resource,
        const struct wined3d_box *dirty_region)
{
    surface_add_dirty_rect(surface_from_resource(sub_resource), dirty_region);
}

static void texture2d_sub_resource_cleanup(struct wined3d_resource *sub_resource)
{
    struct wined3d_surface *surface = surface_from_resource(sub_resource);

    /* Clean out the texture name we gave to the surface so that the
     * surface doesn't try and release it. */
    surface_set_texture_name(surface, 0, TRUE);
    surface_set_texture_name(surface, 0, FALSE);
    surface_set_texture_target(surface, 0, 0);
    surface_set_container(surface, NULL);
    wined3d_surface_decref(surface);
}

/* Do not call while under the GL lock. */
static void texture2d_unload(struct wined3d_resource *resource)
{
    struct wined3d_texture *texture = wined3d_texture_from_resource(resource);
    UINT sub_count = texture->level_count * texture->layer_count;
    UINT i;

    TRACE("texture %p.\n", texture);

    for (i = 0; i < sub_count; ++i)
    {
        struct wined3d_resource *sub_resource = texture->sub_resources[i];
        struct wined3d_surface *surface = surface_from_resource(sub_resource);

        sub_resource->resource_ops->resource_unload(sub_resource);
        surface_set_texture_name(surface, 0, FALSE); /* Delete RGB name */
        surface_set_texture_name(surface, 0, TRUE); /* Delete sRGB name */
    }

    wined3d_texture_unload(texture);
}

static const struct wined3d_texture_ops texture2d_ops =
{
    texture2d_bind,
    texture2d_preload,
    texture2d_sub_resource_add_dirty_region,
    texture2d_sub_resource_cleanup,
};

static const struct wined3d_resource_ops texture2d_resource_ops =
{
    texture2d_unload,
};

static HRESULT cubetexture_init(struct wined3d_texture *texture, const struct wined3d_resource_desc *desc,
        UINT levels, DWORD surface_flags, struct wined3d_device *device, void *parent,
        const struct wined3d_parent_ops *parent_ops
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
        )
{
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    struct wined3d_resource_desc surface_desc;
    unsigned int i, j;
    HRESULT hr;

    /* TODO: It should only be possible to create textures for formats
     * that are reported as supported. */
    if (WINED3DFMT_UNKNOWN >= desc->format)
    {
        WARN("(%p) : Texture cannot be created with a format of WINED3DFMT_UNKNOWN.\n", texture);
        return WINED3DERR_INVALIDCALL;
    }

    if (!gl_info->supported[ARB_TEXTURE_CUBE_MAP] && desc->pool != WINED3D_POOL_SCRATCH)
    {
        WARN("(%p) : Tried to create not supported cube texture.\n", texture);
        return WINED3DERR_INVALIDCALL;
    }

    /* Calculate levels for mip mapping */
    if (desc->usage & WINED3DUSAGE_AUTOGENMIPMAP)
    {
        if (!gl_info->supported[SGIS_GENERATE_MIPMAP])
        {
            WARN("No mipmap generation support, returning D3DERR_INVALIDCALL.\n");
            return WINED3DERR_INVALIDCALL;
        }

        if (levels > 1)
        {
            WARN("D3DUSAGE_AUTOGENMIPMAP is set, and level count > 1, returning D3DERR_INVALIDCALL.\n");
            return WINED3DERR_INVALIDCALL;
        }

        levels = 1;
    }
    else if (!levels)
    {
        levels = wined3d_log2i(desc->width) + 1;
        TRACE("Calculated levels = %u.\n", levels);
    }

    if (!gl_info->supported[ARB_TEXTURE_NON_POWER_OF_TWO])
    {
        UINT pow2_edge_length = 1;
        while (pow2_edge_length < desc->width)
            pow2_edge_length <<= 1;

        if (desc->width != pow2_edge_length)
        {
            if (desc->pool == WINED3D_POOL_SCRATCH)
            {
                /* SCRATCH textures cannot be used for texturing */
                WARN("Creating a scratch NPOT cube texture despite lack of HW support.\n");
            }
            else
            {
                WARN("Attempted to create a NPOT cube texture (edge length %u) without GL support.\n", desc->width);
                return WINED3DERR_INVALIDCALL;
            }
        }
    }

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = wined3d_texture_init(texture, &texture2d_ops, 6, levels,
            desc, device, parent, parent_ops, &texture2d_resource_ops,
            shared_handle, pavClientMem)))
#else
    if (FAILED(hr = wined3d_texture_init(texture, &texture2d_ops, 6, levels,
            desc, device, parent, parent_ops, &texture2d_resource_ops)))
#endif
    {
        WARN("Failed to initialize texture, returning %#x\n", hr);
        return hr;
    }

        texture->pow2_matrix[0] = 1.0f;
        texture->pow2_matrix[5] = 1.0f;
        texture->pow2_matrix[10] = 1.0f;
        texture->pow2_matrix[15] = 1.0f;
    texture->target = GL_TEXTURE_CUBE_MAP_ARB;

    /* Generate all the surfaces. */
    surface_desc = *desc;
    surface_desc.resource_type = WINED3D_RTYPE_SURFACE;
    for (i = 0; i < texture->level_count; ++i)
    {
        /* Create the 6 faces. */
        for (j = 0; j < 6; ++j)
        {
            static const GLenum cube_targets[6] =
            {
                GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB,
                GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB,
                GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB,
                GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB,
                GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB,
                GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB,
            };
            UINT idx = j * texture->level_count + i;
            struct wined3d_surface *surface;


#ifdef VBOX_WITH_WDDM
            if (FAILED(hr = device->device_parent->ops->create_texture_surface(device->device_parent,
                    parent, &surface_desc, idx, surface_flags, &surface
                    ,NULL, pavClientMem ? pavClientMem[i * 6 + j] : NULL
                    )))
#else
            if (FAILED(hr = device->device_parent->ops->create_texture_surface(device->device_parent,
                    parent, &surface_desc, idx, surface_flags, &surface)))
#endif
            {
                FIXME("(%p) Failed to create surface, hr %#x.\n", texture, hr);
                wined3d_texture_cleanup(texture);
                return hr;
            }

            surface_set_container(surface, texture);
            surface_set_texture_target(surface, cube_targets[j], i);
            texture->sub_resources[idx] = &surface->resource;
            TRACE("Created surface level %u @ %p.\n", i, surface);
        }
        surface_desc.width = max(1, surface_desc.width >> 1);
        surface_desc.height = surface_desc.width;
    }

#ifdef VBOX_WITH_WDDM
    if (VBOXSHRC_IS_SHARED(texture))
    {
        Assert(shared_handle);
        for (i = 0; i < texture->level_count; ++i)
        {
            for (j = 0; j < 6; ++j)
            {
                UINT idx = j * texture->level_count + i;
                VBOXSHRC_COPY_SHAREDATA(surface_from_resource(texture->sub_resources[idx]), texture);
            }
        }
#ifdef DEBUG
        for (i = 0; i < texture->level_count; ++i)
        {
            for (j = 0; j < 6; ++j)
            {
                UINT idx = j * texture->level_count + i;
                Assert(!surface_from_resource(texture->sub_resources[idx])->texture_name);
            }
        }
#endif

        if (!VBOXSHRC_IS_SHARED_OPENED(texture))
        {
            for (i = 0; i < texture->level_count; ++i)
            {
                for (j = 0; j < 6; ++j)
                {
                    UINT idx = j * texture->level_count + i;
                    struct wined3d_surface *surface = surface_from_resource(texture->sub_resources[idx]);
                    surface_load_location(surface, SFLAG_INTEXTURE, NULL);
                }
            }

            Assert(!(*shared_handle));
            *shared_handle = VBOXSHRC_GET_SHAREHANDLE(texture);
            pglChromiumParameteriCR(GL_PIN_TEXTURE_SET_CR, (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture));
        }
        else
        {
            struct wined3d_context *context = NULL;
            const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
            struct gl_texture *gl_tex = wined3d_texture_get_gl_texture(texture, gl_info, FALSE);
            texture->texture_rgb.name = (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture);
            gl_tex->states[WINED3DTEXSTA_ADDRESSU] = WINED3D_TADDRESS_WRAP;
            gl_tex->states[WINED3DTEXSTA_ADDRESSV] = WINED3D_TADDRESS_WRAP;
            gl_tex->states[WINED3DTEXSTA_ADDRESSW] = WINED3D_TADDRESS_WRAP;
            gl_tex->states[WINED3DTEXSTA_BORDERCOLOR] = 0;
            gl_tex->states[WINED3DTEXSTA_MAGFILTER] = WINED3D_TEXF_LINEAR;
            gl_tex->states[WINED3DTEXSTA_MINFILTER] = WINED3D_TEXF_POINT; /* GL_NEAREST_MIPMAP_LINEAR */
            gl_tex->states[WINED3DTEXSTA_MIPFILTER] = WINED3D_TEXF_LINEAR; /* GL_NEAREST_MIPMAP_LINEAR */
            gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL] = 0;
            gl_tex->states[WINED3DTEXSTA_MAXANISOTROPY] = 1;
            if (gl_info->supported[EXT_TEXTURE_SRGB_DECODE])
                gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = TRUE;
            else
                gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = FALSE;
            gl_tex->states[WINED3DTEXSTA_SHADOW] = FALSE;
            wined3d_texture_set_dirty(texture, TRUE);

            context = context_acquire(device, NULL);
            pglChromiumParameteriCR(GL_RCUSAGE_TEXTURE_SET_CR, (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture));
            context_release(context);

            for (i = 0; i < texture->level_count; ++i)
            {
                for (j = 0; j < 6; ++j)
                {
                    UINT idx = j * texture->level_count + i;
                    struct wined3d_surface *surface = surface_from_resource(texture->sub_resources[idx]);
                    surface_setup_location_onopen(surface);
                    Assert(*shared_handle);
                    Assert(*shared_handle == VBOXSHRC_GET_SHAREHANDLE(texture));
                }
            }
        }
#ifdef DEBUG
        for (i = 0; i < texture->level_count; ++i)
        {
            for (j = 0; j < 6; ++j)
            {
                UINT idx = j * texture->level_count + i;
                Assert((GLuint)(*shared_handle) == surface_from_resource(texture->sub_resources[idx])->texture_name);
            }
        }
#endif

#ifdef DEBUG
        for (i = 0; i < texture->level_count; ++i)
        {
            for (j = 0; j < 6; ++j)
            {
                UINT idx = j * texture->level_count + i;
                Assert((GLuint)(*shared_handle) == surface_from_resource(texture->sub_resources[idx])->texture_name);
            }
        }
#endif

        Assert(!device->isInDraw);

        /* flush to ensure the texture is allocated/referenced before it is used/released by another
         * process opening/creating it */
        Assert(device->context_count == 1);
        pVBoxFlushToHost(device->contexts[0]->glCtx);
    }
    else
    {
        Assert(!shared_handle);
    }
#endif

    return WINED3D_OK;
}

static HRESULT texture_init(struct wined3d_texture *texture, const struct wined3d_resource_desc *desc,
        UINT levels, DWORD surface_flags, struct wined3d_device *device, void *parent,
        const struct wined3d_parent_ops *parent_ops
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
        )
{
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    struct wined3d_resource_desc surface_desc;
    UINT pow2_width, pow2_height;
    unsigned int i;
    HRESULT hr;

    /* TODO: It should only be possible to create textures for formats
     * that are reported as supported. */
    if (WINED3DFMT_UNKNOWN >= desc->format)
    {
        WARN("(%p) : Texture cannot be created with a format of WINED3DFMT_UNKNOWN.\n", texture);
        return WINED3DERR_INVALIDCALL;
    }

    /* Non-power2 support. */
    if (gl_info->supported[ARB_TEXTURE_NON_POWER_OF_TWO])
    {
        pow2_width = desc->width;
        pow2_height = desc->height;
    }
    else
    {
        /* Find the nearest pow2 match. */
        pow2_width = pow2_height = 1;
        while (pow2_width < desc->width)
            pow2_width <<= 1;
        while (pow2_height < desc->height)
            pow2_height <<= 1;

        if (pow2_width != desc->width || pow2_height != desc->height)
        {
            /* levels == 0 returns an error as well */
            if (levels != 1)
            {
                if (desc->pool == WINED3D_POOL_SCRATCH)
                {
                    WARN("Creating a scratch mipmapped NPOT texture despite lack of HW support.\n");
                }
                else
                {
                    WARN("Attempted to create a mipmapped NPOT texture without unconditional NPOT support.\n");
                return WINED3DERR_INVALIDCALL;
            }
            }
        }
    }

    /* Calculate levels for mip mapping. */
    if (desc->usage & WINED3DUSAGE_AUTOGENMIPMAP)
    {
        if (!gl_info->supported[SGIS_GENERATE_MIPMAP])
        {
            WARN("No mipmap generation support, returning WINED3DERR_INVALIDCALL.\n");
            return WINED3DERR_INVALIDCALL;
        }

        if (levels > 1)
        {
            WARN("D3DUSAGE_AUTOGENMIPMAP is set, and level count > 1, returning WINED3DERR_INVALIDCALL.\n");
            return WINED3DERR_INVALIDCALL;
        }

        levels = 1;
    }
    else if (!levels)
    {
        levels = wined3d_log2i(max(desc->width, desc->height)) + 1;
        TRACE("Calculated levels = %u.\n", levels);
    }

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = wined3d_texture_init(texture, &texture2d_ops, 1, levels,
            desc, device, parent, parent_ops, &texture2d_resource_ops,
            shared_handle, pavClientMem)))
#else
    if (FAILED(hr = wined3d_texture_init(texture, &texture2d_ops, 1, levels,
            desc, device, parent, parent_ops, &texture2d_resource_ops)))
#endif
    {
        WARN("Failed to initialize texture, returning %#x.\n", hr);
        return hr;
    }

    /* Precalculated scaling for 'faked' non power of two texture coords.
     * Second also don't use ARB_TEXTURE_RECTANGLE in case the surface format is P8 and EXT_PALETTED_TEXTURE
     * is used in combination with texture uploads (RTL_READTEX). The reason is that EXT_PALETTED_TEXTURE
     * doesn't work in combination with ARB_TEXTURE_RECTANGLE. */
    if (gl_info->supported[WINED3D_GL_NORMALIZED_TEXRECT]
            && (desc->width != pow2_width || desc->height != pow2_height))
    {
        texture->pow2_matrix[0] = 1.0f;
        texture->pow2_matrix[5] = 1.0f;
        texture->pow2_matrix[10] = 1.0f;
        texture->pow2_matrix[15] = 1.0f;
        texture->target = GL_TEXTURE_2D;
        texture->flags |= WINED3D_TEXTURE_COND_NP2;
        texture->min_mip_lookup = minMipLookup_noFilter;
    }
    else if (gl_info->supported[ARB_TEXTURE_RECTANGLE] && (desc->width != pow2_width || desc->height != pow2_height)
            && !(desc->format == WINED3DFMT_P8_UINT && gl_info->supported[EXT_PALETTED_TEXTURE]
            && wined3d_settings.rendertargetlock_mode == RTL_READTEX))
    {
        texture->pow2_matrix[0] = (float)desc->width;
        texture->pow2_matrix[5] = (float)desc->height;
        texture->pow2_matrix[10] = 1.0f;
        texture->pow2_matrix[15] = 1.0f;
        texture->target = GL_TEXTURE_RECTANGLE_ARB;
        texture->flags |= WINED3D_TEXTURE_COND_NP2;
        texture->flags &= ~WINED3D_TEXTURE_POW2_MAT_IDENT;

        if (texture->resource.format->flags & WINED3DFMT_FLAG_FILTERING)
            texture->min_mip_lookup = minMipLookup_noMip;
        else
            texture->min_mip_lookup = minMipLookup_noFilter;
    }
    else
    {
        if ((desc->width != pow2_width) || (desc->height != pow2_height))
        {
            texture->pow2_matrix[0] = (((float)desc->width) / ((float)pow2_width));
            texture->pow2_matrix[5] = (((float)desc->height) / ((float)pow2_height));
            texture->flags &= ~WINED3D_TEXTURE_POW2_MAT_IDENT;
        }
        else
        {
            texture->pow2_matrix[0] = 1.0f;
            texture->pow2_matrix[5] = 1.0f;
        }

        texture->pow2_matrix[10] = 1.0f;
        texture->pow2_matrix[15] = 1.0f;
        texture->target = GL_TEXTURE_2D;
    }
    TRACE("xf(%f) yf(%f)\n", texture->pow2_matrix[0], texture->pow2_matrix[5]);

    /* Generate all the surfaces. */
    surface_desc = *desc;
    surface_desc.resource_type = WINED3D_RTYPE_SURFACE;
    for (i = 0; i < texture->level_count; ++i)
    {
        struct wined3d_surface *surface;

        /* Use the callback to create the texture surface. */
#ifdef VBOX_WITH_WDDM
        if (FAILED(hr = device->device_parent->ops->create_texture_surface(device->device_parent,
                parent, &surface_desc, i, surface_flags, &surface
                , NULL /* <- we first create a surface in an average "non-shared" fashion and initialize its share properties later (see below)
                     * this is done this way because the surface does not have its parent (texture) setup properly
                     * thus we can not initialize texture at this stage */
                , pavClientMem ? pavClientMem[i] : NULL
                )))
#else
        if (FAILED(hr = device->device_parent->ops->create_texture_surface(device->device_parent,
                parent, &surface_desc, i, surface_flags, &surface)))
#endif
        {
            FIXME("Failed to create surface %p, hr %#x\n", texture, hr);
            wined3d_texture_cleanup(texture);
            return hr;
        }

        surface_set_container(surface, texture);
        surface_set_texture_target(surface, texture->target, i);
        texture->sub_resources[i] = &surface->resource;
        TRACE("Created surface level %u @ %p.\n", i, surface);
        /* Calculate the next mipmap level. */
        surface_desc.width = max(1, surface_desc.width >> 1);
        surface_desc.height = max(1, surface_desc.height >> 1);
    }

#ifdef VBOX_WITH_WDDM
    if (VBOXSHRC_IS_SHARED(texture))
    {
        Assert(shared_handle);
        for (i = 0; i < texture->level_count; ++i)
        {
            VBOXSHRC_COPY_SHAREDATA(surface_from_resource(texture->sub_resources[i]), texture);
        }
#ifdef DEBUG
        for (i = 0; i < texture->level_count; ++i)
        {
            Assert(!surface_from_resource(texture->sub_resources[i])->texture_name);
        }
#endif
        if (!VBOXSHRC_IS_SHARED_OPENED(texture))
        {
            for (i = 0; i < texture->level_count; ++i)
            {
                struct wined3d_surface *surface = surface_from_resource(texture->sub_resources[i]);
                surface_load_location(surface, SFLAG_INTEXTURE, NULL);
            }

            Assert(!(*shared_handle));
            *shared_handle = VBOXSHRC_GET_SHAREHANDLE(texture);
            pglChromiumParameteriCR(GL_PIN_TEXTURE_SET_CR, (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture));
        }
        else
        {
            struct wined3d_context *context = NULL;
            const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
            struct gl_texture *gl_tex = wined3d_texture_get_gl_texture(texture, gl_info, FALSE);
            texture->texture_rgb.name = (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture);
            gl_tex->states[WINED3DTEXSTA_ADDRESSU] = WINED3D_TADDRESS_WRAP;
            gl_tex->states[WINED3DTEXSTA_ADDRESSV] = WINED3D_TADDRESS_WRAP;
            gl_tex->states[WINED3DTEXSTA_ADDRESSW] = WINED3D_TADDRESS_WRAP;
            gl_tex->states[WINED3DTEXSTA_BORDERCOLOR] = 0;
            gl_tex->states[WINED3DTEXSTA_MAGFILTER] = WINED3D_TEXF_LINEAR;
            gl_tex->states[WINED3DTEXSTA_MINFILTER] = WINED3D_TEXF_POINT; /* GL_NEAREST_MIPMAP_LINEAR */
            gl_tex->states[WINED3DTEXSTA_MIPFILTER] = WINED3D_TEXF_LINEAR; /* GL_NEAREST_MIPMAP_LINEAR */
            gl_tex->states[WINED3DTEXSTA_MAXMIPLEVEL] = 0;
            gl_tex->states[WINED3DTEXSTA_MAXANISOTROPY] = 1;
            if (gl_info->supported[EXT_TEXTURE_SRGB_DECODE])
                gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = TRUE;
            else
                gl_tex->states[WINED3DTEXSTA_SRGBTEXTURE] = FALSE;
            gl_tex->states[WINED3DTEXSTA_SHADOW] = FALSE;
            wined3d_texture_set_dirty(texture, TRUE);
            if (texture->flags & WINED3D_TEXTURE_COND_NP2)
            {
                gl_tex->states[WINED3DTEXSTA_ADDRESSU] = WINED3D_TADDRESS_CLAMP;
                gl_tex->states[WINED3DTEXSTA_ADDRESSV] = WINED3D_TADDRESS_CLAMP;
                gl_tex->states[WINED3DTEXSTA_MAGFILTER] = WINED3D_TEXF_POINT;
                gl_tex->states[WINED3DTEXSTA_MINFILTER] = WINED3D_TEXF_POINT;
                gl_tex->states[WINED3DTEXSTA_MIPFILTER] = WINED3D_TEXF_NONE;
            }

            context = context_acquire(device, NULL);
            pglChromiumParameteriCR(GL_RCUSAGE_TEXTURE_SET_CR, (GLuint)VBOXSHRC_GET_SHAREHANDLE(texture));
            context_release(context);

            for (i = 0; i < texture->level_count; ++i)
            {
                struct wined3d_surface *surface = surface_from_resource(texture->sub_resources[i]);
                surface_setup_location_onopen(surface);
                Assert(*shared_handle);
                Assert(*shared_handle == VBOXSHRC_GET_SHAREHANDLE(texture));
            }
        }

#ifdef DEBUG
        for (i = 0; i < texture->level_count; ++i)
        {
            Assert((GLuint)(*shared_handle) == surface_from_resource(texture->sub_resources[i])->texture_name);
        }
#endif

        Assert(!device->isInDraw);

        /* flush to ensure the texture is allocated/referenced before it is used/released by another
         * process opening/creating it */
        Assert(device->context_count == 1);
        pVBoxFlushToHost(device->contexts[0]->glCtx);
    }
    else
    {
        Assert(!shared_handle);
    }
#endif

    return WINED3D_OK;
}

/* Context activation is done by the caller. */
static HRESULT texture3d_bind(struct wined3d_texture *texture,
        struct wined3d_context *context, BOOL srgb)
{
    BOOL dummy;

    TRACE("texture %p, context %p, srgb %#x.\n", texture, context, srgb);

    return wined3d_texture_bind(texture, context, srgb, &dummy);
}

/* Do not call while under the GL lock. */
static void texture3d_preload(struct wined3d_texture *texture, enum WINED3DSRGB srgb)
{
    struct wined3d_device *device = texture->resource.device;
    struct wined3d_context *context;
    BOOL srgb_was_toggled = FALSE;
    unsigned int i;

    TRACE("texture %p, srgb %#x.\n", texture, srgb);

    /* TODO: Use already acquired context when possible. */
    context = context_acquire(device, NULL);
    if (texture->resource.bind_count > 0)
    {
        BOOL texture_srgb = texture->flags & WINED3D_TEXTURE_IS_SRGB;
        BOOL sampler_srgb = texture_srgb_mode(texture, srgb);
        srgb_was_toggled = !texture_srgb != !sampler_srgb;

        if (srgb_was_toggled)
        {
            if (sampler_srgb)
                texture->flags |= WINED3D_TEXTURE_IS_SRGB;
            else
                texture->flags &= ~WINED3D_TEXTURE_IS_SRGB;
        }
    }

    /* If the texture is marked dirty or the sRGB sampler setting has changed
     * since the last load then reload the volumes. */
    if (texture->texture_rgb.dirty)
    {
        for (i = 0; i < texture->level_count; ++i)
        {
            volume_load(volume_from_resource(texture->sub_resources[i]), context, i,
                    texture->flags & WINED3D_TEXTURE_IS_SRGB);
        }
    }
    else if (srgb_was_toggled)
    {
        for (i = 0; i < texture->level_count; ++i)
        {
            struct wined3d_volume *volume = volume_from_resource(texture->sub_resources[i]);
            volume_add_dirty_box(volume, NULL);
            volume_load(volume, context, i, texture->flags & WINED3D_TEXTURE_IS_SRGB);
        }
    }
    else
    {
        TRACE("Texture %p not dirty, nothing to do.\n", texture);
    }

    context_release(context);

    /* No longer dirty */
    texture->texture_rgb.dirty = FALSE;
}

static void texture3d_sub_resource_add_dirty_region(struct wined3d_resource *sub_resource,
        const struct wined3d_box *dirty_region)
{
    volume_add_dirty_box(volume_from_resource(sub_resource), dirty_region);
}

static void texture3d_sub_resource_cleanup(struct wined3d_resource *sub_resource)
{
    struct wined3d_volume *volume = volume_from_resource(sub_resource);

    /* Cleanup the container. */
    volume_set_container(volume, NULL);
    wined3d_volume_decref(volume);
}

/* Do not call while under the GL lock. */
static void texture3d_unload(struct wined3d_resource *resource)
{
    struct wined3d_texture *texture = wined3d_texture_from_resource(resource);
    UINT i;

    TRACE("texture %p.\n", texture);

    for (i = 0; i < texture->level_count; ++i)
    {
        struct wined3d_resource *sub_resource = texture->sub_resources[i];
        sub_resource->resource_ops->resource_unload(sub_resource);
    }

    wined3d_texture_unload(texture);
}

static const struct wined3d_texture_ops texture3d_ops =
{
    texture3d_bind,
    texture3d_preload,
    texture3d_sub_resource_add_dirty_region,
    texture3d_sub_resource_cleanup,
};

static const struct wined3d_resource_ops texture3d_resource_ops =
{
    texture3d_unload,
};


static HRESULT volumetexture_init(struct wined3d_texture *texture, const struct wined3d_resource_desc *desc,
        UINT levels, struct wined3d_device *device, void *parent, const struct wined3d_parent_ops *parent_ops
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
        )
{
    const struct wined3d_gl_info *gl_info = &device->adapter->gl_info;
    UINT tmp_w, tmp_h, tmp_d;
    unsigned int i;
    HRESULT hr;

#ifdef VBOX_WITH_WDDM
    if (shared_handle)
    {
        ERR("shared handle support for volume textures not impemented yet, ignoring!");
    }
#endif

    /* TODO: It should only be possible to create textures for formats
     * that are reported as supported. */
    if (WINED3DFMT_UNKNOWN >= desc->format)
    {
        WARN("(%p) : Texture cannot be created with a format of WINED3DFMT_UNKNOWN.\n", texture);
        return WINED3DERR_INVALIDCALL;
    }

    if (!gl_info->supported[EXT_TEXTURE3D])
    {
        WARN("(%p) : Texture cannot be created - no volume texture support.\n", texture);
        return WINED3DERR_INVALIDCALL;
    }

    /* Calculate levels for mip mapping. */
    if (desc->usage & WINED3DUSAGE_AUTOGENMIPMAP)
    {
        if (!gl_info->supported[SGIS_GENERATE_MIPMAP])
        {
            WARN("No mipmap generation support, returning D3DERR_INVALIDCALL.\n");
            return WINED3DERR_INVALIDCALL;
        }

        if (levels > 1)
        {
            WARN("D3DUSAGE_AUTOGENMIPMAP is set, and level count > 1, returning D3DERR_INVALIDCALL.\n");
            return WINED3DERR_INVALIDCALL;
        }

        levels = 1;
    }
    else if (!levels)
    {
        levels = wined3d_log2i(max(max(desc->width, desc->height), desc->depth)) + 1;
        TRACE("Calculated levels = %u.\n", levels);
    }

    if (!gl_info->supported[ARB_TEXTURE_NON_POWER_OF_TWO])
    {
        UINT pow2_w, pow2_h, pow2_d;
        pow2_w = 1;
        while (pow2_w < desc->width)
            pow2_w <<= 1;
        pow2_h = 1;
        while (pow2_h < desc->height)
            pow2_h <<= 1;
        pow2_d = 1;
        while (pow2_d < desc->depth)
            pow2_d <<= 1;

        if (pow2_w != desc->width || pow2_h != desc->height || pow2_d != desc->depth)
        {
            if (desc->pool == WINED3D_POOL_SCRATCH)
            {
                WARN("Creating a scratch NPOT volume texture despite lack of HW support.\n");
            }
            else
            {
                WARN("Attempted to create a NPOT volume texture (%u, %u, %u) without GL support.\n",
                        desc->width, desc->height, desc->depth);
                return WINED3DERR_INVALIDCALL;
            }
        }
    }

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = wined3d_texture_init(texture, &texture3d_ops, 1, levels,
            desc, device, parent, parent_ops, &texture3d_resource_ops
            , shared_handle, pavClientMem)))
#else
    if (FAILED(hr = wined3d_texture_init(texture, &texture3d_ops, 1, levels,
            desc, device, parent, parent_ops, &texture3d_resource_ops)))
#endif
    {
        WARN("Failed to initialize texture, returning %#x.\n", hr);
        return hr;
    }

    texture->pow2_matrix[0] = 1.0f;
    texture->pow2_matrix[5] = 1.0f;
    texture->pow2_matrix[10] = 1.0f;
    texture->pow2_matrix[15] = 1.0f;
    texture->target = GL_TEXTURE_3D;

    /* Generate all the surfaces. */
    tmp_w = desc->width;
    tmp_h = desc->height;
    tmp_d = desc->depth;

    for (i = 0; i < texture->level_count; ++i)
    {
        struct wined3d_volume *volume;

        /* Create the volume. */
        hr = device->device_parent->ops->create_volume(device->device_parent, parent,
                tmp_w, tmp_h, tmp_d, desc->format, desc->pool, desc->usage, &volume
#ifdef VBOX_WITH_WDDM
                , shared_handle
                , pavClientMem ? pavClientMem[i] : NULL
#endif
                );
        if (FAILED(hr))
        {
            ERR("Creating a volume for the volume texture failed, hr %#x.\n", hr);
            wined3d_texture_cleanup(texture);
            return hr;
        }

        /* Set its container to this texture. */
        volume_set_container(volume, texture);
        texture->sub_resources[i] = &volume->resource;

        /* Calculate the next mipmap level. */
        tmp_w = max(1, tmp_w >> 1);
        tmp_h = max(1, tmp_h >> 1);
        tmp_d = max(1, tmp_d >> 1);
    }

    return WINED3D_OK;
}

HRESULT CDECL wined3d_texture_create_2d(struct wined3d_device *device, const struct wined3d_resource_desc *desc,
        UINT level_count, DWORD surface_flags, void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_texture **texture
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
        )
{
    struct wined3d_texture *object;
    HRESULT hr;

    TRACE("device %p, desc %p, level_count %u, surface_flags %#x, parent %p, parent_ops %p, texture %p.\n",
            device, desc, level_count, surface_flags, parent, parent_ops, texture);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
    {
        *texture = NULL;
        return WINED3DERR_OUTOFVIDEOMEMORY;
    }

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = texture_init(object, desc, level_count, surface_flags, device, parent, parent_ops
            , shared_handle, pavClientMem)))
#else
    if (FAILED(hr = texture_init(object, desc, level_count, surface_flags, device, parent, parent_ops)))
#endif
    {
        WARN("Failed to initialize texture, returning %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        *texture = NULL;
        return hr;
    }

    TRACE("Created texture %p.\n", object);
    *texture = object;

    return WINED3D_OK;
}

HRESULT CDECL wined3d_texture_create_3d(struct wined3d_device *device, const struct wined3d_resource_desc *desc,
        UINT level_count, void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_texture **texture
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
		)
{
    struct wined3d_texture *object;
    HRESULT hr;

    TRACE("device %p, desc %p, level_count %u, parent %p, parent_ops %p, texture %p.\n",
            device, desc, level_count, parent, parent_ops, texture);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
    {
        *texture = NULL;
        return WINED3DERR_OUTOFVIDEOMEMORY;
    }

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = volumetexture_init(object, desc, level_count, device, parent, parent_ops
            , shared_handle, pavClientMem)))
#else
    if (FAILED(hr = volumetexture_init(object, desc, level_count, device, parent, parent_ops)))
#endif
    {
        WARN("Failed to initialize volumetexture, returning %#x\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        *texture = NULL;
        return hr;
    }

    TRACE("Created texture %p.\n", object);
    *texture = object;

    return WINED3D_OK;
}

HRESULT CDECL wined3d_texture_create_cube(struct wined3d_device *device, const struct wined3d_resource_desc *desc,
        UINT level_count, DWORD surface_flags, void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_texture **texture
#ifdef VBOX_WITH_WDDM
        , HANDLE *shared_handle
        , void **pavClientMem
#endif
        )
{
    struct wined3d_texture *object;
    HRESULT hr;

    TRACE("device %p, desc %p, level_count %u, surface_flags %#x, parent %p, parent_ops %p, texture %p.\n",
            device, desc, level_count, surface_flags, parent, parent_ops, texture);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
    {
        *texture = NULL;
        return WINED3DERR_OUTOFVIDEOMEMORY;
    }

#ifdef VBOX_WITH_WDDM
    if (FAILED(hr = cubetexture_init(object, desc, level_count, surface_flags, device, parent, parent_ops
            , shared_handle, pavClientMem)))
#else
    if (FAILED(hr = cubetexture_init(object, desc, level_count, surface_flags, device, parent, parent_ops)))
#endif
    {
        WARN("Failed to initialize cubetexture, returning %#x\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        *texture = NULL;
        return hr;
    }

    TRACE("Created texture %p.\n", object);
    *texture = object;

    return WINED3D_OK;
}

#ifdef VBOX_WITH_WDDM
HRESULT CDECL wined3d_device_blt_voltex(struct wined3d_device *device, struct wined3d_texture *src, struct wined3d_texture *dst,
        const struct wined3d_box *pSrcBoxArg, const VBOXPOINT3D *pDstPoin3D)
{
    unsigned int level_count, i;
    struct wined3d_volume *src_volume;
    struct wined3d_volume *dst_volume;
    HRESULT hr = S_OK;

    level_count = src->level_count;
    if (dst->level_count != level_count)
    {
        ERR("Source and destination have different level counts, returning WINED3DERR_INVALIDCALL.\n");
        return WINED3DERR_INVALIDCALL;
    }

    for (i = 0; i < level_count; ++i)
    {
        struct wined3d_box SrcBox, *pSrcBox;
        VBOXPOINT3D DstPoint, *pDstPoint;

        if (pSrcBoxArg)
        {
            vboxWddmBoxDivided((VBOXBOX3D*)&SrcBox, (VBOXBOX3D*)pSrcBoxArg, i + 1, true);
            pSrcBox = &SrcBox;
        }
        else
        {
            pSrcBox = NULL;
        }

        if (pDstPoin3D)
        {
            vboxWddmPoint3DDivided(&DstPoint, pDstPoin3D, i + 1, true);
            pDstPoint = &DstPoint;
        }
        else
        {
            pDstPoint = NULL;
        }

        src_volume = volume_from_resource(src->sub_resources[i]);
        dst_volume = volume_from_resource(dst->sub_resources[i]);
        hr = wined3d_device_blt_vol(device, src_volume, dst_volume, pSrcBox, pDstPoint);
        if (FAILED(hr))
        {
            ERR("wined3d_device_blt_vol failed, hr %#x.\n", hr);
            return hr;
        }
    }

    return hr;
}
#endif
