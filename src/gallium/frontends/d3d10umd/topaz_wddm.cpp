/*
 * A shared or presentable D3D resource has a WDDM allocation. Software devices
 * use VidMM storage directly. Freedreno BO imports use ExistingSysMem for
 * non-primary surfaces. DXGI presents pass the runtime allocation handles.
 */

#include <stdarg.h>

#include "Device.h"
#include "DxgiFns.h"
#include "State.h"
#include "Debug.h"
#include "Format.h"

#include "frontend/winsys_handle.h"
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

   memset(&wh, 0, sizeof(wh));
   if (pDevice->vidmmWinsys) {
      /* The display-target constructor has already called AllocateCb. */
      pResource->hRTResource = pDevice->device.hRTResource;
      wh.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
      if (!screen->resource_get_handle(screen, pDevice->pipe, res, &wh, 0))
         return E_FAIL;
      pResource->hAllocation = (D3DKMT_HANDLE)(uintptr_t)wh.handle;
      return S_OK;
   }
   wh.type = WINSYS_HANDLE_TYPE_SHARED;
   if (!screen->resource_get_handle(screen, pDevice->pipe, res, &wh, 0)) {
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
   a.bo = (unsigned)(uintptr_t)wh.handle;
   a.bo_offset = wh.offset;
   a.size = wh.stride * res->height0;
   if (pCreate->pPrimaryDesc && !noScanout) {
      a.flags |= QCDX_ALLOC_F_PRIMARY;
      a.vidpn = pCreate->pPrimaryDesc->VidPnSourceId;
      pDevice->device.allocationVidPn = pCreate->pPrimaryDesc->VidPnSourceId;
   }

   memset(&ai, 0, sizeof(ai));
   struct qcdx_escape mapping = {};
   struct qcdx_mmap memory = {};
   if (!a.size || (a.bo_offset & 4095u))
      return E_INVALIDARG;
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
   ai.pPrivateDriverData = &a;
   ai.PrivateDriverDataSize = sizeof(a);
   memset(&opts, 0, sizeof(opts));
   opts.NumAllocations = 1;
   opts.pAllocationInfo = &ai;
   st = pDevice->device.base.createAllocation(&pDevice->device.base, &opts);
   if (st < 0) {
      QcdxWddmLogf("topaz: AllocateCb failed 0x%08lx\n", (unsigned long)st);
      return E_OUTOFMEMORY;
   }
   pResource->hAllocation = ai.hAllocation;
   pResource->hRTResource = pDevice->device.hRTResource;
   return S_OK;
}

void
QcdxWddmDestroyAllocation(Device *pDevice, Resource *pResource)
{
   /* Device-private softpipe allocations are released by DestroySurface. */
   if (pResource->hRTResource || (pResource->hAllocation && !pDevice->vidmmWinsys)) {
      gdikmt_device *device = &pDevice->device.base;
      HRESULT hr = device->destroyAllocation(device, pResource->hRTResource, pResource->hAllocation);
      if (FAILED(hr)) {
         QcdxWddmLogf("DeallocateCb resource=%p allocation=%x hr=%08lx\n",
                      pResource->hRTResource, pResource->hAllocation, (unsigned long)hr);
         if (hr == D3DDDIERR_DEVICEREMOVED)
            pDevice->UMCallbacks.pfnSetErrorCb(pDevice->hRTCoreLayer, hr);
      }
   }
   pResource->hRTResource = NULL;
   pResource->hAllocation = 0;
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
   enum pipe_format format = TopazPipeFormat(a->format);
   if (format != PIPE_FORMAT_NONE && (a->flags & QCDX_ALLOC_F_SRGB))
      format = util_format_srgb(format);
   if (format == PIPE_FORMAT_NONE || (!pDevice->vidmmWinsys && !a->bo))
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
   wh.type = pDevice->vidmmWinsys ? WINSYS_HANDLE_TYPE_D3DKMT_ALLOC : WINSYS_HANDLE_TYPE_SHARED;
   wh.handle = (HANDLE)(uintptr_t)(pDevice->vidmmWinsys ? info.hAllocation : a->bo);
   wh.stride = a->pitch;
   wh.offset = pDevice->vidmmWinsys ? 0 : a->bo_offset;
   resource->resource = pDevice->screen->resource_from_handle(pDevice->screen, &templat, &wh, 0);
   if (!resource->resource)
      return E_OUTOFMEMORY;
   resource->hAllocation = info.hAllocation;
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
   HRESULT hr = resource->hAllocation ? QcdxWddmFinishResource(device, resource) : E_INVALIDARG;
   QcdxTraceDisplay(&device->device, QCDX_RESOLVE_SHARED, resource->hAllocation, resource->Format, 0, hr);
   return hr;
}

