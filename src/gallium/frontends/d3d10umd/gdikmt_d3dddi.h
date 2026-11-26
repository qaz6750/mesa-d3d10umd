#include "gdikmt/gdikmt.h"
#include "util/list.h"
#include <d3d10umddi.h>
#include "freedreno/drm/wddm/qcdx_escape.h"

struct qcdx_shared_allocation {
   list_head link;
   D3DKMT_HANDLE handle;
   unsigned boName;
   unsigned bytes;
   bool direct; /* Dedicated linear allocation with an identical client layout. */
   unsigned vidmmPitch; /* Nonzero for canonical VidMM storage. */
   unsigned gpuPitch, gpuOffset;
   uint64_t systemMemoryMapping; /* Creator mapping passed to AllocateCb. */
};

struct gdikmt_device_d3dddi {  
   gdikmt_device base;

   HANDLE hRTAdapter;
   HANDLE hRTDevice;
   
   D3DDDI_DEVICECALLBACKS KTCallbacks;
   D3DDDI_ADAPTERCALLBACKS *pAdapterCallbacks;
   DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;

   UINT allocationVidPn;
   boolean isPrimary;
   volatile LONG displayTraceMask;
   HANDLE hRTResource;

   const D3D10DDIARG_OPENRESOURCE *pOpenResource;
   list_head sharedAllocations;
   bool gpuVa, directGpuVa, privateSubmit;
   volatile LONG flushes[QCDX_FLUSH_COUNT];
   volatile LONG deviceLoss; /* First GPUVA device-loss HRESULT from the submit worker. */
   uint64_t minimumGpuAddress;
   list_head virtualBuffers[64];
   struct VirtualPacket *unretiredPacket;
   D3DDDICB_CREATEPAGINGQUEUE paging;
   HANDLE virtualContext;
   SRWLOCK qcdxSubmitLock;
   struct gdikmt_context *qcdxSubmitContext;
};


struct gdikmt_context_d3dddi {
    struct gdikmt_context base;

    HANDLE hContext;
};

static inline void
QcdxCountFlush(gdikmt_device_d3dddi *device, qcdx_flush_reason reason)
{
   if (device->privateSubmit) InterlockedIncrement(&device->flushes[reason]);
}

void gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device);
HRESULT QcdxGpuVaDeviceStatus(gdikmt_device_d3dddi *device, HRESULT result);
void QcdxGpuVaInvalidateShared(gdikmt_device_d3dddi *device, unsigned name);
void QcdxGpuVaRotateShared(gdikmt_device_d3dddi *device, unsigned name);
HRESULT QcdxGpuVaAddress(gdikmt_device_d3dddi *device, unsigned name, uint64_t *address);
HRESULT QcdxGpuVaSubmit(gdikmt_device_d3dddi *device, const struct drm_msm_gem_submit *batch, bool privateBos);
HRESULT QcdxGpuVaDrain(gdikmt_device_d3dddi *device);
void QcdxGpuVaClose(gdikmt_device_d3dddi *device, unsigned name);
void QcdxGpuVaStop(gdikmt_device_d3dddi *device);
void QcdxTraceDisplay(gdikmt_device_d3dddi *device, qcdx_display_event event,
                      UINT allocation, UINT detail, UINT flags, HRESULT hr);
