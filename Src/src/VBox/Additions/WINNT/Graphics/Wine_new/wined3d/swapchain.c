/*
 * Copyright 2002-2003 Jason Edmeades
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2005 Oliver Stieber
 * Copyright 2007-2008 Stefan Dösinger for CodeWeavers
 * Copyright 2011 Henri Verbeet for CodeWeavers
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

WINE_DEFAULT_DEBUG_CHANNEL(d3d);
WINE_DECLARE_DEBUG_CHANNEL(fps);

#ifdef VBOX_WINE_WITH_SINGLE_CONTEXT
static VOID swapchain_cleanup_rt_refs(struct wined3d_swapchain *swapchain, struct wined3d_surface * rt, int iBb)
{
    struct wined3d_device * device = swapchain->device;
    struct wined3d_context *context;
    UINT i;
    for (i = 0; i < device->context_count; ++i)
    {
        context = device->contexts[i];

        if (rt == context->current_rt)
        {
            context->current_rt = NULL;
        }
    }

    if (device->swapchain_count)
    {
        struct wined3d_swapchain *default_swapchain = (struct wined3d_swapchain*)device->swapchains[0];
        for (i = 0; i < device->adapter->gl_info.limits.buffers; ++i)
        {
            if (device->fb.render_targets[i] == rt)
            {
                struct wined3d_surface *new_rt;
                if (i)
                    new_rt = NULL;
                else if (iBb == -1) /* front buffer */
                    new_rt = default_swapchain->front_buffer;
                else
                    new_rt = default_swapchain->back_buffers ? default_swapchain->back_buffers[0] : default_swapchain->front_buffer;

                wined3d_device_set_render_target(device, i, new_rt, TRUE);
            }
        }
    }
}

static VOID swapchain_cleanup_refs(struct wined3d_swapchain *swapchain)
{
    /* first make sure the swapchain is not used by anyone */
    struct wined3d_device * device = swapchain->device;
    struct wined3d_context *context;
    UINT i;

    /* Release the swapchain's draw buffers. Make sure swapchain->back_buffers[0]
     * is the last buffer to be destroyed, FindContext() depends on that. */
    if (swapchain->front_buffer)
    {
        surface_set_swapchain(swapchain->front_buffer, NULL);
        if (wined3d_surface_decref(swapchain->front_buffer))
            WARN("Something's still holding the front buffer (%p).\n", swapchain->front_buffer);
        swapchain->front_buffer = NULL;
    }

    if (swapchain->back_buffers)
    {
        i = swapchain->desc.backbuffer_count;

        while (i--)
        {
            surface_set_swapchain(swapchain->back_buffers[i], NULL);
            if (wined3d_surface_decref(swapchain->back_buffers[i]))
                WARN("Something's still holding back buffer %u (%p).\n", i, swapchain->back_buffers[i]);
        }
        HeapFree(GetProcessHeap(), 0, swapchain->back_buffers);
        swapchain->back_buffers = NULL;
    }

    for (i = 0; i < device->context_count; ++i)
    {
        context = device->contexts[i];
        /* pretty hacky, @todo: check if the context is acquired and re-acquire it with a new swapchain */
        if (context->swapchain == swapchain)
        {
            context->swapchain = NULL;
        }
    }
//
//    if (swapchain->front_buffer)
//        swapchain_cleanup_rt_refs(swapchain, swapchain->front_buffer, -1);
//
//    if (swapchain->back_buffers)
//    {
//        for (i = 0; i < swapchain->desc.backbuffer_count; ++i)
//        {
//            swapchain_cleanup_rt_refs(swapchain, swapchain->back_buffers[i], i);
//        }
//    }
}
#endif

#ifdef VBOX_WITH_WDDM

struct wined3d_swapchain * swapchain_find(struct wined3d_device * device, HWND hWnd)
{
    UINT i;
    for (i = 0; i < device->swapchain_count; ++i)
    {
        struct wined3d_swapchain *swapchain = device->swapchains[i];
        if (swapchain->win_handle == hWnd)
        {
            return swapchain;
        }
    }

    return NULL;
}

static VOID swapchain_invalidate(struct wined3d_swapchain *swapchain)
{
    swapchain_cleanup_refs(swapchain);

    swapchain->win_handle = NULL;
    swapchain->hDC = NULL;
}
#endif

/* Do not call while under the GL lock. */
static void swapchain_cleanup(struct wined3d_swapchain *swapchain)
{
#ifndef VBOX_WITH_WDDM
    HRESULT hr;
#endif
    UINT i;

    TRACE("Destroying swapchain %p.\n", swapchain);

#ifndef VBOX_WITH_WDDM
    wined3d_swapchain_set_gamma_ramp(swapchain, 0, &swapchain->orig_gamma);
#endif

    /* Release the swapchain's draw buffers. Make sure swapchain->back_buffers[0]
     * is the last buffer to be destroyed, FindContext() depends on that. */
    if (swapchain->front_buffer)
    {
        surface_set_swapchain(swapchain->front_buffer, NULL);
        if (wined3d_surface_decref(swapchain->front_buffer))
            WARN("Something's still holding the front buffer (%p).\n", swapchain->front_buffer);
        swapchain->front_buffer = NULL;
    }

    if (swapchain->back_buffers)
    {
        i = swapchain->desc.backbuffer_count;

        while (i--)
        {
            surface_set_swapchain(swapchain->back_buffers[i], NULL);
            if (wined3d_surface_decref(swapchain->back_buffers[i]))
                WARN("Something's still holding back buffer %u (%p).\n", i, swapchain->back_buffers[i]);
        }
        HeapFree(GetProcessHeap(), 0, swapchain->back_buffers);
        swapchain->back_buffers = NULL;
    }

#ifdef VBOX_WINE_WITH_SINGLE_CONTEXT
    swapchain_cleanup_refs(swapchain);
#endif

#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
    for (i = 0; i < swapchain->num_contexts; ++i)
    {
        context_destroy(swapchain->device, swapchain->context[i]);
    }
    HeapFree(GetProcessHeap(), 0, swapchain->context);
#endif
#ifndef VBOX_WITH_WDDM
    /* Restore the screen resolution if we rendered in fullscreen.
     * This will restore the screen resolution to what it was before creating
     * the swapchain. In case of d3d8 and d3d9 this will be the original
     * desktop resolution. In case of d3d7 this will be a NOP because ddraw
     * sets the resolution before starting up Direct3D, thus orig_width and
     * orig_height will be equal to the modes in the presentation params. */
    if (!swapchain->desc.windowed && swapchain->desc.auto_restore_display_mode)
    {
        if (FAILED(hr = wined3d_set_adapter_display_mode(swapchain->device->wined3d,
                swapchain->device->adapter->ordinal, &swapchain->original_mode)))
            ERR("Failed to restore display mode, hr %#x.\n", hr);
    }

    if (swapchain->backup_dc)
    {
        TRACE("Destroying backup wined3d window %p, dc %p.\n", swapchain->backup_wnd, swapchain->backup_dc);

        wined3d_release_dc(swapchain->backup_wnd, swapchain->backup_dc);
        DestroyWindow(swapchain->backup_wnd);
    }
#endif
#ifdef VBOX_WINE_WITH_SINGLE_CONTEXT
# ifdef VBOX_WITH_WDDM
    if(swapchain->win_handle) {
        VBoxExtWndDestroy(swapchain->win_handle, swapchain->hDC);
        swapchain_invalidate(swapchain);
    }
# endif
#endif
}

