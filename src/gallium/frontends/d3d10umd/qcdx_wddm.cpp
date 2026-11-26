/*
 * A shared or presentable D3D resource has a WDDM allocation.
 * Single-surface shared and presentable textures use VidMM storage with a linear Freedreno mirror.
 * DXGI presents pass the runtime allocation handles.
 */

#include <stdarg.h>

#include "Device.h"
#include "DxgiFns.h"
#include "State.h"
#include "Debug.h"
#include "Format.h"

#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/os_time.h"

#include "../../../freedreno/drm/wddm/qcdx_escape.h"

unsigned
QcdxD3dFormat(enum pipe_format f)
{
   switch (f) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
      return 21;                                   /* D3DDDIFMT_A8R8G8B8 */
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_SRGB:
      return 22;                                   /* D3DDDIFMT_X8R8G8B8 */
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_SRGB:
      return 32;                                   /* D3DDDIFMT_A8B8G8R8 */
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      return 31;                                   /* D3DDDIFMT_A2B10G10R10 */
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      return 113;                                  /* D3DDDIFMT_A16B16G16R16F */
   default:
      return 0;
   }
}

static enum pipe_format
TopazPipeFormat(unsigned d3d)
{
   switch (d3d) {
   case 22:
      return PIPE_FORMAT_B8G8R8X8_UNORM;
   case 32:
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   case 21:
      return PIPE_FORMAT_B8G8R8A8_UNORM;
   case 31:
      return PIPE_FORMAT_R10G10B10A2_UNORM;
   case 113:
      return PIPE_FORMAT_R16G16B16A16_FLOAT;
   default:
      return PIPE_FORMAT_NONE;
   }
}

