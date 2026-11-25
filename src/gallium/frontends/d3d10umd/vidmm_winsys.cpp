// SPDX-License-Identifier: MIT
// Linear WDDM display targets for Mesa's synchronous software rasterizer.
#include "State.h"
#include "frontend/sw_winsys.h"
#include "freedreno/drm/wddm/qcdx_escape.h"
#include "gallium/drivers/softpipe/sp_texture.h"
#include <utility>

struct VidmmWinsys {
   sw_winsys base;
   Device *device;
   bool stageImports;
};

struct VidmmSurface {
   D3DKMT_HANDLE allocation;
   unsigned pitch;
   bool privateAllocation;
   qcdx_alloc stagingDesc;
   D3DKMT_HANDLE staging;
   SRWLOCK mapLock;
   void *mapped;
   unsigned mapCount;
   bool dirty;
   bool copyPending;
};

static Device *
GetDevice(sw_winsys *ws)
{
   return reinterpret_cast<VidmmWinsys *>(ws)->device;
}

static bool
FormatSupported(sw_winsys *, unsigned, enum pipe_format format)
{
   return QcdxD3dFormat(format) != 0;
}

static sw_displaytarget *
CreateSurface(sw_winsys *ws, unsigned, enum pipe_format format, unsigned width,
              unsigned height, unsigned alignment, const void *, unsigned *stride)
{
   Device *dev = GetDevice(ws);
   qcdx_alloc desc = {};
   desc.format = QcdxD3dFormat(format);
   if (!width || !height || !desc.format || !alignment || (alignment & (alignment - 1)))
      return NULL;
   uint64_t row = ((uint64_t)width * util_format_get_blocksize(format) + alignment - 1) & ~(uint64_t)(alignment - 1);
   if (row > UINT_MAX / height)
      return NULL;

   VidmmSurface *surface = (VidmmSurface *)calloc(1, sizeof(*surface));
   if (!surface)
      return NULL;
   desc.magic = QCDX_ALLOC_MAGIC;
   desc.kind = QCDX_ALLOC_UMD;
   desc.width = width;
   desc.height = height;
   desc.pitch = (unsigned)row;
   desc.size = desc.pitch * height;
   if (util_format_is_srgb(format))
      desc.flags |= QCDX_ALLOC_F_SRGB;
   if (dev->device.isPrimary) {
      desc.flags |= QCDX_ALLOC_F_PRIMARY;
      desc.vidpn = dev->device.allocationVidPn;
   }

   D3DDDI_ALLOCATIONINFO info = {};
   info.pPrivateDriverData = &desc;
   info.PrivateDriverDataSize = sizeof(desc);
   gdikmt_createallocation args = {};
   args.NumAllocations = 1;
   args.pAllocationInfo = &info;
   HRESULT hr = dev->device.base.createAllocation(&dev->device.base, &args);
   if (FAILED(hr) || !info.hAllocation) {
      QcdxWddmLogf("AllocateCb %ux%u pitch=%u format=%u flags=%x allocation=%x hr=%08lx\n",
                   width, height, desc.pitch, desc.format, desc.flags, info.hAllocation, (unsigned long)hr);
      free(surface);
      return NULL;
   }
   surface->allocation = info.hAllocation;
   surface->pitch = *stride = desc.pitch;
   surface->privateAllocation = !dev->device.hRTResource;
   return reinterpret_cast<sw_displaytarget *>(surface);
}

static sw_displaytarget *
FromHandle(sw_winsys *ws, const pipe_resource *res, winsys_handle *handle, unsigned *stride)
{
   if (!res || !QcdxD3dFormat(res->format) || handle->type != WINSYS_HANDLE_TYPE_D3DKMT_ALLOC ||
       !handle->handle || handle->offset)
      return NULL;
   unsigned pixelBytes = util_format_get_blocksize(res->format);
   if (handle->stride % pixelBytes || (uint64_t)res->width0 * pixelBytes > handle->stride ||
       (uint64_t)handle->stride * res->height0 > UINT_MAX)
      return NULL;
   VidmmSurface *surface = (VidmmSurface *)calloc(1, sizeof(*surface));
   if (surface) {
      surface->allocation = (D3DKMT_HANDLE)(uintptr_t)handle->handle;
      surface->pitch = *stride = handle->stride;
      if (reinterpret_cast<VidmmWinsys *>(ws)->stageImports) {
         auto &desc = surface->stagingDesc;
         desc.magic = QCDX_ALLOC_MAGIC;
         desc.kind = QCDX_ALLOC_UMD;
         desc.format = QcdxD3dFormat(res->format);
         desc.width = res->width0;
         desc.height = res->height0;
         desc.pitch = handle->stride;
         desc.size = desc.pitch * desc.height;
      }
   }
   return reinterpret_cast<sw_displaytarget *>(surface);
}