ULONG CDECL wined3d_swapchain_incref(struct wined3d_swapchain *swapchain)
{
    ULONG refcount = InterlockedIncrement(&swapchain->ref);

    TRACE("%p increasing refcount to %u.\n", swapchain, refcount);

    return refcount;
}

/* Do not call while under the GL lock. */
ULONG CDECL wined3d_swapchain_decref(struct wined3d_swapchain *swapchain)
{
    ULONG refcount = InterlockedDecrement(&swapchain->ref);

    TRACE("%p decreasing refcount to %u.\n", swapchain, refcount);

    if (!refcount)
    {
        swapchain_cleanup(swapchain);
        swapchain->parent_ops->wined3d_object_destroyed(swapchain->parent);
        HeapFree(GetProcessHeap(), 0, swapchain);
    }

    return refcount;
}

void * CDECL wined3d_swapchain_get_parent(const struct wined3d_swapchain *swapchain)
{
    TRACE("swapchain %p.\n", swapchain);

    return swapchain->parent;
}

void CDECL wined3d_swapchain_set_window(struct wined3d_swapchain *swapchain, HWND window)
{
#ifdef VBOX_WITH_WDDM
    if (window)
        ERR("not expected!");
#else
    if (!window)
        window = swapchain->device_window;
    if (window == swapchain->win_handle)
        return;

    TRACE("Setting swapchain %p window from %p to %p.\n",
            swapchain, swapchain->win_handle, window);
    swapchain->win_handle = window;
#endif
}

HRESULT CDECL wined3d_swapchain_present(struct wined3d_swapchain *swapchain,
        const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override,
        const RGNDATA *dirty_region, DWORD flags)
{
    TRACE("swapchain %p, src_rect %s, dst_rect %s, dst_window_override %p, dirty_region %p, flags %#x.\n",
            swapchain, wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect),
            dst_window_override, dirty_region, flags);

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);

    if (!swapchain->back_buffers)
    {
        WARN("Swapchain doesn't have a backbuffer, returning WINED3DERR_INVALIDCALL\n");
        return WINED3DERR_INVALIDCALL;
    }

    wined3d_swapchain_set_window(swapchain, dst_window_override);

    swapchain->swapchain_ops->swapchain_present(swapchain, src_rect, dst_rect, dirty_region, flags);

    return WINED3D_OK;
}

HRESULT CDECL wined3d_swapchain_get_front_buffer_data(const struct wined3d_swapchain *swapchain,
        struct wined3d_surface *dst_surface)
{
    struct wined3d_surface *src_surface;
    RECT src_rect, dst_rect;

    TRACE("swapchain %p, dst_surface %p.\n", swapchain, dst_surface);

    src_surface = swapchain->front_buffer;
    SetRect(&src_rect, 0, 0, src_surface->resource.width, src_surface->resource.height);
    dst_rect = src_rect;

#ifndef VBOX_WITH_WINE_FIXES
    if (swapchain->desc.windowed)
    {
        MapWindowPoints(swapchain->win_handle, NULL, (POINT *)&dst_rect, 2);
        FIXME("Using destination rect %s in windowed mode, this is likely wrong.\n",
                wine_dbgstr_rect(&dst_rect));
    }
#endif

    return wined3d_surface_blt(dst_surface, &dst_rect, src_surface, &src_rect, 0, NULL, WINED3D_TEXF_POINT);
}

struct wined3d_surface * CDECL wined3d_swapchain_get_back_buffer(const struct wined3d_swapchain *swapchain,
        UINT back_buffer_idx, enum wined3d_backbuffer_type type)
{
    TRACE("swapchain %p, back_buffer_idx %u, type %#x.\n",
            swapchain, back_buffer_idx, type);

#ifdef VBOX_WITH_WDDM
    if (back_buffer_idx == ~0UL)
    {
        return swapchain->front_buffer;
    }
#endif

    /* Return invalid if there is no backbuffer array, otherwise it will
     * crash when ddraw is used (there swapchain->back_buffers is always
     * NULL). We need this because this function is called from
     * stateblock_init_default_state() to get the default scissorrect
     * dimensions. */
    if (!swapchain->back_buffers || back_buffer_idx >= swapchain->desc.backbuffer_count)
    {
        WARN("Invalid back buffer index.\n");
        /* Native d3d9 doesn't set NULL here, just as wine's d3d9. But set it
         * here in wined3d to avoid problems in other libs. */
        return NULL;
    }

    TRACE("Returning back buffer %p.\n", swapchain->back_buffers[back_buffer_idx]);

    return swapchain->back_buffers[back_buffer_idx];
}

HRESULT CDECL wined3d_swapchain_get_raster_status(const struct wined3d_swapchain *swapchain,
        struct wined3d_raster_status *raster_status)
{
    TRACE("swapchain %p, raster_status %p.\n", swapchain, raster_status);

    return wined3d_get_adapter_raster_status(swapchain->device->wined3d,
            swapchain->device->adapter->ordinal, raster_status);
}

HRESULT CDECL wined3d_swapchain_get_display_mode(const struct wined3d_swapchain *swapchain,
        struct wined3d_display_mode *mode, enum wined3d_display_rotation *rotation)
{
    HRESULT hr;

    TRACE("swapchain %p, mode %p, rotation %p.\n", swapchain, mode, rotation);

    hr = wined3d_get_adapter_display_mode(swapchain->device->wined3d,
            swapchain->device->adapter->ordinal, mode, rotation);

    TRACE("Returning w %u, h %u, refresh rate %u, format %s.\n",
            mode->width, mode->height, mode->refresh_rate, debug_d3dformat(mode->format_id));

    return hr;
}

struct wined3d_device * CDECL wined3d_swapchain_get_device(const struct wined3d_swapchain *swapchain)
{
    TRACE("swapchain %p.\n", swapchain);

    return swapchain->device;
}

void CDECL wined3d_swapchain_get_desc(const struct wined3d_swapchain *swapchain,
        struct wined3d_swapchain_desc *desc)
{
    TRACE("swapchain %p, desc %p.\n", swapchain, desc);

    *desc = swapchain->desc;
}

HRESULT CDECL wined3d_swapchain_set_gamma_ramp(const struct wined3d_swapchain *swapchain,
        DWORD flags, const struct wined3d_gamma_ramp *ramp)
{
    HDC dc;

    TRACE("swapchain %p, flags %#x, ramp %p.\n", swapchain, flags, ramp);

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);

#ifdef VBOX_WITH_WDDM
    dc = GetDC(swapchain->win_handle);
#else
    dc = GetDC(swapchain->device_window);
#endif
    SetDeviceGammaRamp(dc, (void *)ramp);
#ifdef VBOX_WITH_WDDM
    ReleaseDC(swapchain->win_handle, dc);
#else
    ReleaseDC(swapchain->device_window, dc);
