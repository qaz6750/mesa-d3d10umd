#include "gdikmt/gdikmt.h"
#include "util/list.h"
#include <d3d10umddi.h>
#include "freedreno/drm/wddm/qcdx_escape.h"

struct qcdx_shared_allocation {
   list_head link;
   D3DKMT_HANDLE handle;
   unsigned boName;
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
   SRWLOCK qcdxSubmitLock;
   struct gdikmt_context *qcdxSubmitContext;
};


struct gdikmt_context_d3dddi {
    struct gdikmt_context base;

    HANDLE hContext;
};

void gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device);
void QcdxTraceDisplay(gdikmt_device_d3dddi *device, qcdx_display_event event,
                      UINT allocation, UINT detail, UINT flags, HRESULT hr);