static bool
GetHandle(sw_winsys *, sw_displaytarget *dt, winsys_handle *handle)
{
   if (handle->type != WINSYS_HANDLE_TYPE_D3DKMT_ALLOC)
      return false;
   VidmmSurface *surface = reinterpret_cast<VidmmSurface *>(dt);
   handle->handle = (HANDLE)(uintptr_t)surface->allocation;
   handle->stride = surface->pitch;
   handle->offset = 0;
   return true;
}

static HRESULT
UnlockAllocation(Device *dev, D3DKMT_HANDLE allocation)
{
   D3DDDICB_UNLOCK args = {};
   args.NumAllocations = 1;
   args.phAllocations = &allocation;
   HRESULT hr = dev->device.KTCallbacks.pfnUnlockCb(dev->device.hRTDevice, &args);
   if (FAILED(hr))
      QcdxTraceDisplay(&dev->device, QCDX_UNLOCK_CB, allocation, 0, 0, hr);
   return hr;
}

static HRESULT
LockAllocation(Device *dev, D3DKMT_HANDLE allocation, unsigned usage, void **address)
{
   D3DDDICB_LOCKFLAGS flags = {};
   flags.ReadOnly = !(usage & PIPE_MAP_WRITE);
   flags.WriteOnly = !(usage & PIPE_MAP_READ);
   flags.DonotWait = !!(usage & PIPE_MAP_DONTBLOCK);
   flags.LockEntire = TRUE;
   HRESULT hr = dev->device.base.lockAllocation(&dev->device.base, allocation, flags, address);
   if (SUCCEEDED(hr) && !*address) {
      UnlockAllocation(dev, allocation);
      hr = E_UNEXPECTED;
   }
   if (FAILED(hr))
      QcdxTraceDisplay(&dev->device, QCDX_LOCK_CB, allocation, usage, flags.Value, hr);
   return hr;
}

static HRESULT
CreateStaging(Device *dev, VidmmSurface *surface)
{
   D3DDDI_ALLOCATIONINFO info = {};
   info.pPrivateDriverData = &surface->stagingDesc;
   info.PrivateDriverDataSize = sizeof(surface->stagingDesc);
   D3DDDICB_ALLOCATE args = {};
   args.NumAllocations = 1;
   args.pAllocationInfo = &info;
   HRESULT hr = dev->device.KTCallbacks.pfnAllocateCb(dev->device.hRTDevice, &args);
   if (FAILED(hr) || !info.hAllocation) {
      if (SUCCEEDED(hr)) hr = E_UNEXPECTED;
      QcdxTraceDisplay(&dev->device, QCDX_ALLOCATE_CB, surface->allocation, 1, 0, hr);
      return hr;
   }
   surface->staging = info.hAllocation;
   return S_OK;
}

static HRESULT
CopyStaging(Device *dev, VidmmSurface *surface, bool writeBack)
{
   D3DKMT_HANDLE source = writeBack ? surface->staging : surface->allocation;
   D3DKMT_HANDLE destination = writeBack ? surface->allocation : surface->staging;
   HRESULT hr = QcdxCopyVidmmSurface(&dev->device, source, destination);
   QcdxTraceDisplay(&dev->device, QCDX_SHARED_COPY, surface->allocation, surface->staging, writeBack, hr);
   return hr;
}

static void *
MapSurface(sw_winsys *ws, sw_displaytarget *dt, unsigned usage)
{
   Device *dev = GetDevice(ws);
   VidmmSurface *surface = reinterpret_cast<VidmmSurface *>(dt);
   HRESULT hr = S_OK;
   void *address = NULL;
   if (!surface->stagingDesc.size) {
      hr = LockAllocation(dev, surface->allocation, usage, &address);
   } else {
      /* WDDM permits the creator to CPU-lock shared allocations. Submit a
       * normal, residency-tracked copy on the device which opened this one. */
      AcquireSRWLockExclusive(&surface->mapLock);
      if (!surface->mapCount) {
         if (!surface->staging)
            hr = CreateStaging(dev, surface);
         if (SUCCEEDED(hr) && !surface->copyPending)
            hr = CopyStaging(dev, surface, false);
         if (SUCCEEDED(hr)) {
            unsigned access = PIPE_MAP_READ_WRITE | (usage & PIPE_MAP_DONTBLOCK);
            hr = LockAllocation(dev, surface->staging, access, &surface->mapped);
            surface->copyPending = (usage & PIPE_MAP_DONTBLOCK) &&
               (hr == DXGI_DDI_ERR_WASSTILLDRAWING || hr == D3DDDIERR_WASSTILLDRAWING);
         }
      }
      if (SUCCEEDED(hr)) {
         ++surface->mapCount;
         surface->dirty |= !!(usage & PIPE_MAP_WRITE);
         address = surface->mapped;
      }
      ReleaseSRWLockExclusive(&surface->mapLock);
   }
   if (FAILED(hr))
      dev->UMCallbacks.pfnSetErrorCb(dev->hRTCoreLayer, hr);
   return address;
}