#endif

    return WINED3D_OK;
}

HRESULT CDECL wined3d_swapchain_get_gamma_ramp(const struct wined3d_swapchain *swapchain,
        struct wined3d_gamma_ramp *ramp)
{
    HDC dc;

    TRACE("swapchain %p, ramp %p.\n", swapchain, ramp);

#ifdef VBOX_WITH_WDDM
    dc = GetDC(swapchain->win_handle);
#else
    dc = GetDC(swapchain->device_window);
#endif
    GetDeviceGammaRamp(dc, ramp);
#ifdef VBOX_WITH_WDDM
    ReleaseDC(swapchain->win_handle, dc);
#else
    ReleaseDC(swapchain->device_window, dc);
#endif

    return WINED3D_OK;
}

/* A GL context is provided by the caller */
static void swapchain_blit(const struct wined3d_swapchain *swapchain,
        struct wined3d_context *context, const RECT *src_rect, const RECT *dst_rect)
{
    struct wined3d_surface *backbuffer = swapchain->back_buffers[0];
    UINT src_w = src_rect->right - src_rect->left;
    UINT src_h = src_rect->bottom - src_rect->top;
    GLenum gl_filter;
    const struct wined3d_gl_info *gl_info = context->gl_info;
    RECT win_rect;
    UINT win_h;

    TRACE("swapchain %p, context %p, src_rect %s, dst_rect %s.\n",
            swapchain, context, wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect));

    if (src_w == dst_rect->right - dst_rect->left && src_h == dst_rect->bottom - dst_rect->top)
        gl_filter = GL_NEAREST;
    else
        gl_filter = GL_LINEAR;

    GetClientRect(swapchain->win_handle, &win_rect);
    win_h = win_rect.bottom - win_rect.top;

    if (gl_info->fbo_ops.glBlitFramebuffer && is_identity_fixup(backbuffer->resource.format->color_fixup))
    {
        DWORD location = SFLAG_INTEXTURE;

        if (backbuffer->resource.multisample_type)
        {
            location = SFLAG_INRB_RESOLVED;
            surface_load_location(backbuffer, location, NULL);
        }

        context_apply_fbo_state_blit(context, GL_READ_FRAMEBUFFER, backbuffer, NULL, location);
        gl_info->gl_ops.gl.p_glReadBuffer(GL_COLOR_ATTACHMENT0);
        context_check_fbo_status(context, GL_READ_FRAMEBUFFER);

        context_apply_fbo_state_blit(context, GL_DRAW_FRAMEBUFFER, swapchain->front_buffer, NULL, SFLAG_INDRAWABLE);
        context_set_draw_buffer(context, GL_BACK);
        context_invalidate_state(context, STATE_FRAMEBUFFER);

        gl_info->gl_ops.gl.p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        context_invalidate_state(context, STATE_RENDER(WINED3D_RS_COLORWRITEENABLE));
        context_invalidate_state(context, STATE_RENDER(WINED3D_RS_COLORWRITEENABLE1));
        context_invalidate_state(context, STATE_RENDER(WINED3D_RS_COLORWRITEENABLE2));
        context_invalidate_state(context, STATE_RENDER(WINED3D_RS_COLORWRITEENABLE3));

        gl_info->gl_ops.gl.p_glDisable(GL_SCISSOR_TEST);
        context_invalidate_state(context, STATE_RENDER(WINED3D_RS_SCISSORTESTENABLE));

        /* Note that the texture is upside down */
        gl_info->fbo_ops.glBlitFramebuffer(src_rect->left, src_rect->top, src_rect->right, src_rect->bottom,
                dst_rect->left, win_h - dst_rect->top, dst_rect->right, win_h - dst_rect->bottom,
                GL_COLOR_BUFFER_BIT, gl_filter);
        checkGLcall("Swapchain present blit(EXT_framebuffer_blit)\n");
    }
    else
    {
        struct wined3d_device *device = swapchain->device;
        struct wined3d_context *context2;
        float tex_left = src_rect->left;
        float tex_top = src_rect->top;
        float tex_right = src_rect->right;
        float tex_bottom = src_rect->bottom;

        context2 = context_acquire(device, swapchain->back_buffers[0]);
        context_apply_blit_state(context2, device);

        if (backbuffer->flags & SFLAG_NORMCOORD)
        {
            tex_left /= src_w;
            tex_right /= src_w;
            tex_top /= src_h;
            tex_bottom /= src_h;
        }

        if (is_complex_fixup(backbuffer->resource.format->color_fixup))
            gl_filter = GL_NEAREST;

        context_apply_fbo_state_blit(context2, GL_FRAMEBUFFER, swapchain->front_buffer, NULL, SFLAG_INDRAWABLE);
        context_bind_texture(context2, backbuffer->texture_target, backbuffer->texture_name);

        /* Set up the texture. The surface is not in a wined3d_texture
         * container, so there are no D3D texture settings to dirtify. */
        device->blitter->set_shader(device->blit_priv, context2, backbuffer);
        gl_info->gl_ops.gl.p_glTexParameteri(backbuffer->texture_target, GL_TEXTURE_MIN_FILTER, gl_filter);
        gl_info->gl_ops.gl.p_glTexParameteri(backbuffer->texture_target, GL_TEXTURE_MAG_FILTER, gl_filter);

        context_set_draw_buffer(context, GL_BACK);

        /* Set the viewport to the destination rectandle, disable any projection
         * transformation set up by context_apply_blit_state(), and draw a
         * (-1,-1)-(1,1) quad.
         *
         * Back up viewport and matrix to avoid breaking last_was_blit
         *
         * Note that context_apply_blit_state() set up viewport and ortho to
         * match the surface size - we want the GL drawable(=window) size. */
        gl_info->gl_ops.gl.p_glPushAttrib(GL_VIEWPORT_BIT);
        gl_info->gl_ops.gl.p_glViewport(dst_rect->left, win_h - dst_rect->bottom,
                dst_rect->right, win_h - dst_rect->top);
        gl_info->gl_ops.gl.p_glMatrixMode(GL_PROJECTION);
        gl_info->gl_ops.gl.p_glPushMatrix();
        gl_info->gl_ops.gl.p_glLoadIdentity();

        gl_info->gl_ops.gl.p_glBegin(GL_QUADS);
            /* bottom left */
            gl_info->gl_ops.gl.p_glTexCoord2f(tex_left, tex_bottom);
            gl_info->gl_ops.gl.p_glVertex2i(-1, -1);

            /* top left */
            gl_info->gl_ops.gl.p_glTexCoord2f(tex_left, tex_top);
            gl_info->gl_ops.gl.p_glVertex2i(-1, 1);

            /* top right */
            gl_info->gl_ops.gl.p_glTexCoord2f(tex_right, tex_top);
            gl_info->gl_ops.gl.p_glVertex2i(1, 1);

            /* bottom right */
            gl_info->gl_ops.gl.p_glTexCoord2f(tex_right, tex_bottom);
            gl_info->gl_ops.gl.p_glVertex2i(1, -1);
        gl_info->gl_ops.gl.p_glEnd();

        gl_info->gl_ops.gl.p_glPopMatrix();
        gl_info->gl_ops.gl.p_glPopAttrib();

        device->blitter->unset_shader(context->gl_info);
        checkGLcall("Swapchain present blit(manual)\n");

        context_release(context2);
    }
}

