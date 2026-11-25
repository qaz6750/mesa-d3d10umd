/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/


#include "gdikmt/gdikmt.h"
#include "util/u_debug.h"
#include "target-helpers/inline_debug_helper.h"
#ifdef GALLIUM_LLVMPIPE
#include "llvmpipe/lp_public.h"
#endif
#ifdef GALLIUM_SOFTPIPE
#include "softpipe/sp_public.h"
#endif
#ifdef GALLIUM_VIRGL
#include "virgl/gdi/virgl_gdi_public.h"
#endif
#ifdef GALLIUM_FREEDRENO
#include <unistd.h>
#include "xf86drm.h"
#include "util/xmlconfig.h"
#include "util/driconf.h"
#include "pipe/p_screen.h"
#include "renderonly/renderonly.h"
#include "compiler/glsl_types.h"

struct pipe_screen *fd_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro);

/* freedreno reads driconf options in fd_screen_create(): give it its defaults */
static const driOptionDescription fd_driconf[] = {
#include "freedreno/driinfo_freedreno.h"
};

/* renderonly.c needs libdrm and is not built on Windows; freedreno only calls these when
 * screen->ro != NULL, and we always pass NULL. */
void
renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
}

struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc, struct renderonly *ro,
                                          struct winsys_handle *out_handle)
{
   return NULL;
}

/* qcdxkm: DRM ioctls of freedreno travel as escapes through the D3D10 device callbacks */
static int
d3d10_fd_escape(void *ctx, void *data, unsigned size)
{
   struct gdikmt_device *device = (struct gdikmt_device *)ctx;
   return device->escape(device, data, size) == 0 ? 0 : -1;
}
#endif

extern struct pipe_screen *
d3d10_create_screen(struct gdikmt_device* device);

void
d3d10_destroy_screen(struct pipe_screen *screen)
{
   screen->destroy(screen);
#if defined(GALLIUM_FREEDRENO) && !defined(GALLIUM_VIRGL)
   glsl_type_singleton_decref();
#endif
}

#if !defined(GALLIUM_FREEDRENO) && !defined(GALLIUM_VIRGL)
#include "frontend/sw_winsys.h"
#include "sw/gdi/gdi_sw_winsys.h"
#include "winddk_compat.h"
#include <d3dkmthk.h>

static HDC d3d10_gdi_acquire_hdc(void *winsys_drawable_handle) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   return GetDC(hWnd);
}

static void d3d10_gdi_release_hdc(void *winsys_drawable_handle, HDC hDC) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   ReleaseDC(hWnd, hDC);
}

#endif

struct pipe_screen *
d3d10_create_screen(struct gdikmt_device* device)
{
#ifdef GALLIUM_VIRGL
   return virgl_gdi_screen_create(device);
#elif defined(GALLIUM_FREEDRENO)
   {
      int fd = fd_wddm_open(d3d10_fd_escape, device);
      if (fd < 0)
         return NULL;
      /* freedreno/NIR need the GLSL type cache; on Linux the GL frontend creates it */
      glsl_type_singleton_init_or_ref();
      driOptionCache info = {0}, options = {0};
      struct pipe_screen_config config = {.options_info = &info, .options = &options};
      driParseOptionInfo(&info, fd_driconf, ARRAY_SIZE(fd_driconf));
      struct pipe_screen *screen = fd_screen_create(fd, &config, NULL);
      /* fd_screen_create takes its own duplicate and copies the options. */
      fd_wddm_close(fd);
      driDestroyOptionCache(&options);
      driDestroyOptionInfo(&info);
      if (screen)
         screen = debug_screen_wrap(screen);
      if (!screen)
         glsl_type_singleton_decref();
      return screen;
   }
#else
   const char *default_driver;
   const char *driver;
   struct pipe_screen *screen = NULL;
   struct sw_winsys *winsys = gdi_create_sw_winsys(d3d10_gdi_acquire_hdc, d3d10_gdi_release_hdc);
   if(!winsys)
      goto no_winsys;

#ifdef GALLIUM_LLVMPIPE
   default_driver = "llvmpipe";
#else
   default_driver = "softpipe";
#endif

   driver = debug_get_option("GALLIUM_DRIVER", default_driver);

#ifdef GALLIUM_LLVMPIPE
   if (strcmp(driver, "llvmpipe") == 0) {
      screen = llvmpipe_create_screen( winsys );
   }
#else
   (void)driver;
#endif

#ifdef GALLIUM_SOFTPIPE
   if (screen == NULL) {
      screen = softpipe_create_screen( winsys );
   }
#endif

   if (screen == NULL)
      goto no_screen;

   return debug_screen_wrap( screen );

no_screen:
   winsys->destroy(winsys);
no_winsys:
   return NULL;
#endif
}
