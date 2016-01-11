/*
 * Cogl
 *
 * A Low Level GPU Graphics and Utilities API
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * Based on the PowerVR NULL backend and KMS backend
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl-winsys-egl-external-private.h"
#include "cogl-winsys-egl-private.h"
#include "cogl-renderer-private.h"
#include "cogl-framebuffer-private.h"
#include "cogl-onscreen-private.h"

static const CoglWinsysEGLVtable _cogl_winsys_egl_vtable;

typedef struct _CoglDisplayExternal
{
  int egl_surface_width;
  int egl_surface_height;
  CoglBool have_onscreen;
} CoglDisplayExternal;

static void
_cogl_winsys_renderer_disconnect (CoglRenderer *renderer)
{
  CoglRendererEGL *egl_renderer = renderer->winsys;

  eglTerminate (egl_renderer->edpy);

  g_slice_free (CoglRendererEGL, egl_renderer);
}

static CoglBool
_cogl_winsys_renderer_connect (CoglRenderer *renderer,
                               CoglError **error)
{
  CoglRendererEGL *egl_renderer;

  renderer->winsys = g_slice_new0 (CoglRendererEGL);
  egl_renderer = renderer->winsys;

  egl_renderer->platform_vtable = &_cogl_winsys_egl_vtable;

  egl_renderer->edpy = eglGetDisplay (EGL_DEFAULT_DISPLAY);

  if (!_cogl_winsys_egl_renderer_connect_common (renderer, error))
    goto error;

  return TRUE;

error:
  _cogl_winsys_renderer_disconnect (renderer);
  return FALSE;
}

static CoglBool
_cogl_winsys_egl_context_created (CoglDisplay *display,
                                  CoglError **error)
{
  CoglRenderer *renderer = display->renderer;
  CoglRendererEGL *egl_renderer = renderer->winsys;
  CoglDisplayEGL *egl_display = display->winsys;
  CoglDisplayExternal *platform_display = egl_display->platform;
  const char *error_message;

  egl_display->egl_surface =
    eglCreateWindowSurface (egl_renderer->edpy,
                            egl_display->egl_config,
                            (EGLNativeWindowType) NULL,
                            NULL);
  if (egl_display->egl_surface == EGL_NO_SURFACE)
    {
      error_message = "Unable to create EGL window surface";
      goto fail;
    }

  if (!_cogl_winsys_egl_make_current (display,
                                      egl_display->egl_surface,
                                      egl_display->egl_surface,
                                      egl_display->egl_context))
    {
      error_message = "Unable to eglMakeCurrent with egl surface";
      goto fail;
    }

  eglQuerySurface (egl_renderer->edpy,
                   egl_display->egl_surface,
                   EGL_WIDTH,
                   &platform_display->egl_surface_width);

  eglQuerySurface (egl_renderer->edpy,
                   egl_display->egl_surface,
                   EGL_HEIGHT,
                   &platform_display->egl_surface_height);

  return TRUE;

 fail:
  _cogl_set_error (error, COGL_WINSYS_ERROR,
                   COGL_WINSYS_ERROR_CREATE_CONTEXT,
                   "%s", error_message);
  return FALSE;
}

static CoglBool
_cogl_winsys_egl_display_setup (CoglDisplay *display,
                                CoglError **error)
{
  CoglDisplayEGL *egl_display = display->winsys;
  CoglDisplayExternal *platform_display;

  platform_display = g_slice_new0 (CoglDisplayExternal);
  egl_display->platform = platform_display;

  return TRUE;
}

static void
_cogl_winsys_egl_display_destroy (CoglDisplay *display)
{
  CoglDisplayEGL *egl_display = display->winsys;

  g_slice_free (CoglDisplayExternal, egl_display->platform);
}

static void
_cogl_winsys_egl_cleanup_context (CoglDisplay *display)
{
  CoglRenderer *renderer = display->renderer;
  CoglRendererEGL *egl_renderer = renderer->winsys;
  CoglDisplayEGL *egl_display = display->winsys;

  if (egl_display->egl_surface != EGL_NO_SURFACE)
    {
      eglDestroySurface (egl_renderer->edpy, egl_display->egl_surface);
      egl_display->egl_surface = EGL_NO_SURFACE;
    }
}

static void
_cogl_winsys_onscreen_swap_buffers_with_damage (CoglOnscreen *onscreen,
                                                const int *rectangles,
                                                int n_rectangles)
{
  CoglContext *context = COGL_FRAMEBUFFER (onscreen)->context;
  CoglDisplayEGL *egl_display = context->display->winsys;
  CoglDisplayKMS *kms_display = egl_display->platform;
  CoglRenderer *renderer = context->display->renderer;
  CoglRendererEGL *egl_renderer = renderer->winsys;
  CoglRendererKMS *kms_renderer = egl_renderer->platform;
  CoglOnscreenEGL *egl_onscreen = onscreen->winsys;
  CoglOnscreenKMS *kms_onscreen = egl_onscreen->platform;
  uint32_t handle, stride;
  CoglFlipKMS *flip;

  /* If we already have a pending swap then block until it completes */
  while (kms_onscreen->next_fb_id != 0)
    handle_drm_event (kms_renderer);

  if (kms_onscreen->pending_egl_surface)
    {
      eglDestroySurface (egl_renderer->edpy, egl_onscreen->egl_surface);
      egl_onscreen->egl_surface = kms_onscreen->pending_egl_surface;
      kms_onscreen->pending_egl_surface = NULL;

      _cogl_framebuffer_winsys_update_size (COGL_FRAMEBUFFER (kms_display->onscreen),
                                            kms_display->width, kms_display->height);
      context->current_draw_buffer_changes |= COGL_FRAMEBUFFER_STATE_BIND;
    }
  parent_vtable->onscreen_swap_buffers_with_damage (onscreen,
                                                    rectangles,
                                                    n_rectangles);

  if (kms_onscreen->pending_surface)
    {
      free_current_bo (onscreen);
      gbm_surface_destroy (kms_onscreen->surface);
      kms_onscreen->surface = kms_onscreen->pending_surface;
      kms_onscreen->pending_surface = NULL;
    }
  /* Now we need to set the CRTC to whatever is the front buffer */
  kms_onscreen->next_bo = gbm_surface_lock_front_buffer (kms_onscreen->surface);