static void swapchain_gl_present(struct wined3d_swapchain *swapchain, const RECT *src_rect_in,
        const RECT *dst_rect_in, const RGNDATA *dirty_region, DWORD flags)
{
    struct wined3d_surface *back_buffer = swapchain->back_buffers[0];
    const struct wined3d_fb_state *fb = &swapchain->device->fb;
    const struct wined3d_gl_info *gl_info;
    struct wined3d_context *context;
    RECT src_rect, dst_rect;
    BOOL render_to_fbo;

    context = context_acquire(swapchain->device, back_buffer);
    if (!context->valid)
    {
        context_release(context);
        WARN("Invalid context, skipping present.\n");
        return;
    }

    gl_info = context->gl_info;

    /* Render the cursor onto the back buffer, using our nifty directdraw blitting code :-) */
    if (swapchain->device->bCursorVisible &&
        swapchain->device->cursorTexture &&
        !swapchain->device->hardwareCursor)
    {
        struct wined3d_surface cursor;
        RECT destRect =
        {
            swapchain->device->xScreenSpace - swapchain->device->xHotSpot,
            swapchain->device->yScreenSpace - swapchain->device->yHotSpot,
            swapchain->device->xScreenSpace + swapchain->device->cursorWidth - swapchain->device->xHotSpot,
            swapchain->device->yScreenSpace + swapchain->device->cursorHeight - swapchain->device->yHotSpot,
        };
        TRACE("Rendering the cursor. Creating fake surface at %p\n", &cursor);
        /* Build a fake surface to call the Blitting code. It is not possible to use the interface passed by
         * the application because we are only supposed to copy the information out. Using a fake surface
         * allows us to use the Blitting engine and avoid copying the whole texture -> render target blitting code.
         */
        memset(&cursor, 0, sizeof(cursor));
        cursor.resource.ref = 1;
        cursor.resource.device = swapchain->device;
        cursor.resource.pool = WINED3D_POOL_SCRATCH;
        cursor.resource.format = wined3d_get_format(gl_info, WINED3DFMT_B8G8R8A8_UNORM);
        cursor.resource.type = WINED3D_RTYPE_SURFACE;
        cursor.texture_name = swapchain->device->cursorTexture;
        cursor.texture_target = GL_TEXTURE_2D;
        cursor.texture_level = 0;
        cursor.resource.width = swapchain->device->cursorWidth;
        cursor.resource.height = swapchain->device->cursorHeight;
        /* The cursor must have pow2 sizes */
        cursor.pow2Width = cursor.resource.width;
        cursor.pow2Height = cursor.resource.height;
        /* The surface is in the texture */
        cursor.flags |= SFLAG_INTEXTURE;
        /* DDBLT_KEYSRC will cause BltOverride to enable the alpha test with GL_NOTEQUAL, 0.0,
         * which is exactly what we want :-)
         */
#ifndef VBOX_WITH_WINE_FIXES
        if (swapchain->desc.windowed)
            MapWindowPoints(NULL, swapchain->win_handle, (POINT *)&destRect, 2);
#endif
        wined3d_surface_blt(back_buffer, &destRect, &cursor, NULL, WINEDDBLT_KEYSRC,
                NULL, WINED3D_TEXF_POINT);
    }

    if (swapchain->device->logo_surface)
    {
        struct wined3d_surface *src_surface = swapchain->device->logo_surface;
        RECT rect = {0, 0, src_surface->resource.width, src_surface->resource.height};

        /* Blit the logo into the upper left corner of the drawable. */
        wined3d_surface_blt(back_buffer, &rect, src_surface, &rect, WINEDDBLT_KEYSRC,
                NULL, WINED3D_TEXF_POINT);
    }

#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
    TRACE("Presenting HDC %p.\n", context->hdc);
#else
    TRACE("Presenting HDC %p.\n", context->swapchain->hDC);
#endif

    render_to_fbo = swapchain->render_to_fbo;

    if (src_rect_in)
    {
        src_rect = *src_rect_in;
        if (!render_to_fbo && (src_rect.left || src_rect.top
                || src_rect.right != swapchain->desc.backbuffer_width
                || src_rect.bottom != swapchain->desc.backbuffer_height))
        {
            render_to_fbo = TRUE;
        }
    }
    else
    {
        src_rect.left = 0;
        src_rect.top = 0;
        src_rect.right = swapchain->desc.backbuffer_width;
        src_rect.bottom = swapchain->desc.backbuffer_height;
    }

    if (dst_rect_in)
        dst_rect = *dst_rect_in;
    else
        GetClientRect(swapchain->win_handle, &dst_rect);

    if (!render_to_fbo && (dst_rect.left || dst_rect.top
            || dst_rect.right != swapchain->desc.backbuffer_width
            || dst_rect.bottom != swapchain->desc.backbuffer_height))
        render_to_fbo = TRUE;

    /* Rendering to a window of different size, presenting partial rectangles,
     * or rendering to a different window needs help from FBO_blit or a textured
     * draw. Render the swapchain to a FBO in the future.
     *
     * Note that FBO_blit from the backbuffer to the frontbuffer cannot solve
     * all these issues - this fails if the window is smaller than the backbuffer.
     */
    if (!swapchain->render_to_fbo && render_to_fbo && wined3d_settings.offscreen_rendering_mode == ORM_FBO)
    {
        surface_load_location(back_buffer, SFLAG_INTEXTURE, NULL);
        surface_modify_location(back_buffer, SFLAG_INDRAWABLE, FALSE);
        swapchain->render_to_fbo = TRUE;
        swapchain_update_draw_bindings(swapchain);
    }
    else
    {
        surface_load_location(back_buffer, back_buffer->draw_binding, NULL);
    }

    if (swapchain->render_to_fbo)
    {
        /* This codepath should only be hit with the COPY swapeffect. Otherwise a backbuffer-
         * window size mismatch is impossible(fullscreen) and src and dst rectangles are
         * not allowed(they need the COPY swapeffect)
         *
         * The DISCARD swap effect is ok as well since any backbuffer content is allowed after
         * the swap. */
        if (swapchain->desc.swap_effect == WINED3D_SWAP_EFFECT_FLIP)
            FIXME("Render-to-fbo with WINED3D_SWAP_EFFECT_FLIP\n");

        swapchain_blit(swapchain, context, &src_rect, &dst_rect);
    }

#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
    if (swapchain->num_contexts > 1)
        gl_info->gl_ops.gl.p_glFinish();
#endif

#ifdef VBOX_WINE_WITH_SINGLE_CONTEXT
# ifdef DEBUG
    {
        HWND wnd = WindowFromDC(context->swapchain->hDC);
        Assert(wnd == context->swapchain->win_handle);
    }
# endif
    /* call wglSwapBuffers through the gl table to avoid confusing the Steam overlay */
    gl_info->gl_ops.wgl.p_wglSwapBuffers(context->swapchain->hDC); /* TODO: cycle through the swapchain buffers */
#else

    /* call wglSwapBuffers through the gl table to avoid confusing the Steam overlay */
    gl_info->gl_ops.wgl.p_wglSwapBuffers(context->hdc); /* TODO: cycle through the swapchain buffers */
#endif

    TRACE("SwapBuffers called, Starting new frame\n");
    /* FPS support */
    if (TRACE_ON(fps))
    {
        DWORD time = GetTickCount();
        ++swapchain->frames;

        /* every 1.5 seconds */
        if (time - swapchain->prev_time > 1500)
        {
            TRACE_(fps)("%p @ approx %.2ffps\n",
                    swapchain, 1000.0 * swapchain->frames / (time - swapchain->prev_time));
            swapchain->prev_time = time;
            swapchain->frames = 0;
        }
    }

    /* This is disabled, but the code left in for debug purposes.
     *
     * Since we're allowed to modify the new back buffer on a D3DSWAPEFFECT_DISCARD flip,
     * we can clear it with some ugly color to make bad drawing visible and ease debugging.
     * The Debug runtime does the same on Windows. However, a few games do not redraw the
     * screen properly, like Max Payne 2, which leaves a few pixels undefined.
     *
     * Tests show that the content of the back buffer after a discard flip is indeed not
     * reliable, so no game can depend on the exact content. However, it resembles the
     * old contents in some way, for example by showing fragments at other locations. In
     * general, the color theme is still intact. So Max payne, which draws rather dark scenes
     * gets a dark background image. If we clear it with a bright ugly color, the game's
     * bug shows up much more than it does on Windows, and the players see single pixels
     * with wrong colors.
     * (The Max Payne bug has been confirmed on Windows with the debug runtime) */
    if (FALSE && swapchain->desc.swap_effect == WINED3D_SWAP_EFFECT_DISCARD)
    {
        static const struct wined3d_color cyan = {0.0f, 1.0f, 1.0f, 1.0f};

        TRACE("Clearing the color buffer with cyan color\n");

        wined3d_device_clear(swapchain->device, 0, NULL,
                WINED3DCLEAR_TARGET, &cyan, 1.0f, 0);
    }

    if (!swapchain->render_to_fbo && ((swapchain->front_buffer->flags & SFLAG_INSYSMEM)
            || (back_buffer->flags & SFLAG_INSYSMEM)))
    {
        /* Both memory copies of the surfaces are ok, flip them around too instead of dirtifying
         * Doesn't work with render_to_fbo because we're not flipping
         */
        struct wined3d_surface *front = swapchain->front_buffer;

        if (front->resource.size == back_buffer->resource.size)
        {
            DWORD fbflags;
            flip_surface(front, back_buffer);

            /* Tell the front buffer surface that is has been modified. However,
             * the other locations were preserved during that, so keep the flags.
             * This serves to update the emulated overlay, if any. */
            fbflags = front->flags;
            surface_modify_location(front, SFLAG_INDRAWABLE, TRUE);
            front->flags = fbflags;
        }
        else
        {
            surface_modify_location(front, SFLAG_INDRAWABLE, TRUE);
            surface_modify_location(back_buffer, SFLAG_INDRAWABLE, TRUE);
        }
    }
    else
    {
        surface_modify_location(swapchain->front_buffer, SFLAG_INDRAWABLE, TRUE);
        /* If the swapeffect is DISCARD, the back buffer is undefined. That means the SYSMEM
         * and INTEXTURE copies can keep their old content if they have any defined content.
         * If the swapeffect is COPY, the content remains the same. If it is FLIP however,
         * the texture / sysmem copy needs to be reloaded from the drawable
         */
        if (swapchain->desc.swap_effect == WINED3D_SWAP_EFFECT_FLIP)
            surface_modify_location(back_buffer, back_buffer->draw_binding, TRUE);
    }

    if (fb->depth_stencil)
    {
        if (swapchain->desc.flags & WINED3DPRESENTFLAG_DISCARD_DEPTHSTENCIL
                || fb->depth_stencil->flags & SFLAG_DISCARD)
        {
            surface_modify_ds_location(fb->depth_stencil, SFLAG_DISCARDED,
                    fb->depth_stencil->resource.width,
                    fb->depth_stencil->resource.height);
            if (fb->depth_stencil == swapchain->device->onscreen_depth_stencil)
            {
                wined3d_surface_decref(swapchain->device->onscreen_depth_stencil);
                swapchain->device->onscreen_depth_stencil = NULL;
            }
        }
    }

    context_release(context);
}

