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
   UINT src = pSrcResource ? pSrcResource->shared.handle : 0;
   UINT dst = pDst ? pDst->shared.handle : 0;
   QcdxTraceDisplay(&device->device, QCDX_PRESENT_BEGIN, src, dst, pPresentData->Flags.Value, S_OK);

   if (pSrcResource && pSrcResource->shared.handle) {
      TopazStripsRestore(device);
      TopazMaybeDump(device->pipe, pSrcResource->resource);
      HRESULT hr = QcdxWddmPresent(device, pSrcResource, pDst, pPresentData->pDXGIContext);
      QcdxTraceDisplay(&device->device, QCDX_PRESENT_RESULT, src, dst, pPresentData->Flags.Value, hr);
      static unsigned n;
      if (++n <= 60 || FAILED(hr) || getenv("QCDX_DRAWLOG"))
         QcdxWddmLogf("DXGI Present #%u res %p alloc 0x%x dst %p flags 0x%x -> 0x%08lx\n", n, pSrcResource->resource,
                       pSrcResource->shared.handle, pPresentData->hDstResource, pPresentData->Flags.Value, (unsigned long)hr);
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
   D3DKMT_HANDLE allocation = res->shared.handle;
   if (!device->device.KTCallbacks.pfnSetDisplayModeCb)
      return DXGI_DDI_ERR_UNSUPPORTED;
   if (!allocation) {
      if (!res->resource || !device->screen->resource_get_handle)
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

static HRESULT
RotateGpuStorage(Device *device, const DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   pipe_resource *stack[4];
   pipe_resource **storage = stack;
   const UINT count = args->Resources;
   Resource *first = CastResource(args->pResources[0]);
   if (!first || !first->resource)
      return E_INVALIDARG;

   for (UINT i = 0; i < count; ++i) {
      Resource *r = CastResource(args->pResources[i]);
      if (!r || !r->resource || !r->shared.handle || r->NumSubResources != 1 ||
          !r->transfers || r->transfers[0] || r->vidmmLocked || r->resource->target != PIPE_TEXTURE_2D ||
          r->resource->screen != first->resource->screen || r->resource->nr_samples != first->resource->nr_samples ||
          r->resource->format != first->resource->format || r->resource->width0 != first->resource->width0 ||
          r->resource->height0 != first->resource->height0)
         return E_INVALIDARG;
      for (UINT j = 0; j < i; ++j) {
         if (r->resource == CastResource(args->pResources[j])->resource)
            return E_INVALIDARG;
      }
   }
   if (count > ARRAY_SIZE(stack)) {
      storage = static_cast<pipe_resource **>(calloc(count, sizeof(*storage)));
      if (!storage)
         return E_OUTOFMEMORY;
   }
   for (UINT i = 0; i < count; ++i)
      storage[i] = CastResource(args->pResources[i])->resource;

   // Views stay bound to their pipe_resource. The VidMM handle must follow
   // the underlying BO through the entire flip chain.
   QcdxCountFlush(&device->device, QCDX_FLUSH_ROTATE);
   device->pipe->flush(device->pipe, NULL, 0);
   fd_resource_rotate_storage(device->pipe, storage, count);
   if (storage != stack)
      free(storage);
   AcquireSRWLockExclusive(&device->device.qcdxSubmitLock);
   const auto firstStorage = first->shared;
   for (UINT i = 0; i < count; ++i) {
      auto &slot = CastResource(args->pResources[i])->shared;
      QcdxGpuVaRotateShared(&device->device, slot.boName);
      const list_head link = slot.link;
      slot = i + 1 < count ? CastResource(args->pResources[i + 1])->shared : firstStorage;
      slot.link = link;
   }
   ReleaseSRWLockExclusive(&device->device.qcdxSubmitLock);
   return S_OK;
}

HRESULT APIENTRY
_RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   LOG_ENTRYPOINT();
   if (!args || !args->hDevice || (args->Resources && !args->pResources))
      return E_INVALIDARG;
   if (args->Resources <= 1)
      return S_OK;

   Device *device = CastDevice(args->hDevice);
   HRESULT hr = RotateGpuStorage(device, args);
   Resource *first = CastResource(args->pResources[0]);
   QcdxTraceDisplay(&device->device, QCDX_ROTATE, first ? first->shared.handle : 0, args->Resources, 0, hr);
   return hr;
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

extern "C" bool fd6_wddm_blit_rotated(struct pipe_context *pipe, const struct pipe_blit_info *info,
                                       unsigned turns);

static HRESULT
BltResources(Device *device, Resource *src, Resource *dst, const DXGI_DDI_ARG_BLT *Blt)
{
   if (!src || !dst || !src->resource || !dst->resource)
      return E_INVALIDARG;
   if (static_cast<unsigned>(Blt->Rotate) > DXGI_DDI_MODE_ROTATION_ROTATE270)
      return DXGI_DDI_ERR_UNSUPPORTED;

   const bool rotated = Blt->Rotate >= DXGI_DDI_MODE_ROTATION_ROTATE90;
   if (Blt->SrcSubresource >= src->NumSubResources || Blt->DstSubresource >= dst->NumSubResources ||
       src->buffer || dst->buffer)
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
   const bool sameTexture = src->resource == dst->resource;
   const bool sameSubresource = sameTexture && Blt->SrcSubresource == Blt->DstSubresource;
   const bool mayAlias = sameTexture ? sameSubresource :
      src->shared.boName && src->shared.boName == dst->shared.boName;
   if (sameSubresource && !rotated && !Blt->Flags.Resolve &&
       blit.src.box.width == width && blit.src.box.height == height)
      return S_OK;
   if (mayAlias) {
      HRESULT hr = QcdxWddmFinishResource(device, dst);
      if (FAILED(hr))
         return hr;
   }
   const bool simple = blit.src.format == blit.dst.format && blit.src.box.width == width &&
                       blit.src.box.height == height && !Blt->Flags.Resolve;
   blit.mask = PIPE_MASK_RGBA;
   blit.filter = Blt->Flags.Stretch ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   if (simple && !mayAlias && !rotated) {
      device->pipe->resource_copy_region(device->pipe, blit.dst.resource, blit.dst.level,
         blit.dst.box.x, blit.dst.box.y, blit.dst.box.z, blit.src.resource, blit.src.level, &blit.src.box);
      return S_OK;
   }
   if (!device->screen->is_format_supported(device->screen, blit.src.format, blit.src.resource->target,
          blit.src.resource->nr_samples, blit.src.resource->nr_storage_samples, PIPE_BIND_SAMPLER_VIEW) ||
       !device->screen->is_format_supported(device->screen, blit.dst.format, blit.dst.resource->target,
          blit.dst.resource->nr_samples, blit.dst.resource->nr_storage_samples, PIPE_BIND_RENDER_TARGET))
      return DXGI_DDI_ERR_UNSUPPORTED;

   struct pipe_resource *scratch = NULL;
   if (mayAlias || (rotated && !simple)) {
      struct pipe_resource desc = {};
      desc.target = PIPE_TEXTURE_2D;
      desc.format = blit.dst.format;
      desc.width0 = width;
      desc.height0 = height;
      desc.depth0 = desc.array_size = 1;
      desc.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
      scratch = device->screen->resource_create(device->screen, &desc);
      if (!scratch) return E_OUTOFMEMORY;
      struct pipe_blit_info scaled = blit;
      scaled.dst.resource = scratch;
      scaled.dst.level = 0;
      u_box_2d(0, 0, width, height, &scaled.dst.box);
      device->pipe->blit(device->pipe, &scaled);
      blit.src = scaled.dst;
   }
   bool ok = true;
   if (rotated)
      ok = fd6_wddm_blit_rotated(device->pipe, &blit, Blt->Rotate - DXGI_DDI_MODE_ROTATION_IDENTITY);
   else
      device->pipe->blit(device->pipe, &blit);
   pipe_resource_reference(&scratch, NULL);
   return ok ? S_OK : DXGI_DDI_ERR_UNSUPPORTED;
}

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *args)
{
   if (!args || !args->hDevice)
      return E_INVALIDARG;
   Device *device = CastDevice(args->hDevice);
   Resource *src = CastResource(args->hSrcResource), *dst = CastResource(args->hDstResource);
   HRESULT hr = BltResources(device, src, dst, args);
   QcdxTraceDisplay(&device->device, QCDX_BLT, src ? src->shared.handle : 0,
                    dst ? dst->shared.handle : 0, args->Flags.Value, hr);
   return hr;
}