bool
QcdxWddmWanted(const D3D10DDIARG_CREATERESOURCE *pCreate)
{
   return pCreate->pPrimaryDesc != NULL ||
          (pCreate->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
          (pCreate->BindFlags & D3D10_DDI_BIND_PRESENT);
}

void
QcdxWddmTemplate(struct pipe_resource *templat)
{
   /* the KMD reads/writes these pixels with the CPU: no UBWC/tiling */
   templat->bind |= PIPE_BIND_LINEAR | PIPE_BIND_SHARED;
}

static void
TrackSharedAllocation(Device *device, Resource *resource, D3DKMT_HANDLE handle, unsigned boName)
{
   resource->shared.handle = handle;
   resource->shared.boName = boName;
   AcquireSRWLockExclusive(&device->device.qcdxSubmitLock);
   list_addtail(&resource->shared.link, &device->device.sharedAllocations);
   ReleaseSRWLockExclusive(&device->device.qcdxSubmitLock);
}

static void
ReleaseSystemMemoryMapping(Device *device, uint64_t address)
{
   qcdx_mmap memory = {};
   memory.addr = address;
   qcdx_escape request = {};
   request.magic = QCDX_ESC_MAGIC;
   request.nr = QCDX_NR_MUNMAP;
   request.size = sizeof(memory);
   request.ret = -1;
   memcpy(request.data, &memory, sizeof(memory));
   NTSTATUS status = device->device.base.escape(&device->device.base, &request, sizeof(request));
   if (status != STATUS_SUCCESS || request.ret)
      QcdxWddmLogf("Release shared BO mapping: status 0x%08lx ret %d\n", (unsigned long)status, request.ret);
}

HRESULT
QcdxWddmCreateAllocation(Device *pDevice, Resource *pResource, const D3D10DDIARG_CREATERESOURCE *pCreate,
                          bool noScanout)
{
   struct pipe_screen *screen = pDevice->screen;
   struct pipe_resource *res = pResource->resource;
   struct winsys_handle wh;
   struct qcdx_alloc a;
   D3DDDI_ALLOCATIONINFO ai;
   struct gdikmt_createallocation opts;
   NTSTATUS st;

   const bool scanout = pCreate->pPrimaryDesc && !noScanout;
   // A shared allocation must be openable outside the creating process.
   // Give VidMM ownership of its pixels even when DXGI never presents it.
   const bool shared = (pCreate->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0;
   const bool presentable = (pCreate->BindFlags & D3D10_DDI_BIND_PRESENT) != 0;
   const bool singleSurface = res->target == PIPE_TEXTURE_2D && res->array_size == 1 &&
                              res->last_level == 0 && res->nr_samples <= 1;
   const bool vidmm = scanout || ((shared || presentable) && singleSurface);
   memset(&wh, 0, sizeof(wh));
   wh.type = WINSYS_HANDLE_TYPE_SHARED;
   if (!screen->resource_get_handle(screen, pDevice->pipe, res, &wh,
                                    vidmm ? PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE : 0)) {
      QcdxWddmLogf("topaz: resource_get_handle failed\n");
      return E_OUTOFMEMORY;
   }

   memset(&a, 0, sizeof(a));
   a.magic = QCDX_ALLOC_MAGIC;
   a.kind = QCDX_ALLOC_UMD;
   a.width = res->width0;
   a.height = res->height0;
   a.pitch = wh.stride;
   a.format = QcdxD3dFormat(res->format);
   if (!a.format || (uint64_t)wh.stride * res->height0 > UINT_MAX)
      return DXGI_DDI_ERR_UNSUPPORTED;
   if (util_format_is_srgb(res->format))
      a.flags |= QCDX_ALLOC_F_SRGB;
   if (!vidmm) {
      a.bo = (unsigned)(uintptr_t)wh.handle;
      a.bo_offset = wh.offset;
   }
   a.size = wh.stride * res->height0;
   if (vidmm && singleSurface && wh.modifier == DRM_FORMAT_MOD_LINEAR && !wh.offset &&
       wh.size >= a.size && wh.size <= 64u * 1024 * 1024 && !(wh.size & 4095u)) {
      a.size = static_cast<unsigned>(wh.size);
      a.flags |= QCDX_ALLOC_F_DIRECT;
   }
   if (scanout) {
      a.flags |= QCDX_ALLOC_F_PRIMARY;
      a.vidpn = pCreate->pPrimaryDesc->VidPnSourceId;
      pDevice->device.allocationVidPn = pCreate->pPrimaryDesc->VidPnSourceId;
   }

   memset(&ai, 0, sizeof(ai));
   struct qcdx_escape mapping = {};
   struct qcdx_mmap memory = {};
   if (!a.size || (a.bo_offset & 4095u))
      return E_INVALIDARG;
   if (!vidmm) {
      memory.offset = (unsigned long long)a.bo << 12;
      memory.size = ((unsigned long long)a.bo_offset + a.size + 4095u) & ~4095ull;
      mapping.magic = QCDX_ESC_MAGIC;
      mapping.nr = QCDX_NR_MMAP;
      mapping.size = sizeof(memory);
      mapping.ret = -1;
      memcpy(mapping.data, &memory, sizeof(memory));
      st = pDevice->device.base.escape(&pDevice->device.base, &mapping, sizeof(mapping));
      if (st != STATUS_SUCCESS || mapping.ret != 0)
         return E_OUTOFMEMORY;
      memcpy(&memory, mapping.data, sizeof(memory));
      if (!memory.addr)
         return E_OUTOFMEMORY;
      ai.pSystemMem = (const void *)(uintptr_t)(memory.addr + a.bo_offset);
   }
   ai.pPrivateDriverData = &a;
   ai.PrivateDriverDataSize = sizeof(a);
   memset(&opts, 0, sizeof(opts));
   opts.NumAllocations = 1;
   opts.pAllocationInfo = &ai;
   st = pDevice->device.base.createAllocation(&pDevice->device.base, &opts);
   if (st < 0) {
      if (memory.addr) ReleaseSystemMemoryMapping(pDevice, memory.addr);
      QcdxWddmLogf("AllocateCb failed 0x%08lx, %ux%u pitch=%u vidmm=%u primary=%u\n",
                   (unsigned long)st, a.width, a.height, a.pitch, (unsigned)vidmm, (unsigned)scanout);
      return static_cast<HRESULT>(st);
   }
   pResource->shared.systemMemoryMapping = memory.addr;
   pResource->shared.bytes = a.size;
   if (vidmm) {
      pResource->shared.direct = (a.flags & QCDX_ALLOC_F_DIRECT) != 0;
      pResource->shared.vidmmPitch = a.pitch;
      pResource->shared.gpuPitch = wh.stride;
      pResource->shared.gpuOffset = wh.offset;
   }
   TrackSharedAllocation(pDevice, pResource, ai.hAllocation, (unsigned)(uintptr_t)wh.handle);
   pResource->hRTResource = pDevice->device.hRTResource;
   if (vidmm && !scanout)
      QcdxTraceDisplay(&pDevice->device, QCDX_CREATE_RESOURCE, ai.hAllocation,
                       pCreate->Format, pCreate->BindFlags, S_OK);
   return S_OK;
}

void
QcdxWddmDestroyAllocation(Device *pDevice, Resource *pResource)
{
   if (pResource->shared.link.next) {
      AcquireSRWLockExclusive(&pDevice->device.qcdxSubmitLock);
      list_del(&pResource->shared.link);
      ReleaseSRWLockExclusive(&pDevice->device.qcdxSubmitLock);
   }
   if (pResource->hRTResource || pResource->shared.handle) {
      gdikmt_device *device = &pDevice->device.base;
      HRESULT hr = device->destroyAllocation(device, pResource->hRTResource, pResource->shared.handle);
      if (FAILED(hr)) {
         QcdxWddmLogf("DeallocateCb resource=%p allocation=%x hr=%08lx\n",
                      pResource->hRTResource, pResource->shared.handle, (unsigned long)hr);
         if (hr == D3DDDIERR_DEVICEREMOVED)
            pDevice->UMCallbacks.pfnSetErrorCb(pDevice->hRTCoreLayer, hr);
      } else if (pResource->shared.systemMemoryMapping) {
         ReleaseSystemMemoryMapping(pDevice, pResource->shared.systemMemoryMapping);
      }
   }
   pResource->shared.systemMemoryMapping = 0;
   pResource->hRTResource = NULL;
   pResource->shared.handle = 0;
   pResource->shared.boName = 0;
}

/* Open the storage described by Dxgkrnl for this device. */
HRESULT
QcdxWddmOpen(Device *pDevice, const D3D10DDIARG_OPENRESOURCE *pOpen, Resource *resource)
{
   const struct qcdx_alloc *a;
   struct pipe_resource templat;
   struct winsys_handle wh;

   if (pOpen->NumAllocations != 1 || !pOpen->pOpenAllocationInfo)
      return E_INVALIDARG;
   const D3DDDI_OPENALLOCATIONINFO &info = pOpen->pOpenAllocationInfo[0];
   if (!info.hAllocation || !info.pPrivateDriverData || info.PrivateDriverDataSize < sizeof(*a))
      return E_INVALIDARG;
   a = (const struct qcdx_alloc *)info.pPrivateDriverData;
   if (a->magic != QCDX_ALLOC_MAGIC || a->kind > QCDX_ALLOC_GDI)
      return E_INVALIDARG;
   resource->shared.bytes = a->size;
   enum pipe_format format = TopazPipeFormat(a->format);
   if (format != PIPE_FORMAT_NONE && (a->flags & QCDX_ALLOC_F_SRGB))
      format = util_format_srgb(format);
   if (format == PIPE_FORMAT_NONE)
      return DXGI_DDI_ERR_UNSUPPORTED;
   unsigned pixelBytes = util_format_get_blocksize(format);
   if (!a->width || !a->height || a->pitch % pixelBytes || (uint64_t)a->width * pixelBytes > a->pitch ||
       (uint64_t)a->height * a->pitch > a->size)
      return E_INVALIDARG;
   memset(&templat, 0, sizeof(templat));
   templat.target = PIPE_TEXTURE_2D;
   templat.format = format;
   templat.width0 = a->width;
   templat.height0 = a->height;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.last_level = 0;
   templat.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR | PIPE_BIND_SHARED;
   memset(&wh, 0, sizeof(wh));
   wh.type = WINSYS_HANDLE_TYPE_SHARED;
   // Under GPUVA each import gets a private BO. The batch mirrors the live
   // allocation view when its layout requires a copy.
   if (!a->bo || pDevice->device.gpuVa) {
      if (a->pitch & 31) return DXGI_DDI_ERR_UNSUPPORTED;
      resource->resource = pDevice->screen->resource_create(pDevice->screen, &templat);
      if (!resource->resource) return E_OUTOFMEMORY;
      if (!pDevice->screen->resource_get_handle(pDevice->screen, pDevice->pipe, resource->resource, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE)) {
         pipe_resource_reference(&resource->resource, NULL);
         return E_OUTOFMEMORY;
      }
      resource->shared.direct = (a->flags & QCDX_ALLOC_F_DIRECT) && !a->bo && !a->bo_offset &&
                               wh.modifier == DRM_FORMAT_MOD_LINEAR && wh.size == a->size &&
                               !wh.offset && wh.stride == a->pitch;
      resource->shared.vidmmPitch = a->pitch;
      resource->shared.gpuPitch = wh.stride;
      resource->shared.gpuOffset = wh.offset;
      TrackSharedAllocation(pDevice, resource, info.hAllocation, (unsigned)(uintptr_t)wh.handle);
      return S_OK;
   }
   wh.handle = (HANDLE)(uintptr_t)a->bo;
   wh.stride = a->pitch;
   wh.offset = a->bo_offset;
   resource->resource = pDevice->screen->resource_from_handle(pDevice->screen, &templat, &wh, 0);
   if (!resource->resource)
      return E_OUTOFMEMORY;
   TrackSharedAllocation(pDevice, resource, info.hAllocation, a->bo);
   return S_OK;
}

extern "C" void fd_bo_cache_debug_stats(unsigned *v);
extern "C" void fd_bo_del_debug_stats(unsigned *v);
extern "C" void fd_context_debug_fences(struct pipe_context *pctx, uint32_t *ctl, uint32_t *last);

static HRESULT
FinishFence(Device *device, struct pipe_fence_handle *fence)
{
   if (!fence)
      return E_FAIL;
   struct pipe_screen *screen = device->screen;
   bool completed = screen->fence_finish(screen, device->pipe, fence, OS_TIMEOUT_INFINITE);
   screen->fence_reference(screen, &fence, NULL);
   return completed ? S_OK : E_FAIL;
}

HRESULT
QcdxWddmFinishResource(Device *device, Resource *resource)
{
   if (!resource->resource)
      return E_INVALIDARG;
   struct pipe_context *pipe = device->pipe;
   struct pipe_fence_handle *fence = NULL;
   if (pipe->flush_resource)
      pipe->flush_resource(pipe, resource->resource);
   pipe->flush(pipe, &fence, 0);
   return FinishFence(device, fence);
}

HRESULT APIENTRY
_ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *args)
{
   if (!args || !args->hDevice || !args->hResource)
      return E_INVALIDARG;
   Device *device = CastDevice(args->hDevice);
   Resource *resource = CastResource(args->hResource);
   HRESULT hr = resource->shared.handle ? QcdxWddmFinishResource(device, resource) : E_INVALIDARG;
   QcdxTraceDisplay(&device->device, QCDX_RESOLVE_SHARED, resource->shared.handle, resource->Format, 0, hr);
   return hr;
}

static void
ReportPresentTiming(Device *pDevice)
{
   PresentTiming &timing = pDevice->timing;
   double ms = 1000.0 / timing.frequency.QuadPart;
   uint32_t ctl, last;
   unsigned cs[6], ds[5];
   fd_context_debug_fences(pDevice->pipe, &ctl, &last);
   fd_bo_cache_debug_stats(cs);
   fd_bo_del_debug_stats(ds);
   QcdxWddmLogf("FENCES control %u last %u | BO cache: hits %u miss %u busy-head %u put %u expired %u nobucket %u"
                " | del: suballoc %u nocache %u bo %u ring %u shared/nosync %u\n",
                ctl, last, cs[0], cs[1], cs[2], cs[3], cs[4], cs[5], ds[0], ds[1], ds[2], ds[3], ds[4]);
   QcdxWddmLogf("TIMING 60 presents: frame %.1f ms, flush %.2f, submit wait %.2f, present cb %.2f, "
                "map flushes/frame %.1f (%.2f ms/frame), draws/frame %.0f in %.2f ms\n",
                timing.frameTicks * ms / 59, timing.flushTicks * ms / 60, timing.waitTicks * ms / 60,
                timing.callbackTicks * ms / 60, timing.mapFlushes / 60.0, timing.mapFlushTicks * ms / 60,
                timing.draws / 60.0, timing.drawTicks * ms / 60);
   LARGE_INTEGER frequency = timing.frequency;
   timing = {};
   timing.frequency = frequency;
}

/* Queue the DXGI present behind the draws on their WDDM context. */
HRESULT
QcdxWddmPresent(Device *pDevice, Resource *pSrc, Resource *pDst, void *pDXGIContext)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_fence_handle *fence = NULL;

   if (!pSrc || !pSrc->shared.handle || (pDst && !pDst->shared.handle))
      return E_INVALIDARG;
   PresentTiming &timing = pDevice->timing;
   LARGE_INTEGER begin, flushed, completed;
   if (timing.frequency.QuadPart) QueryPerformanceCounter(&begin);
   pipe->flush(pipe, &fence, 0);
   if (timing.frequency.QuadPart) QueryPerformanceCounter(&flushed);
   if (!fence)
      return QcdxGpuVaDeviceStatus(&pDevice->device, E_FAIL);
   bool submitted = fd_wddm_fence_submitted(pDevice->screen, pipe, fence);
   pDevice->screen->fence_reference(pDevice->screen, &fence, NULL);
   HRESULT result = QcdxGpuVaDeviceStatus(&pDevice->device, submitted ? S_OK : E_FAIL);
   if (FAILED(result)) return result;
   if (timing.frequency.QuadPart) {
      QueryPerformanceCounter(&completed);
      timing.flushTicks += flushed.QuadPart - begin.QuadPart;
      timing.waitTicks += completed.QuadPart - flushed.QuadPart;
      if (timing.previous.QuadPart)
         timing.frameTicks += begin.QuadPart - timing.previous.QuadPart;
      timing.previous = begin;
   }
   // A present without prior drawing may be the first user of this context.
   auto &device = pDevice->device;
   AcquireSRWLockExclusive(&device.qcdxSubmitLock);
   HRESULT create = S_OK;
   if (!device.qcdxSubmitContext)
      create = device.base.createContext(&device.base, &device.qcdxSubmitContext);
   auto *context = (struct gdikmt_context_d3dddi *)device.qcdxSubmitContext;
   ReleaseSRWLockExclusive(&device.qcdxSubmitLock);
   if (FAILED(create))
      return create;
   /* not gdikmt's present(): it never passes a destination; a DXGI blt present with hDstResource
      (DWM to its primary) needs hDstAllocation */
   {
      DXGIDDICB_PRESENT p;
      memset(&p, 0, sizeof(p));
      p.hSrcAllocation = pSrc->shared.handle;
      p.hDstAllocation = pDst ? pDst->shared.handle : 0;
      p.pDXGIContext = pDXGIContext;
      p.hContext = context->hContext;
      LARGE_INTEGER callbackBegin, callbackEnd;
      if (timing.frequency.QuadPart) QueryPerformanceCounter(&callbackBegin);
      HRESULT hr = pDevice->device.pDXGIBaseCallbacks->pfnPresentCb(pDevice->device.hRTDevice, &p);
      if (timing.frequency.QuadPart) {
         QueryPerformanceCounter(&callbackEnd);
         timing.callbackTicks += callbackEnd.QuadPart - callbackBegin.QuadPart;
         if (++timing.frames == 60)
            ReportPresentTiming(pDevice);
      }
      QcdxTraceDisplay(&pDevice->device, QCDX_PRESENT_CB, p.hSrcAllocation, p.hDstAllocation, 0, hr);
      if (FAILED(hr))
         QcdxWddmLogf("topaz: PresentCb src 0x%x dst 0x%x ctx %p -> 0x%08lx\n", p.hSrcAllocation, p.hDstAllocation,
                       p.hContext, (unsigned long)hr);
      return hr;
   }
}