static const struct wined3d_swapchain_ops swapchain_gl_ops =
{
    swapchain_gl_present,
};

/* Helper function that blits the front buffer contents to the target window. */
void x11_copy_to_screen(const struct wined3d_swapchain *swapchain, const RECT *rect)
{
    const struct wined3d_surface *front;
    POINT offset = {0, 0};
    HDC src_dc, dst_dc;
    RECT draw_rect;
    HWND window;

    TRACE("swapchain %p, rect %s.\n", swapchain, wine_dbgstr_rect(rect));

    front = swapchain->front_buffer;
    if (!(front->resource.usage & WINED3DUSAGE_RENDERTARGET))
        return;

    if (front->resource.map_count)
        ERR("Trying to blit a mapped surface.\n");

    TRACE("Copying surface %p to screen.\n", front);

    src_dc = front->hDC;
    window = swapchain->win_handle;
    dst_dc = GetDCEx(window, 0, DCX_CLIPSIBLINGS | DCX_CACHE);

    /* Front buffer coordinates are screen coordinates. Map them to the
     * destination window if not fullscreened. */
    if (swapchain->desc.windowed)
    {
#ifndef VBOX_WITH_WINE_FIXES
        ClientToScreen(window, &offset);
#else
        ERR("should not be here!");
#endif
    }

    TRACE("offset %s.\n", wine_dbgstr_point(&offset));

    draw_rect.left = 0;
    draw_rect.right = front->resource.width;
    draw_rect.top = 0;
    draw_rect.bottom = front->resource.height;

    if (rect)
        IntersectRect(&draw_rect, &draw_rect, rect);

    BitBlt(dst_dc, draw_rect.left - offset.x, draw_rect.top - offset.y,
            draw_rect.right - draw_rect.left, draw_rect.bottom - draw_rect.top,
            src_dc, draw_rect.left, draw_rect.top, SRCCOPY);
    ReleaseDC(window, dst_dc);
}

