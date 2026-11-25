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
#include "util/box.h"
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
   if (!pPresentData || !pPresentData->hDevice)
      return E_INVALIDARG;

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);
   Resource *pDst = pPresentData->hDstResource ? CastResource(pPresentData->hDstResource) : NULL;
   UINT src = pSrcResource ? pSrcResource->hAllocation : 0;
   UINT dst = pDst ? pDst->hAllocation : 0;
   QcdxTraceDisplay(&device->device, QCDX_PRESENT_BEGIN, src, dst, pPresentData->Flags.Value, S_OK);

   if (pSrcResource && pSrcResource->hAllocation) {
      TopazStripsRestore(device);
      TopazMaybeDump(device->pipe, pSrcResource->resource);
      TopazProbePresent();
      HRESULT hr = QcdxWddmPresent(device, pSrcResource, pDst, pPresentData->pDXGIContext);
      QcdxTraceDisplay(&device->device, QCDX_PRESENT_RESULT, src, dst, pPresentData->Flags.Value, hr);
      static unsigned n;
      if (++n <= 60 || FAILED(hr) || getenv("QCDX_DRAWLOG"))
         QcdxWddmLogf("DXGI Present #%u res %p alloc 0x%x dst %p flags 0x%x -> 0x%08lx\n", n, pSrcResource->resource,
                       pSrcResource->hAllocation, pPresentData->hDstResource, pPresentData->Flags.Value, (unsigned long)hr);
      return hr;
   }
   QcdxWddmLogf("DXGI Present rejected: missing WDDM source allocation\n");
   QcdxTraceDisplay(&device->device, QCDX_PRESENT_RESULT, src, dst, pPresentData->Flags.Value, E_INVALIDARG);
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
   if (!SetDisplayMode || !SetDisplayMode->hDevice || !SetDisplayMode->hResource)
      return E_INVALIDARG;

   Device* device = CastDevice(SetDisplayMode->hDevice);
   Resource *res = CastResource(SetDisplayMode->hResource);
   D3DKMT_HANDLE allocation = res->hAllocation;
   if (!device->device.KTCallbacks.pfnSetDisplayModeCb)
      return DXGI_DDI_ERR_UNSUPPORTED;
   if (!allocation) {
      if (device->vidmmWinsys || !res->resource || !device->screen->resource_get_handle)
         return E_INVALIDARG;
      struct winsys_handle handle = {};
      handle.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
      if (!device->screen->resource_get_handle(device->screen, NULL, res->resource, &handle, 0) || !handle.handle)
         return DXGI_DDI_ERR_UNSUPPORTED;
      allocation = (D3DKMT_HANDLE)(uintptr_t)handle.handle;
   }
   QcdxTraceDisplay(&device->device, QCDX_MODE_BEGIN, allocation, 0, 0, S_OK);
   /* The mode callback can pin and scan this primary before the first Present. */
   HRESULT hr = QcdxWddmFinishResource(device, res);
   if (SUCCEEDED(hr))
      hr = device->device.base.setDisplayMode(&device->device.base, allocation);
   QcdxWddmLogf("DXGI SetDisplayMode alloc 0x%x -> 0x%08lx\n", allocation, (unsigned long)hr);
   return hr;
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

   if (!RotateResourceIdentities || !RotateResourceIdentities->hDevice ||
       (RotateResourceIdentities->Resources && !RotateResourceIdentities->pResources))
      return E_INVALIDARG;
   QcdxWddmLogf("DXGI RotateResourceIdentities %u\n", RotateResourceIdentities->Resources);
   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }
   Device *owner = CastDevice(RotateResourceIdentities->hDevice);
   if (owner->vidmmWinsys) {
      Resource *first = CastResource(RotateResourceIdentities->pResources[0]);
      HRESULT hr = QcdxWddmRotateVidmm(owner, RotateResourceIdentities);
      QcdxTraceDisplay(&owner->device, QCDX_ROTATE, first ? first->hAllocation : 0,
                       RotateResourceIdentities->Resources, 0, hr);
      return hr;
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

static HRESULT
CopyRotated(struct pipe_context *pipe, const struct pipe_blit_info &blit,
            DXGI_DDI_MODE_ROTATION rotation, bool mayAlias)
{
   struct pipe_transfer *srcTransfer = NULL, *dstTransfer = NULL;
   const uint8_t *src = static_cast<const uint8_t *>(pipe->texture_map(pipe, blit.src.resource,
      blit.src.level, PIPE_MAP_READ, &blit.src.box, &srcTransfer));
   if (!src)
      return E_FAIL;

   const ptrdiff_t pixel = util_format_get_blocksize(blit.dst.format);
   ptrdiff_t stride = srcTransfer->stride;
   uint8_t *snapshot = NULL;
   if (mayAlias) {
      // Distinct KMT handles can open the same allocation. Finish reading before
      // locking its destination so an in-place turn cannot overwrite unread texels.
      const size_t rowBytes = blit.src.box.width * pixel;
      snapshot = static_cast<uint8_t *>(calloc(blit.src.box.height, rowBytes));
      if (snapshot) {
         for (int y = 0; y < blit.src.box.height; ++y)
            memcpy(snapshot + y * rowBytes, src + y * stride, rowBytes);
      }
      pipe_texture_unmap(pipe, srcTransfer);
      srcTransfer = NULL;
      if (!snapshot)
         return E_OUTOFMEMORY;
      src = snapshot;
      stride = rowBytes;
   }
   uint8_t *dst = static_cast<uint8_t *>(pipe->texture_map(pipe, blit.dst.resource,
      blit.dst.level, PIPE_MAP_WRITE, &blit.dst.box, &dstTransfer));
   if (!dst) {
      if (srcTransfer)
         pipe_texture_unmap(pipe, srcTransfer);
      free(snapshot);
      return E_FAIL;
   }

   const ptrdiff_t lastRow = (blit.src.box.height - 1) * stride;
   const ptrdiff_t lastCol = (blit.src.box.width - 1) * pixel;
   ptrdiff_t stepX, stepY;
   if (rotation == DXGI_DDI_MODE_ROTATION_ROTATE90) {
      src += lastRow;
      stepX = -stride;
      stepY = pixel;
   } else if (rotation == DXGI_DDI_MODE_ROTATION_ROTATE180) {
      src += lastRow + lastCol;
      stepX = -pixel;
      stepY = -stride;
   } else {
      src += lastCol;
      stepX = stride;
      stepY = -pixel;
   }
   for (int y = 0; y < blit.dst.box.height; ++y) {
      const uint8_t *srcRow = src + y * stepY;
      uint8_t *dstRow = dst + (size_t)y * dstTransfer->stride;
      for (int x = 0; x < blit.dst.box.width; ++x)
         memcpy(dstRow + x * pixel, srcRow + x * stepX, pixel);
   }
   pipe_texture_unmap(pipe, dstTransfer);
   if (srcTransfer)
      pipe_texture_unmap(pipe, srcTransfer);
   free(snapshot);
   return S_OK;
}

static HRESULT
BltResources(Device *device, Resource *src, Resource *dst, const DXGI_DDI_ARG_BLT *Blt)
{
   if (!src || !dst || !src->resource || !dst->resource)
      return E_INVALIDARG;
   if (static_cast<unsigned>(Blt->Rotate) > DXGI_DDI_MODE_ROTATION_ROTATE270)
      return DXGI_DDI_ERR_UNSUPPORTED;

   const bool rotated = Blt->Rotate >= DXGI_DDI_MODE_ROTATION_ROTATE90;
   if (Blt->SrcSubresource >= src->NumSubResources || Blt->DstSubresource >= dst->NumSubResources ||
       src->buffer || dst->buffer || (!rotated && src->resource == dst->resource))
      return E_INVALIDARG;
   struct pipe_blit_info blit = {};
   blit.src.resource = src->resource;
   blit.src.format = src->resource->format;
   blit.src.level = Blt->SrcSubresource % src->MipLevels;
   blit.src.box.z = Blt->SrcSubresource / src->MipLevels;
   blit.src.box.width = u_minify(src->resource->width0, blit.src.level);
   blit.src.box.height = u_minify(src->resource->height0, blit.src.level);
   blit.src.box.depth = 1;
   blit.dst.resource = dst->resource;
   blit.dst.format = dst->resource->format;
   blit.dst.level = Blt->DstSubresource % dst->MipLevels;
   if (Blt->DstRight > u_minify(dst->resource->width0, blit.dst.level) ||
       Blt->DstBottom > u_minify(dst->resource->height0, blit.dst.level) ||
       Blt->DstLeft > Blt->DstRight || Blt->DstTop > Blt->DstBottom)
      return E_INVALIDARG;
   blit.dst.box.x = Blt->DstLeft;
   blit.dst.box.y = Blt->DstTop;
   blit.dst.box.z = Blt->DstSubresource / dst->MipLevels;
   blit.dst.box.width = Blt->DstRight - Blt->DstLeft;
   blit.dst.box.height = Blt->DstBottom - Blt->DstTop;
   blit.dst.box.depth = 1;
   if (!blit.dst.box.width || !blit.dst.box.height)
      return S_OK;

   const bool transpose = Blt->Rotate == DXGI_DDI_MODE_ROTATION_ROTATE90 ||
                          Blt->Rotate == DXGI_DDI_MODE_ROTATION_ROTATE270;
   const int width = transpose ? blit.dst.box.height : blit.dst.box.width;
   const int height = transpose ? blit.dst.box.width : blit.dst.box.height;
   if (rotated && (util_format_get_blockwidth(blit.dst.format) != 1 ||
                   util_format_get_blockheight(blit.dst.format) != 1 ||
                   util_format_is_depth_or_stencil(blit.dst.format) ||
                   blit.dst.resource->nr_samples > 1))
      return DXGI_DDI_ERR_UNSUPPORTED;
   const bool mayAlias = src->resource == dst->resource || (src->hAllocation && dst->hAllocation);
   if (rotated && mayAlias) {
      HRESULT hr = QcdxWddmFinishResource(device, dst);
      if (FAILED(hr))
         return hr;
   }
   if (blit.src.format == blit.dst.format && blit.src.box.width == width && blit.src.box.height == height &&
       !Blt->Flags.Resolve && (!rotated || blit.src.resource->nr_samples <= 1)) {
      if (rotated)
         return CopyRotated(device->pipe, blit, Blt->Rotate, mayAlias);
      device->pipe->resource_copy_region(device->pipe, blit.dst.resource, blit.dst.level,
         blit.dst.box.x, blit.dst.box.y, blit.dst.box.z, blit.src.resource, blit.src.level, &blit.src.box);
   } else {
      if ((device->vidmmWinsys && Blt->Flags.Resolve) ||
          !device->screen->is_format_supported(device->screen, blit.src.format, blit.src.resource->target,
             blit.src.resource->nr_samples, blit.src.resource->nr_storage_samples, PIPE_BIND_SAMPLER_VIEW) ||
          !device->screen->is_format_supported(device->screen, blit.dst.format, blit.dst.resource->target,
             blit.dst.resource->nr_samples, blit.dst.resource->nr_storage_samples, PIPE_BIND_RENDER_TARGET))
         return DXGI_DDI_ERR_UNSUPPORTED;
      blit.mask = PIPE_MASK_RGBA;
      blit.filter = Blt->Flags.Stretch ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
      if (rotated) {
         // Gallium blits can scale and convert, but their rectangles have parallel axes.
         // Blit to the unrotated extent first and rotate the resulting texels once.
         struct pipe_resource desc = {};
         desc.target = PIPE_TEXTURE_2D;
         desc.format = blit.dst.format;
         desc.width0 = width;
         desc.height0 = height;
         desc.depth0 = desc.array_size = 1;
         desc.bind = PIPE_BIND_RENDER_TARGET;
         struct pipe_resource *scratch = device->screen->resource_create(device->screen, &desc);
         if (!scratch)
            return E_OUTOFMEMORY;
         struct pipe_blit_info scaled = blit;
         scaled.dst.resource = scratch;
         scaled.dst.level = 0;
         u_box_2d(0, 0, width, height, &scaled.dst.box);
         device->pipe->blit(device->pipe, &scaled);
         blit.src = scaled.dst;
         HRESULT hr = CopyRotated(device->pipe, blit, Blt->Rotate, false);
         pipe_resource_reference(&scratch, NULL);
         return hr;
      }
      device->pipe->blit(device->pipe, &blit);
   }
   return S_OK;
}

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *args)
{
   if (!args || !args->hDevice)
      return E_INVALIDARG;
   Device *device = CastDevice(args->hDevice);
   Resource *src = CastResource(args->hSrcResource), *dst = CastResource(args->hDstResource);
   HRESULT hr = BltResources(device, src, dst, args);
   QcdxTraceDisplay(&device->device, QCDX_BLT, src ? src->hAllocation : 0,
                    dst ? dst->hAllocation : 0, args->Flags.Value, hr);
   return hr;
}