/* Debug log for processes without a console (DWM): if C:\ProgramData\qcdx\umd.log.enable exists,
 * stderr (Mesa's fprintf) and DebugPrintf go to C:\ProgramData\qcdx\umd-<pid>.log; with
 * umd.tgsi also every translated shader is dumped (the last one before a hang is the culprit). */
extern "C" { bool topaz_log_tgsi; }
static bool topaz_log_on;

static BOOL CALLBACK
QcdxInitLogOnce(PINIT_ONCE, PVOID, PVOID *)
{
   char path[MAX_PATH];

   /* C:\ProgramData\qcdx\umd.env: NAME=VALUE lines (e.g. FD_MESA_DEBUG=sysmem) for processes
    * without an environment of ours, like DWM; read before the screen is created */
   {
      FILE *f = fopen("C:\\ProgramData\\qcdx\\umd.env", "r");
      if (f) {
         char line[256];
         while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
               line[--n] = 0;
            if (n && line[0] != '#' && strchr(line, '='))
               _putenv(line);
         }
         fclose(f);
      }
   }
   if (GetFileAttributesA("C:\\ProgramData\\qcdx\\umd.log.enable") == INVALID_FILE_ATTRIBUTES)
      return TRUE;
   snprintf(path, sizeof(path), "C:\\ProgramData\\qcdx\\umd-%lu.log", (unsigned long)GetCurrentProcessId());
   if (freopen(path, "a", stderr)) {
      setvbuf(stderr, NULL, _IONBF, 0);
      topaz_log_on = true;
      topaz_log_tgsi = GetFileAttributesA("C:\\ProgramData\\qcdx\\umd.tgsi") != INVALID_FILE_ATTRIBUTES;
      fprintf(stderr, "==== qcdx UMD log, pid %lu, FD_MESA_DEBUG=%s ====\n", (unsigned long)GetCurrentProcessId(),
              getenv("FD_MESA_DEBUG") ? getenv("FD_MESA_DEBUG") : "");
   }
   return TRUE;
}

void
QcdxWddmLogInit(void)
{
   static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
   InitOnceExecuteOnce(&once, QcdxInitLogOnce, NULL, NULL);
}

extern "C" void
QcdxWddmLog(const char *text)
{
   if (topaz_log_on)
      fputs(text, stderr);
}

extern "C" void
QcdxWddmLogf(const char *fmt, ...)
{
   va_list ap;

   if (!topaz_log_on)
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}