/* DXGI Present: finish the GPU work on the source, then let the KMD blt/flip it */
HRESULT
QcdxWddmPresent(Device *pDevice, Resource *pSrc, Resource *pDst, void *pDXGIContext)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_fence_handle *fence = NULL;

   if (!pSrc || !pSrc->hAllocation || (pDst && !pDst->hAllocation))
      return E_INVALIDARG;
   static LARGE_INTEGER qf, tPrev;
   static double sumFrame, sumFlush, sumWait, sumCb;
   static unsigned nFrames;
   static int timing = -1;
   LARGE_INTEGER t0, t1, t2;
   if (timing < 0) {
      timing = getenv("QCDX_TIMING") && getenv("QCDX_TIMING")[0] == '1';
      QueryPerformanceFrequency(&qf);
   }
   QueryPerformanceCounter(&t0);
   pipe->flush(pipe, &fence, 0);
   QueryPerformanceCounter(&t1);
   HRESULT wait = FinishFence(pDevice, fence);
   if (FAILED(wait))
      return wait;
   QueryPerformanceCounter(&t2);
   if (timing) {
      double ms = 1000.0 / (double)qf.QuadPart;
      if (tPrev.QuadPart)
         sumFrame += (t0.QuadPart - tPrev.QuadPart) * ms;
      sumFlush += (t1.QuadPart - t0.QuadPart) * ms;
      sumWait += (t2.QuadPart - t1.QuadPart) * ms;
      tPrev = t0;
      if (++nFrames == 60) {
         extern unsigned g_topazMapFlushes;
         extern long long g_topazMapFlushTicks;
         extern long long g_topazDrawTicks;
         extern unsigned g_topazDrawCount;
         if (!pDevice->vidmmWinsys) {
            uint32_t ctl, last;
            fd_context_debug_fences(pipe, &ctl, &last);
            unsigned cs[6];
            fd_bo_cache_debug_stats(cs);
            unsigned ds[5];
            fd_bo_del_debug_stats(ds);
            QcdxWddmLogf("FENCES control %u last %u | BO cache: hits %u miss %u busy-head %u put %u expired %u nobucket %u"
                          " | del: suballoc %u nocache %u bo %u ring %u shared/nosync %u\n",
                          ctl, last, cs[0], cs[1], cs[2], cs[3], cs[4], cs[5], ds[0], ds[1], ds[2], ds[3], ds[4]);
         }
         QcdxWddmLogf("TIMING 60 presents: frame %.1f ms, flush %.2f, gpu wait %.2f, present cb %.2f, NO_OVERWRITE flushes/frame %.1f (%.2f ms/frame), draws/frame %.0f in %.2f ms\n",
                       sumFrame / 59, sumFlush / 60, sumWait / 60, sumCb / 60, g_topazMapFlushes / 60.0,
                       g_topazMapFlushTicks * ms / 60, g_topazDrawCount / 60.0, g_topazDrawTicks * ms / 60);
         g_topazDrawTicks = 0; g_topazDrawCount = 0;
         nFrames = 0; sumFrame = sumFlush = sumWait = sumCb = 0; g_topazMapFlushes = 0; g_topazMapFlushTicks = 0;
         tPrev.QuadPart = 0;
      }
   }
   if (!pDevice->topazCtx) {
      HRESULT hr = pDevice->device.base.createContext(&pDevice->device.base, &pDevice->topazCtx);
      if (FAILED(hr))
         return hr;
   }
   /* not gdikmt's present(): it never passes a destination; a DXGI blt present with hDstResource
      (DWM to its primary) needs hDstAllocation */
   {
      DXGIDDICB_PRESENT p;
      memset(&p, 0, sizeof(p));
      p.hSrcAllocation = pSrc->hAllocation;
      p.hDstAllocation = pDst ? pDst->hAllocation : 0;
      p.pDXGIContext = pDXGIContext;
      p.hContext = ((struct gdikmt_context_d3dddi *)pDevice->topazCtx)->hContext;
      LARGE_INTEGER c0, c1;
      QueryPerformanceCounter(&c0);
      HRESULT hr = pDevice->device.pDXGIBaseCallbacks->pfnPresentCb(pDevice->device.hRTDevice, &p);
      QueryPerformanceCounter(&c1);
      QcdxTraceDisplay(&pDevice->device, QCDX_PRESENT_CB, p.hSrcAllocation, p.hDstAllocation, 0, hr);
      if (timing)
         sumCb += (c1.QuadPart - c0.QuadPart) * 1000.0 / (double)qf.QuadPart;
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

bool
QcdxWddmNoFlip(void)
{
   return GetFileAttributesA("C:\\qcdx\\umd.noflip") != INVALID_FILE_ATTRIBUTES;
}
