#include "gdikmt/gdikmt.h"
#include <d3d10umddi.h>
#include "freedreno/drm/wddm/qcdx_escape.h"

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
   SRWLOCK qcdxSubmitLock;
   struct gdikmt_context *qcdxSubmitContext;
};


struct gdikmt_context_d3dddi {
    struct gdikmt_context base;

    HANDLE hContext;
};

void gdikmt_d3dddi_fill_basefuncs(struct gdikmt_device_d3dddi *device);
HRESULT QcdxSubmitVidmmSurface(gdikmt_device_d3dddi *device, D3DKMT_HANDLE source,
                               D3DKMT_HANDLE destination, unsigned color, const qcdx_copy_region *region = nullptr);
void QcdxTraceDisplay(gdikmt_device_d3dddi *device, qcdx_display_event event,
                      UINT allocation, UINT detail, UINT flags, HRESULT hr);