#if (COGL_VERSION_ENCODE (COGL_GBM_MAJOR, COGL_GBM_MINOR, COGL_GBM_MICRO) >= \
     COGL_VERSION_ENCODE (8, 1, 0))
  stride = gbm_bo_get_stride (kms_onscreen->next_bo);
#else
  stride = gbm_bo_get_pitch (kms_onscreen->next_bo);
#endif
  handle = gbm_bo_get_handle (kms_onscreen->next_bo).u32;

  if (drmModeAddFB (kms_renderer->fd,
                    kms_display->width,
                    kms_display->height,
                    24, /* depth */
                    32, /* bpp */
                    stride,
                    handle,
                    &kms_onscreen->next_fb_id))
    {
      g_warning ("Failed to create new back buffer handle: %m");
      gbm_surface_release_buffer (kms_onscreen->surface,
                                  kms_onscreen->next_bo);
      kms_onscreen->next_bo = NULL;
      kms_onscreen->next_fb_id = 0;
      return;
    }

  /* If this is the first framebuffer to be presented then we now setup the
   * crtc modes, else we flip from the previous buffer */
  if (kms_display->pending_set_crtc)
    {
      setup_crtc_modes (context->display, kms_onscreen->next_fb_id);
      kms_display->pending_set_crtc = FALSE;
    }

  flip = g_slice_new0 (CoglFlipKMS);
  flip->onscreen = onscreen;

  flip_all_crtcs (context->display, flip, kms_onscreen->next_fb_id);

  if (flip->pending == 0)
    {
      drmModeRmFB (kms_renderer->fd, kms_onscreen->next_fb_id);
      gbm_surface_release_buffer (kms_onscreen->surface,
                                  kms_onscreen->next_bo);
      kms_onscreen->next_bo = NULL;
      kms_onscreen->next_fb_id = 0;
      g_slice_free (CoglFlipKMS, flip);
      flip = NULL;

      queue_swap_notify_for_onscreen (onscreen);
    }
  else
    {
      /* Ensure the onscreen remains valid while it has any pending flips... */
      cogl_object_ref (flip->onscreen);

      /* Process flip right away if we can't wait for vblank */
      if (kms_renderer->page_flips_not_supported)
        {
          setup_crtc_modes (context->display, kms_onscreen->next_fb_id);
          process_flip (flip);
        }
    }
}