static void swapchain_gdi_present(struct wined3d_swapchain *swapchain, const RECT *src_rect_in,
        const RECT *dst_rect_in, const RGNDATA *dirty_region, DWORD flags)
{
    struct wined3d_surface *front, *back;

    front = swapchain->front_buffer;
    back = swapchain->back_buffers[0];

    /* Flip the DC. */
    {
        HDC tmp;
        tmp = front->hDC;
        front->hDC = back->hDC;
        back->hDC = tmp;
    }

    /* Flip the DIBsection. */
    {
        HBITMAP tmp;
        tmp = front->dib.DIBsection;
        front->dib.DIBsection = back->dib.DIBsection;
        back->dib.DIBsection = tmp;
    }

    /* Flip the surface data. */
    {
        void *tmp;

        tmp = front->dib.bitmap_data;
        front->dib.bitmap_data = back->dib.bitmap_data;
        back->dib.bitmap_data = tmp;

        tmp = front->resource.allocatedMemory;
        front->resource.allocatedMemory = back->resource.allocatedMemory;
        back->resource.allocatedMemory = tmp;

        if (front->resource.heapMemory)
            ERR("GDI Surface %p has heap memory allocated.\n", front);

        if (back->resource.heapMemory)
            ERR("GDI Surface %p has heap memory allocated.\n", back);
    }

    /* FPS support */
    if (TRACE_ON(fps))
    {
        static LONG prev_time, frames;
        DWORD time = GetTickCount();

        ++frames;

        /* every 1.5 seconds */
        if (time - prev_time > 1500)
        {
            TRACE_(fps)("@ approx %.2ffps\n", 1000.0 * frames / (time - prev_time));
            prev_time = time;
            frames = 0;
        }
    }

    x11_copy_to_screen(swapchain, NULL);
}

static const struct wined3d_swapchain_ops swapchain_gdi_ops =
{
    swapchain_gdi_present,
};

void swapchain_update_render_to_fbo(struct wined3d_swapchain *swapchain)
{
    RECT client_rect;

    if (wined3d_settings.offscreen_rendering_mode != ORM_FBO)
        return;

    if (!swapchain->desc.backbuffer_count)
    {
        TRACE("Single buffered rendering.\n");
        swapchain->render_to_fbo = FALSE;
        return;
    }

    GetClientRect(swapchain->win_handle, &client_rect);

    TRACE("Backbuffer %ux%u, window %ux%u.\n",
            swapchain->desc.backbuffer_width,
            swapchain->desc.backbuffer_height,
            client_rect.right, client_rect.bottom);
    TRACE("Multisample type %#x, quality %#x.\n",
            swapchain->desc.multisample_type,
            swapchain->desc.multisample_quality);

    if (!wined3d_settings.always_offscreen && !swapchain->desc.multisample_type
            && swapchain->desc.backbuffer_width == client_rect.right
            && swapchain->desc.backbuffer_height == client_rect.bottom)
    {
        TRACE("Backbuffer dimensions match window dimensions, rendering onscreen.\n");
        swapchain->render_to_fbo = FALSE;
        return;
    }

    TRACE("Rendering to FBO.\n");
    swapchain->render_to_fbo = TRUE;
}

/* Do not call while under the GL lock. */
static HRESULT swapchain_init(struct wined3d_swapchain *swapchain, struct wined3d_device *device,
        struct wined3d_swapchain_desc *desc, void *parent, const struct wined3d_parent_ops *parent_ops)
{
    const struct wined3d_adapter *adapter = device->adapter;
    struct wined3d_resource_desc surface_desc;
#ifndef VBOX_WITH_WDDM
    BOOL displaymode_set = FALSE;
#endif
    RECT client_rect;
    HWND window;
    HRESULT hr;
    UINT i;
#ifdef VBOX_WITH_WDDM
    struct wined3d_swapchain *overriden_swapchain = NULL;
    HDC hDC = NULL;
#endif

    if (desc->backbuffer_count > WINED3DPRESENT_BACK_BUFFER_MAX)
    {
        FIXME("The application requested %u back buffers, this is not supported.\n",
                desc->backbuffer_count);
        return WINED3DERR_INVALIDCALL;
    }

    if (desc->backbuffer_count > 1)
    {
        FIXME("The application requested more than one back buffer, this is not properly supported.\n"
                "Please configure the application to use double buffering (1 back buffer) if possible.\n");
    }

    if (device->wined3d->flags & WINED3D_NO3D)
        swapchain->swapchain_ops = &swapchain_gdi_ops;
    else
        swapchain->swapchain_ops = &swapchain_gl_ops;

#ifndef VBOX_WITH_WDDM
    window = desc->device_window ? desc->device_window : device->create_parms.focus_window;
#else
    if (desc->device_window)
    {
        overriden_swapchain = swapchain_find(device, desc->device_window);
        if (!overriden_swapchain)
        {
            ERR("invalid window handle supplied");
            return E_FAIL;
        }

        window = overriden_swapchain->win_handle;
        hDC = overriden_swapchain->hDC;
    }
    else
    {
        hr = VBoxExtWndCreate(desc->backbuffer_width, desc->backbuffer_height, &window, &hDC);
        if (FAILED(hr))
        {
            ERR("VBoxExtWndCreate failed, hr 0x%x", hr);
            return hr;
        }
    }
    Assert(window);
    Assert(hDC);
    desc->device_window = window;
#endif

    swapchain->device = device;
    swapchain->parent = parent;
    swapchain->parent_ops = parent_ops;
    swapchain->ref = 1;
    swapchain->win_handle = window;
#ifndef VBOX_WITH_WDDM
    swapchain->device_window = window;
# ifdef VBOX_WINE_WITH_SINGLE_CONTEXT
    swapchain->hDC = VBoxExtGetDC(window);
    if (!swapchain->hDC)
    {
        ERR("failed to get window DC");
        return E_FAIL;
    }
# endif
#else
    Assert(window);
    swapchain->hDC = hDC;
#endif

#ifndef VBOX_WITH_WDDM
    if (FAILED(hr = wined3d_get_adapter_display_mode(device->wined3d,
            adapter->ordinal, &swapchain->original_mode, NULL)))
    {
        ERR("Failed to get current display mode, hr %#x.\n", hr);
        goto err;
    }
#endif

    GetClientRect(window, &client_rect);
    if (desc->windowed
            && (!desc->backbuffer_width || !desc->backbuffer_height
            || desc->backbuffer_format == WINED3DFMT_UNKNOWN))
    {

        if (!desc->backbuffer_width)
        {
            desc->backbuffer_width = client_rect.right;
            TRACE("Updating width to %u.\n", desc->backbuffer_width);
        }

        if (!desc->backbuffer_height)
        {
            desc->backbuffer_height = client_rect.bottom;
            TRACE("Updating height to %u.\n", desc->backbuffer_height);
        }
#ifndef VBOX_WITH_WDDM
        if (desc->backbuffer_format == WINED3DFMT_UNKNOWN)
        {
            desc->backbuffer_format = swapchain->original_mode.format_id;
            TRACE("Updating format to %s.\n", debug_d3dformat(swapchain->original_mode.format_id));
        }
#endif
    }
    swapchain->desc = *desc;
    swapchain_update_render_to_fbo(swapchain);

    TRACE("Creating front buffer.\n");

    surface_desc.resource_type = WINED3D_RTYPE_SURFACE;
    surface_desc.format = swapchain->desc.backbuffer_format;
    surface_desc.multisample_type = swapchain->desc.multisample_type;
    surface_desc.multisample_quality = swapchain->desc.multisample_quality;
    surface_desc.usage = WINED3DUSAGE_RENDERTARGET;
    surface_desc.pool = WINED3D_POOL_DEFAULT;
    surface_desc.width = swapchain->desc.backbuffer_width;
    surface_desc.height = swapchain->desc.backbuffer_height;
    surface_desc.depth = 1;
    surface_desc.size = 0;

