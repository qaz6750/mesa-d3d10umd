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
 **************************************************************************/

/*
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdint.h>
#include <windows.h>
#include <stdio.h>
#include <winerror.h>
#include <winnt.h>

#include "DxgiFns.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"

extern "C" void fd_resource_rotate_storage(struct pipe_context *pctx, struct pipe_resource **prscs, unsigned n);

#include "frontend/winsys_handle.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"

/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */
HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);

   if (pSrcResource && pSrcResource->hAllocation) {
      Resource *pDst = pPresentData->hDstResource ? CastResource(pPresentData->hDstResource) : NULL;
      TopazStripsRestore(device);
      TopazMaybeDump(device->pipe, pSrcResource->resource);
      TopazProbePresent();
      HRESULT hr = QcdxWddmPresent(device, pSrcResource, pDst, pPresentData->pDXGIContext);
      static unsigned n;
      if (++n <= 60 || FAILED(hr) || getenv("QCDX_DRAWLOG"))
         QcdxWddmLogf("DXGI Present #%u res %p alloc 0x%x dst %p flags 0x%x -> 0x%08lx\n", n, pSrcResource->resource,
                       pSrcResource->hAllocation, pPresentData->hDstResource, pPresentData->Flags.Value, (unsigned long)hr);
      return hr;
   }
   QcdxWddmLogf("DXGI Present rejected: missing WDDM source allocation\n");
   return E_INVALIDARG;
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   LOG_ENTRYPOINT();
   
   Device* device = CastDevice(SetDisplayMode->hDevice);
   Resource *res = CastResource(SetDisplayMode->hResource);

   if (res->hAllocation) {
      HRESULT hr = device->device.base.setDisplayMode(&device->device.base, res->hAllocation);
      QcdxWddmLogf("DXGI SetDisplayMode alloc 0x%x -> 0x%08lx\n", res->hAllocation, (unsigned long)hr);
      return hr;
   }
   QcdxWddmLogf("DXGI SetDisplayMode without allocation\n");

   if(!device->screen->resource_get_handle) {
      LOG_UNSUPPORTED_ENTRYPOINT();
      return S_OK;
   }

   struct winsys_handle handle;
   handle.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
   if(!device->screen->resource_get_handle(device->screen, NULL, res->resource, &handle, 0)) {
      LOG_UNSUPPORTED_ENTRYPOINT();
      return S_OK;
   };

   device->device.base.setDisplayMode(&device->device.base, (D3DKMT_HANDLE)handle.handle);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *RotateResourceIdentities )
{
   LOG_ENTRYPOINT();

   QcdxWddmLogf("DXGI RotateResourceIdentities %u\n", RotateResourceIdentities->Resources);
   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }
   UINT NumResources = RotateResourceIdentities->Resources;
   const DXGI_DDI_HRESOURCE *hResources = RotateResourceIdentities->pResources;

   /* qcdxkm experiment (umd.env QCDX_NOROTATE=1): no rotation - DWM renders into and flips the same
    * buffer, which then always holds the previous frame (DWM redraws only dirty regions) */
   if (getenv("QCDX_NOROTATE"))
      return S_OK;

   /* qcdxkm: rotate the storage (freedreno BOs), not the pipe_resource objects: views keep pointing
    * at the right buffer. The allocation handles follow their BOs. */
   {
      Device *device = CastDevice(RotateResourceIdentities->hDevice);
      struct pipe_resource *prscs[16];
      UINT n = NumResources < 16 ? NumResources : 16;
      D3DKMT_HANDLE firstAllocation = CastResource(hResources[0])->hAllocation;

      /* QCDX_ROTATE_REVERSE=1: rotate the other way ([i] takes [i-1]) - experiment */
      bool rev = getenv("QCDX_ROTATE_REVERSE") != NULL;
      for (UINT i = 0; i < n; ++i)
         prscs[rev ? n - 1 - i : i] = CastPipeResource(hResources[i]);
      device->pipe->flush(device->pipe, NULL, 0);
      {
         static unsigned topazRot;
         if (++topazRot <= 60)
            QcdxWddmLogf("  rotate%s: [0] %p a0x%x [1] %p a0x%x [2] %p a0x%x\n", rev ? " reverse" : "",
                          CastResource(hResources[0])->resource, CastResource(hResources[0])->hAllocation,
                          n > 1 ? CastResource(hResources[1])->resource : NULL, n > 1 ? CastResource(hResources[1])->hAllocation : 0,
                          n > 2 ? CastResource(hResources[2])->resource : NULL, n > 2 ? CastResource(hResources[2])->hAllocation : 0);
      }
      fd_resource_rotate_storage(device->pipe, prscs, n);
      /* QCDX_FULLCOPY=1: the new back buffer [1] starts as a full copy of the frame just presented
       * ([0]). DWM then only has to be right inside its damage rect; stale content outside it that
       * came from diverged buffers can no longer exist. Cross-check experiment. */
      if (!rev && n > 1 && getenv("QCDX_FULLCOPY") && getenv("QCDX_FULLCOPY")[0] == '1') {
         struct pipe_context *pipe = device->pipe;
         struct pipe_resource *src = prscs[0], *dst = prscs[1];
         struct pipe_box box;
         memset(&box, 0, sizeof(box));
         box.width = src->width0;
         box.height = src->height0;
         box.depth = 1;
         pipe->resource_copy_region(pipe, dst, 0, 0, 0, 0, src, 0, &box);
      }
      /* QCDX_POISON=1 (INVALID as a test: DWM relies on the back buffers keeping their content, it
       * only copies/redraws the damage - poisoning makes everything outside the damage magenta): the next back buffer ([1]) becomes magenta, so pixels DWM does not write in
       * a frame show up as magenta (draw-side bug) instead of stale content */
      if (!rev && n > 1 && getenv("QCDX_POISON") && getenv("QCDX_POISON")[0] == '1') {
         struct pipe_context *pipe = device->pipe;
         struct pipe_resource *pr = prscs[1];
         struct pipe_surface tmpl;
         memset(&tmpl, 0, sizeof(tmpl));
         tmpl.format = pr->format;
         tmpl.u.tex.level = 0;
         tmpl.u.tex.first_layer = 0;
         tmpl.u.tex.last_layer = 0;
         struct pipe_surface *surf = pipe->create_surface(pipe, pr, &tmpl);
         if (surf) {
            union pipe_color_union magenta;
            magenta.f[0] = 1.0f; magenta.f[1] = 0.0f; magenta.f[2] = 1.0f; magenta.f[3] = 1.0f;
            pipe->clear_render_target(pipe, surf, &magenta, 0, 0, surf->width, surf->height, true);
            pipe_surface_reference(&surf, NULL);
         }
      }
      if (!rev) {
         for (UINT i = 0; i + 1 < n; ++i)
            CastResource(hResources[i])->hAllocation = CastResource(hResources[i + 1])->hAllocation;
         CastResource(hResources[n - 1])->hAllocation = firstAllocation;
      } else {
         D3DKMT_HANDLE lastAllocation = CastResource(hResources[n - 1])->hAllocation;
         for (UINT i = n - 1; i > 0; --i)
            CastResource(hResources[i])->hAllocation = CastResource(hResources[i - 1])->hAllocation;
         CastResource(hResources[0])->hAllocation = lastAllocation;
      }
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   QcdxWddmLogf("DXGI Blt (unsupported)\n");
   LOG_UNSUPPORTED_ENTRYPOINT();

   return S_OK;
}