static CoglBool
_cogl_winsys_egl_onscreen_init (CoglOnscreen *onscreen,
                                EGLConfig egl_config,
                                CoglError **error)
{
  CoglFramebuffer *framebuffer = COGL_FRAMEBUFFER (onscreen);
  CoglContext *context = framebuffer->context;
  CoglDisplay *display = context->display;
  CoglDisplayEGL *egl_display = display->winsys;
  CoglDisplayExternal *platform_display = egl_display->platform;
  CoglOnscreenEGL *egl_onscreen = onscreen->winsys;

  if (platform_display->have_onscreen)
    {
      _cogl_set_error (error, COGL_WINSYS_ERROR,
                   COGL_WINSYS_ERROR_CREATE_ONSCREEN,
                   "EGL platform only supports a single onscreen window");
      return FALSE;
    }

  egl_onscreen->egl_surface = egl_display->egl_surface;

  _cogl_framebuffer_winsys_update_size (framebuffer,
                                        platform_display->egl_surface_width,
                                        platform_display->egl_surface_height);
  platform_display->have_onscreen = TRUE;

  return TRUE;
}

static void
_cogl_winsys_egl_onscreen_deinit (CoglOnscreen *onscreen)
{
  CoglFramebuffer *framebuffer = COGL_FRAMEBUFFER (onscreen);
  CoglContext *context = framebuffer->context;
  CoglDisplay *display = context->display;
  CoglDisplayEGL *egl_display = display->winsys;
  CoglDisplayExternal *platform_display = egl_display->platform;

  platform_display->have_onscreen = FALSE;
}

static const CoglWinsysEGLVtable
_cogl_winsys_egl_vtable =
  {
    .display_setup = _cogl_winsys_egl_display_setup,
    .display_destroy = _cogl_winsys_egl_display_destroy,
    .context_created = _cogl_winsys_egl_context_created,
    .cleanup_context = _cogl_winsys_egl_cleanup_context,
    .onscreen_init = _cogl_winsys_egl_onscreen_init,
    .onscreen_deinit = _cogl_winsys_egl_onscreen_deinit
  };

const CoglWinsysVtable *
_cogl_winsys_egl_external_get_vtable (void)
{
  static CoglBool vtable_inited = FALSE;
  static CoglWinsysVtable vtable;

  if (!vtable_inited)
    {
      /* The EGL_EXTERNAL winsys is a subclass of the EGL winsys so we
         start by copying its vtable */

      vtable = *_cogl_winsys_egl_get_vtable ();

      vtable.id = COGL_WINSYS_ID_EGL_EXTERNAL;
      vtable.name = "EGL_EXTERNAL";

      vtable.renderer_connect = _cogl_winsys_renderer_connect;
      vtable.renderer_disconnect = _cogl_winsys_renderer_disconnect;

      vtable.onscreen_swap_region = NULL;
      vtable.onscreen_swap_buffers_with_damage =
        _cogl_winsys_onscreen_swap_buffers_with_damage;

      vtable_inited = TRUE;
    }

  return &vtable;
}

void
cogl_egl_external_renderer_set_swap_handlers (CoglRenderer *renderer,