    if (FAILED(hr = device->device_parent->ops->create_swapchain_surface(device->device_parent,
            parent, &surface_desc, &swapchain->front_buffer)))
    {
        WARN("Failed to create front buffer, hr %#x.\n", hr);
        goto err;
    }

    surface_set_swapchain(swapchain->front_buffer, swapchain);
    if (!(device->wined3d->flags & WINED3D_NO3D))
        surface_modify_location(swapchain->front_buffer, SFLAG_INDRAWABLE, TRUE);

    /* MSDN says we're only allowed a single fullscreen swapchain per device,
     * so we should really check to see if there is a fullscreen swapchain
     * already. Does a single head count as full screen? */

#ifndef VBOX_WITH_WDDM
    if (!desc->windowed)
    {
        struct wined3d_display_mode mode;

        /* Change the display settings */
        mode.width = desc->backbuffer_width;
        mode.height = desc->backbuffer_height;
        mode.format_id = desc->backbuffer_format;
        mode.refresh_rate = desc->refresh_rate;
        mode.scanline_ordering = WINED3D_SCANLINE_ORDERING_UNKNOWN;

        if (FAILED(hr = wined3d_set_adapter_display_mode(device->wined3d, adapter->ordinal, &mode)))
        {
            WARN("Failed to set display mode, hr %#x.\n", hr);
            goto err;
        }
        displaymode_set = TRUE;
    }
#endif

    if (!(device->wined3d->flags & WINED3D_NO3D))
    {
#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
        static const enum wined3d_format_id formats[] =
        {
            WINED3DFMT_D24_UNORM_S8_UINT,
            WINED3DFMT_D32_UNORM,
            WINED3DFMT_R24_UNORM_X8_TYPELESS,
            WINED3DFMT_D16_UNORM,
            WINED3DFMT_S1_UINT_D15_UNORM
        };

        const struct wined3d_gl_info *gl_info = &adapter->gl_info;

        swapchain->context = HeapAlloc(GetProcessHeap(), 0, sizeof(*swapchain->context));
        if (!swapchain->context)
        {
            ERR("Failed to create the context array.\n");
            hr = E_OUTOFMEMORY;
            goto err;
        }
        swapchain->num_contexts = 1;

        /* In WGL both color, depth and stencil are features of a pixel format. In case of D3D they are separate.
         * You are able to add a depth + stencil surface at a later stage when you need it.
         * In order to support this properly in WineD3D we need the ability to recreate the opengl context and
         * drawable when this is required. This is very tricky as we need to reapply ALL opengl states for the new
         * context, need torecreate shaders, textures and other resources.
         *
         * The context manager already takes care of the state problem and for the other tasks code from Reset
         * can be used. These changes are way to risky during the 1.0 code freeze which is taking place right now.
         * Likely a lot of other new bugs will be exposed. For that reason request a depth stencil surface all the
         * time. It can cause a slight performance hit but fixes a lot of regressions. A fixme reminds of that this
         * issue needs to be fixed. */
        for (i = 0; i < (sizeof(formats) / sizeof(*formats)); i++)
        {
            swapchain->ds_format = wined3d_get_format(gl_info, formats[i]);
            swapchain->context[0] = context_create(swapchain, swapchain->front_buffer, swapchain->ds_format);
            if (swapchain->context[0]) break;
            TRACE("Depth stencil format %s is not supported, trying next format\n",
                  debug_d3dformat(formats[i]));
        }

        if (!swapchain->context[0])
#else
        struct wined3d_context * swapchain_context;
        swapchain->ds_format = wined3d_get_format(&device->adapter->gl_info, WINED3DFMT_D24_UNORM_S8_UINT);
        swapchain_context = context_find_create(device, swapchain, swapchain->ds_format);
        if (!swapchain_context)
#endif
        {
            WARN("Failed to create context.\n");
            hr = WINED3DERR_NOTAVAILABLE;
            goto err;
        }

        if (wined3d_settings.offscreen_rendering_mode != ORM_FBO
                && (!desc->enable_auto_depth_stencil
                || swapchain->desc.auto_depth_stencil_format != swapchain->ds_format->id))
        {
            FIXME("Add OpenGL context recreation support to context_validate_onscreen_formats\n");
        }
#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
        context_release(swapchain->context[0]);
#else
        context_release(swapchain_context);
#endif
    }

    if (swapchain->desc.backbuffer_count > 0)
    {
        swapchain->back_buffers = HeapAlloc(GetProcessHeap(), 0,
                sizeof(*swapchain->back_buffers) * swapchain->desc.backbuffer_count);
        if (!swapchain->back_buffers)
        {
            ERR("Failed to allocate backbuffer array memory.\n");
            hr = E_OUTOFMEMORY;
            goto err;
        }

        for (i = 0; i < swapchain->desc.backbuffer_count; ++i)
        {
            TRACE("Creating back buffer %u.\n", i);
            if (FAILED(hr = device->device_parent->ops->create_swapchain_surface(device->device_parent,
                    parent, &surface_desc, &swapchain->back_buffers[i])))
            {
                WARN("Failed to create back buffer %u, hr %#x.\n", i, hr);
                goto err;
            }
            surface_set_swapchain(swapchain->back_buffers[i], swapchain);
        }
    }

    /* Swapchains share the depth/stencil buffer, so only create a single depthstencil surface. */
    if (desc->enable_auto_depth_stencil && !(device->wined3d->flags & WINED3D_NO3D))
    {
        TRACE("Creating depth/stencil buffer.\n");
        if (!device->auto_depth_stencil)
        {
            surface_desc.format = swapchain->desc.auto_depth_stencil_format;
            surface_desc.usage = WINED3DUSAGE_DEPTHSTENCIL;

            if (FAILED(hr = device->device_parent->ops->create_swapchain_surface(device->device_parent,
                    device->device_parent, &surface_desc, &device->auto_depth_stencil)))
            {
                WARN("Failed to create the auto depth stencil, hr %#x.\n", hr);
                goto err;
            }
        }
    }

#ifndef VBOX_WITH_WDDM
    wined3d_swapchain_get_gamma_ramp(swapchain, &swapchain->orig_gamma);
#else
    if (overriden_swapchain)
    {
        swapchain_invalidate(overriden_swapchain);
    }
#endif