static void
UnmapSurface(sw_winsys *ws, sw_displaytarget *dt)
{
   Device *dev = GetDevice(ws);
   VidmmSurface *surface = reinterpret_cast<VidmmSurface *>(dt);
   HRESULT hr = S_OK;
   if (!surface->stagingDesc.size) {
      hr = UnlockAllocation(dev, surface->allocation);
   } else {
      AcquireSRWLockExclusive(&surface->mapLock);
      if (!surface->mapCount) {
         hr = E_UNEXPECTED;
      } else if (!--surface->mapCount) {
         surface->mapped = NULL;
         hr = UnlockAllocation(dev, surface->staging);
         if (SUCCEEDED(hr) && surface->dirty) {
            hr = CopyStaging(dev, surface, true);
            if (SUCCEEDED(hr)) {
               /* The copy reads staging. An exclusive CPU lock waits for it
                * before publishing the writes to another device or process. */
               void *address = NULL;
               hr = LockAllocation(dev, surface->staging, PIPE_MAP_WRITE, &address);
               if (SUCCEEDED(hr)) hr = UnlockAllocation(dev, surface->staging);
            }
         }
         surface->dirty = false;
      }
      ReleaseSRWLockExclusive(&surface->mapLock);
   }
   if (FAILED(hr))
      dev->UMCallbacks.pfnSetErrorCb(dev->hRTCoreLayer, hr);
}

static void
DestroySurface(sw_winsys *ws, sw_displaytarget *dt)
{
   VidmmSurface *surface = reinterpret_cast<VidmmSurface *>(dt);
   gdikmt_device *device = &GetDevice(ws)->device.base;
   if (surface->staging)
      device->destroyAllocation(device, NULL, surface->staging);
   if (surface->privateAllocation)
      device->destroyAllocation(device, NULL, surface->allocation);
   free(surface);
}

extern "C" sw_winsys *
qcdx_create_vidmm_winsys(gdikmt_device *base, unsigned features)
{
   auto *device = reinterpret_cast<gdikmt_device_d3dddi *>(base);
   if (!device->KTCallbacks.pfnLockCb || !device->KTCallbacks.pfnUnlockCb ||
       !device->KTCallbacks.pfnAllocateCb || !device->KTCallbacks.pfnDeallocateCb)
      return NULL;

   Device *owner = CONTAINING_RECORD(device, Device, device);
   if (((features & QCDX_CAP_VIDMM_COPY) && !device->KTCallbacks.pfnRenderCb) ||
       !owner->UMCallbacks.pfnSetErrorCb || !device->KTCallbacks.pfnCreateContextCb ||
       !device->KTCallbacks.pfnDestroyContextCb || !device->pDXGIBaseCallbacks ||
       !device->pDXGIBaseCallbacks->pfnPresentCb)
      return NULL;
   VidmmWinsys *ws = (VidmmWinsys *)calloc(1, sizeof(*ws));
   if (!ws)
      return NULL;
   ws->device = owner;
   ws->stageImports = !!(features & QCDX_CAP_VIDMM_COPY);
   ws->base.unmap_on_flush = true;
   ws->base.destroy = [](sw_winsys *w) { free(w); };
   ws->base.is_displaytarget_format_supported = FormatSupported;
   ws->base.displaytarget_create = CreateSurface;
   ws->base.displaytarget_destroy = DestroySurface;
   ws->base.displaytarget_from_handle = FromHandle;
   ws->base.displaytarget_get_handle = GetHandle;
   ws->base.displaytarget_map = MapSurface;
   ws->base.displaytarget_unmap = UnmapSurface;
   return &ws->base;
}

HRESULT
QcdxWddmRotateVidmm(Device *device, const DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   // Existing D3D views keep their texture. Exchange storage only after all
   // cached tiles and lock callbacks for the previous frame have retired.
   for (UINT i = 0; i < args->Resources; i++) {
      Resource *r = CastResource(args->pResources[i]);
      if (!r || !r->hAllocation || !r->resource || !softpipe_resource(r->resource)->dt ||
          r->NumSubResources != 1 || r->transfers[0] ||
          r->resource->width0 != CastResource(args->pResources[0])->resource->width0 ||
          r->resource->height0 != CastResource(args->pResources[0])->resource->height0 ||
          QcdxD3dFormat(r->resource->format) != QcdxD3dFormat(CastResource(args->pResources[0])->resource->format))
         return E_INVALIDARG;
   }
   device->pipe->flush(device->pipe, NULL, 0);
   for (UINT i = 1; i < args->Resources; i++) {
      Resource *left = CastResource(args->pResources[i - 1]);
      Resource *right = CastResource(args->pResources[i]);
      struct softpipe_resource *a = softpipe_resource(left->resource);
      struct softpipe_resource *b = softpipe_resource(right->resource);
      std::swap(a->dt, b->dt);
      std::swap(a->stride[0], b->stride[0]);
      std::swap(a->img_stride[0], b->img_stride[0]);
      std::swap(left->hAllocation, right->hAllocation);
      a->timestamp++;
      b->timestamp++;
   }
   return S_OK;
}