    return WINED3D_OK;

err:
#ifndef VBOX_WITH_WDDM
    if (displaymode_set)
    {
        if (FAILED(wined3d_set_adapter_display_mode(device->wined3d,
                adapter->ordinal, &swapchain->original_mode)))
            ERR("Failed to restore display mode.\n");
        ClipCursor(NULL);
    }
#endif
    if (swapchain->back_buffers)
    {
        for (i = 0; i < swapchain->desc.backbuffer_count; ++i)
        {
            if (swapchain->back_buffers[i])
            {
                surface_set_swapchain(swapchain->back_buffers[i], NULL);
                wined3d_surface_decref(swapchain->back_buffers[i]);
            }
        }
        HeapFree(GetProcessHeap(), 0, swapchain->back_buffers);
#ifdef VBOX_WITH_WINE_FIX_INITCLEAR
        swapchain->back_buffers = NULL;
#endif
    }
#ifdef VBOX_WITH_WINE_FIX_INITCLEAR
    swapchain->desc.backbuffer_count = 0;
#endif
#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
    if (swapchain->context)
    {
        if (swapchain->context[0])
        {
            context_release(swapchain->context[0]);
            context_destroy(device, swapchain->context[0]);
            swapchain->num_contexts = 0;
        }
        HeapFree(GetProcessHeap(), 0, swapchain->context);
    }
#else
    if (!device->swapchain_count)
    {
        while (device->context_count)
        {
            context_destroy(device, device->contexts[0]);
        }
    }
#endif
    if (swapchain->front_buffer)
    {
        surface_set_swapchain(swapchain->front_buffer, NULL);
        wined3d_surface_decref(swapchain->front_buffer);
    }

#ifdef VBOX_WITH_WDDM
    if (!overriden_swapchain && swapchain->win_handle)
    {
        VBoxExtWndDestroy(swapchain->win_handle, swapchain->hDC);
    }
    swapchain_invalidate(swapchain);
#endif

    return hr;
}

/* Do not call while under the GL lock. */
HRESULT CDECL wined3d_swapchain_create(struct wined3d_device *device, struct wined3d_swapchain_desc *desc,
        void *parent, const struct wined3d_parent_ops *parent_ops, struct wined3d_swapchain **swapchain)
{
    struct wined3d_swapchain *object;
    HRESULT hr;

    TRACE("device %p, desc %p, parent %p, parent_ops %p, swapchain %p.\n",
            device, desc, parent, parent_ops, swapchain);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    hr = swapchain_init(object, device, desc, parent, parent_ops);
    if (FAILED(hr))
    {
        WARN("Failed to initialize swapchain, hr %#x.\n", hr);
        HeapFree(GetProcessHeap(), 0, object);
        return hr;
    }

    TRACE("Created swapchain %p.\n", object);
    *swapchain = object;

    return WINED3D_OK;
}

#ifndef VBOX_WINE_WITH_SINGLE_CONTEXT
/* Do not call while under the GL lock. */
static struct wined3d_context *swapchain_create_context(struct wined3d_swapchain *swapchain)
{
    struct wined3d_context **newArray;
    struct wined3d_context *ctx;

    TRACE("Creating a new context for swapchain %p, thread %u.\n", swapchain, GetCurrentThreadId());

    if (!(ctx = context_create(swapchain, swapchain->front_buffer, swapchain->ds_format)))
    {
        ERR("Failed to create a new context for the swapchain\n");
        return NULL;
    }
    context_release(ctx);

    newArray = HeapAlloc(GetProcessHeap(), 0, sizeof(*newArray) * (swapchain->num_contexts + 1));
    if(!newArray) {
        ERR("Out of memory when trying to allocate a new context array\n");
        context_destroy(swapchain->device, ctx);
        return NULL;
    }
    memcpy(newArray, swapchain->context, sizeof(*newArray) * swapchain->num_contexts);
    HeapFree(GetProcessHeap(), 0, swapchain->context);
    newArray[swapchain->num_contexts] = ctx;
    swapchain->context = newArray;
    swapchain->num_contexts++;

    TRACE("Returning context %p\n", ctx);
    return ctx;
}

void swapchain_destroy_contexts(struct wined3d_swapchain *swapchain)
{
    unsigned int i;

    for (i = 0; i < swapchain->num_contexts; ++i)
    {
        context_destroy(swapchain->device, swapchain->context[i]);
    }
    swapchain->num_contexts = 0;
}

struct wined3d_context *swapchain_get_context(struct wined3d_swapchain *swapchain)
{
    DWORD tid = GetCurrentThreadId();
    unsigned int i;

    for (i = 0; i < swapchain->num_contexts; ++i)
    {
#  ifdef VBOX_WINE_WITH_SINGLE_SWAPCHAIN_CONTEXT
        if (VBoxTlsRefIsFunctional(swapchain->context[i]))
#  else
        if (swapchain->context[i]->tid == tid)
#endif
            return swapchain->context[i];
    }

    /* Create a new context for the thread */
    return swapchain_create_context(swapchain);
}
#else /* if defined VBOX_WINE_WITH_SINGLE_CONTEXT */
struct wined3d_context *swapchain_get_context(struct wined3d_swapchain *swapchain)
{
    struct wined3d_context * context = swapchain->device->contexts[0];
    return context;
}
#endif

void get_drawable_size_swapchain(const struct wined3d_context *context, UINT *width, UINT *height)
{
    /* The drawable size of an onscreen drawable is the surface size.
     * (Actually: The window size, but the surface is created in window size) */
    *width = context->current_rt->resource.width;
    *height = context->current_rt->resource.height;
}

#ifndef VBOX_WITH_WDDM
HDC swapchain_get_backup_dc(struct wined3d_swapchain *swapchain)
{
    if (!swapchain->backup_dc)
    {
        TRACE("Creating the backup window for swapchain %p.\n", swapchain);

        if (!(swapchain->backup_wnd = CreateWindowA(WINED3D_OPENGL_WINDOW_CLASS_NAME, "WineD3D fake window",
                WS_OVERLAPPEDWINDOW, 10, 10, 10, 10, NULL, NULL, NULL, NULL)))
        {
            ERR("Failed to create a window.\n");
            return NULL;
        }

        if (!(swapchain->backup_dc = GetDC(swapchain->backup_wnd)))
        {
            ERR("Failed to get a DC.\n");
            DestroyWindow(swapchain->backup_wnd);
            swapchain->backup_wnd = NULL;
            return NULL;
        }
    }

    return swapchain->backup_dc;
}
#endif
void swapchain_update_draw_bindings(struct wined3d_swapchain *swapchain)
{
    UINT i;

    surface_update_draw_binding(swapchain->front_buffer);

    for (i = 0; i < swapchain->desc.backbuffer_count; ++i)
    {
        surface_update_draw_binding(swapchain->back_buffers[i]);
    }
}

#ifdef VBOX_WITH_WDDM
HRESULT CDECL wined3d_swapchain_present_rt(struct wined3d_swapchain *swapchain, struct wined3d_surface *rt)
{
    struct wined3d_surface *bb = swapchain->back_buffers[0];
    HRESULT hr = wined3d_surface_blt(bb, NULL, rt, NULL, 0, NULL, 0);
    if (FAILED(hr))
    {
        ERR("wined3d_surface_blt failed with hr(%d)", hr);
        return hr;
    }

    hr =  wined3d_swapchain_present(swapchain, NULL, NULL, NULL, NULL, 0);
    if (FAILED(hr))
    {
        ERR("wined3d_swapchain_present failed with hr(%d)", hr);
        return hr;
    }

    return S_OK;
}

HRESULT CDECL wined3d_swapchain_get_host_win_id(struct wined3d_swapchain *swapchain, int32_t *pi32Id)
{
    int32_t id = pVBoxGetWindowId(swapchain->hDC);
    if (!id)
    {
        *pi32Id = 0;
        ERR("failed to get id for hdc 0x%x", swapchain->hDC);
        return E_FAIL;
    }
    *pi32Id = id;
    return S_OK;
}
#endif
