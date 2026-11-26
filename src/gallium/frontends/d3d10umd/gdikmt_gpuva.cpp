#include "State.h"
#include "drm-uapi/msm_drm.h"
#include <cstdlib>
#include <cstring>

/* The submit lock protects private allocations, the paging queue and the
 * context. A BO keeps one VidMm address until its final GEM_CLOSE. */
struct VirtualBuffer {
   list_head link;
   unsigned name, bytes;
   bool readOnly;
   D3DKMT_HANDLE allocation;
   uint64_t address, cpu;
};

HRESULT
QcdxGpuVaDeviceStatus(gdikmt_device_d3dddi *device, HRESULT result)
{
   if (result == D3DDDIERR_DEVICEREMOVED) result = DXGI_ERROR_DEVICE_REMOVED;
   if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG)
      InterlockedCompareExchange(&device->deviceLoss, result, S_OK);
   HRESULT loss = ReadAcquire(&device->deviceLoss);
   return FAILED(loss) ? loss : result;
}

template<typename T>
static HRESULT
GemEscape(gdikmt_device_d3dddi *device, unsigned nr, T &data)
{
   static_assert(sizeof(T) <= QCDX_ESC_MAX_DATA, "escape payload");
   qcdx_escape packet = {};
   packet.magic = QCDX_ESC_MAGIC;
   packet.nr = nr;
   packet.size = sizeof(T);
   packet.ret = -1;
   memcpy(packet.data, &data, sizeof(T));
   D3DDDICB_ESCAPE call = {};
   call.hDevice = device->hRTDevice;
   if (nr == DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE) call.hContext = device->virtualContext;
   call.pPrivateDriverData = &packet;
   call.PrivateDriverDataSize = sizeof(packet);
   HRESULT hr = device->KTCallbacks.pfnEscapeCb(device->hRTAdapter, &call);
   if (FAILED(hr)) return hr;
   if (packet.ret == -19) return DXGI_ERROR_DEVICE_REMOVED;
   if (packet.ret) return packet.ret == -11 ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL;
   memcpy(&data, packet.data, sizeof(T));
   return S_OK;
}

static HRESULT
EnsureContext(gdikmt_device_d3dddi *device)
{
   auto &cb = device->KTCallbacks;
   if (device->virtualContext) return S_OK;
   if (!cb.pfnCreatePagingQueueCb || !cb.pfnDestroyPagingQueueCb || !cb.pfnMapGpuVirtualAddressCb ||
       !cb.pfnFreeGpuVirtualAddressCb || !cb.pfnMakeResidentCb || !cb.pfnEvictCb ||
       !cb.pfnWaitForSynchronizationObjectFromCpuCb || !cb.pfnCreateContextVirtualCb ||
       !cb.pfnSubmitCommandCb || !cb.pfnAllocateCb || !cb.pfnDeallocate2Cb || !cb.pfnDestroyContextCb) return E_NOINTERFACE;
   HRESULT hr = cb.pfnCreatePagingQueueCb(device->hRTDevice, &device->paging);
   if (FAILED(hr)) return hr;
   D3DDDICB_CREATECONTEXTVIRTUAL context = {};
   context.EngineAffinity = 1;
   hr = cb.pfnCreateContextVirtualCb(device->hRTDevice, &context);
   if (SUCCEEDED(hr)) device->virtualContext = context.hContext;
   else {
      D3DDDI_DESTROYPAGINGQUEUE destroy = {device->paging.hPagingQueue};
      cb.pfnDestroyPagingQueueCb(device->hRTDevice, &destroy);
      device->paging = {};
   }
   return hr;
}

static HRESULT
WaitPaging(gdikmt_device_d3dddi *device, HRESULT hr, UINT64 value)
{
   if (hr != E_PENDING) return hr;
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &device->paging.hSyncObject;
   wait.FenceValueArray = &value;
   return device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb(device->hRTDevice, &wait);
}

static HRESULT
MapAddress(gdikmt_device_d3dddi *device, D3DKMT_HANDLE allocation, unsigned bytes,
           bool write, bool execute, uint64_t *address, uint64_t base = 0)
{
   D3DDDI_MAPGPUVIRTUALADDRESS map = {};
   map.hPagingQueue = device->paging.hPagingQueue;
   map.hAllocation = allocation;
   map.BaseAddress = base;
   map.MinimumAddress = base ? 0 : device->minimumGpuAddress;
   map.SizeInPages = (static_cast<uint64_t>(bytes) + 4095) / 4096;
   map.Protection.Write = write;
   map.Protection.Execute = execute;
   HRESULT hr = device->KTCallbacks.pfnMapGpuVirtualAddressCb(device->hRTDevice, &map);
   if (SUCCEEDED(hr) || hr == E_PENDING) *address = map.VirtualAddress;
   hr = WaitPaging(device, hr, map.PagingFenceValue);
   return SUCCEEDED(hr) && !*address ? E_UNEXPECTED : hr;
}

static HRESULT
FreeAddress(gdikmt_device_d3dddi *device, uint64_t address, unsigned bytes)
{
   if (!address) return S_OK;
   D3DDDICB_FREEGPUVIRTUALADDRESS request = {};
   request.BaseAddress = address;
   request.Size = (uint64_t(bytes) + 65535) & ~UINT64_C(65535);
   return device->KTCallbacks.pfnFreeGpuVirtualAddressCb(device->hRTDevice, &request);
}

static void
ReleaseBuffer(gdikmt_device_d3dddi *device, VirtualBuffer *buffer)
{
   HRESULT hr = FreeAddress(device, buffer->address, buffer->bytes);
   if (FAILED(hr))
      QcdxWddmLogf("Free GPUVA %llx failed, hr=%08lx\n", (unsigned long long)buffer->address, (unsigned long)hr);
   if (buffer->allocation) {
      D3DDDICB_DEALLOCATE2 release = {};
      release.Flags.SynchronousDestroy = 1;
      release.NumAllocations = 1;
      release.HandleList = &buffer->allocation;
      if (FAILED(device->KTCallbacks.pfnDeallocate2Cb(device->hRTDevice, &release))) {
         /* The KMD releases outstanding user mappings at device teardown. */
         buffer->cpu = 0;
      }
   }
   if (buffer->cpu) {
      qcdx_mmap mapping = {};
      mapping.addr = buffer->cpu;
      GemEscape(device, QCDX_NR_MUNMAP, mapping);
   }
   free(buffer);
}

static VirtualBuffer *
FindBuffer(gdikmt_device_d3dddi *device, unsigned name)
{
   list_for_each_entry(VirtualBuffer, item, &device->virtualBuffers[name % ARRAY_SIZE(device->virtualBuffers)], link)
      if (item->name == name) return item;
   return nullptr;
}

HRESULT
QcdxGpuVaAddress(gdikmt_device_d3dddi *device, unsigned name, uint64_t *address)
{
   VirtualBuffer *buffer = FindBuffer(device, name);
   if (buffer) { *address = buffer->address; return S_OK; }
   HRESULT hr = EnsureContext(device);
   if (FAILED(hr)) return hr;
   qcdx_bo_layout layout = {name};
   hr = GemEscape(device, QCDX_NR_BO_LAYOUT, layout);
   if (FAILED(hr) || !layout.bytes || (layout.bytes & 4095)) return FAILED(hr) ? hr : E_INVALIDARG;
   buffer = static_cast<VirtualBuffer *>(calloc(1, sizeof(*buffer)));
   if (!buffer) return E_OUTOFMEMORY;
   buffer->name = name;
   buffer->bytes = layout.bytes;
   buffer->readOnly = layout.read_only;
   qcdx_mmap mapping = {};
   mapping.offset = uint64_t(name) << 12;
   mapping.size = layout.bytes;
   hr = GemEscape(device, QCDX_NR_MMAP, mapping);
   if (SUCCEEDED(hr)) {
      buffer->cpu = mapping.addr;
      qcdx_alloc desc = {};
      desc.magic = QCDX_ALLOC_MAGIC;
      desc.kind = QCDX_ALLOC_UMD;
      desc.bo = name;
      desc.size = layout.bytes;
      desc.flags = QCDX_ALLOC_F_GPUVA_BACKING;
      D3DDDI_ALLOCATIONINFO2 info = {};
      info.pSystemMem = reinterpret_cast<void *>(uintptr_t(mapping.addr));
      info.pPrivateDriverData = &desc;
      info.PrivateDriverDataSize = sizeof(desc);
      D3DDDICB_ALLOCATE allocate = {};
      allocate.NumAllocations = 1;
      allocate.pAllocationInfo2 = &info;
      hr = device->KTCallbacks.pfnAllocateCb(device->hRTDevice, &allocate);
      if (SUCCEEDED(hr)) buffer->allocation = info.hAllocation;
   }
   if (SUCCEEDED(hr))
      hr = MapAddress(device, buffer->allocation, layout.bytes, !layout.read_only, true, &buffer->address);
   if (SUCCEEDED(hr)) {
      drm_msm_gem_info bind = {};
      bind.handle = name;
      bind.info = MSM_INFO_SET_IOVA;
      bind.value = buffer->address;
      hr = GemEscape(device, DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, bind);
   }
   if (FAILED(hr)) { ReleaseBuffer(device, buffer); return hr; }
   list_addtail(&buffer->link, &device->virtualBuffers[name % ARRAY_SIZE(device->virtualBuffers)]);
   *address = buffer->address;
   return S_OK;
}

struct VirtualPacket {
   qcdx_render_command ticket;
   qcdx_gpuva_operand surfaces[QCDX_MAX_SUBMIT_BOS];
   struct Mapping {
      unsigned bytes;
      VirtualBuffer *replaced; /* Restore this BO's private mapping after its fence. */
   } mappings[QCDX_MAX_SUBMIT_BOS];
   bool commandBuffers[QCDX_MAX_SUBMIT_BOS];
   UINT residentCount;
   D3DKMT_HANDLE allocations[2 * QCDX_MAX_SUBMIT_BOS];
};

static HRESULT
MapSharedOperand(gdikmt_device_d3dddi *device, VirtualBuffer *buffer, const qcdx_shared_allocation *shared,
                 bool write, VirtualPacket::Mapping &mapping, uint64_t *address)
{
   mapping.bytes = shared->bytes;
   if (device->directGpuVa && shared->direct && buffer->bytes == shared->bytes &&
       !buffer->readOnly && !shared->gpuOffset && shared->gpuPitch == shared->vidmmPitch) {
      HRESULT hr = MapAddress(device, shared->handle, shared->bytes, write, false, address, buffer->address);
      if (*address) {
         mapping.replaced = buffer;
         return *address == buffer->address ? hr : QcdxGpuVaDeviceStatus(device, DXGI_ERROR_DEVICE_REMOVED);
      }
      // Only synchronous rejection leaves the original mapping untouched.
      if (hr != E_INVALIDARG && hr != E_NOTIMPL && hr != DXGI_ERROR_UNSUPPORTED) return hr;
      device->directGpuVa = false;
   }
   return MapAddress(device, shared->handle, shared->bytes, write, false, address);
}

static HRESULT
EvictPacket(gdikmt_device_d3dddi *device, VirtualPacket *packet)
{
   if (!packet->residentCount) return S_OK;
   D3DDDICB_EVICT evict = {};
   evict.NumAllocations = packet->residentCount;
   evict.AllocationList = packet->allocations;
   return device->KTCallbacks.pfnEvictCb(device->hRTDevice, &evict);
}

HRESULT
QcdxGpuVaSubmit(gdikmt_device_d3dddi *device, const drm_msm_gem_submit *batch)
{
   HRESULT loss = ReadAcquire(&device->deviceLoss);
   if (FAILED(loss)) return loss;
   if (!batch->nr_bos || batch->nr_bos > QCDX_MAX_SUBMIT_BOS || !batch->nr_cmds)
      return E_INVALIDARG;
   const auto *bos = reinterpret_cast<const drm_msm_gem_submit_bo *>(uintptr_t(batch->bos));
   const auto *cmds = reinterpret_cast<const drm_msm_gem_submit_cmd *>(uintptr_t(batch->cmds));
   uint32_t first = 0;
   while (first < batch->nr_cmds && cmds[first].type == MSM_SUBMIT_CMD_IB_TARGET_BUF) ++first;
   if (first == batch->nr_cmds || cmds[first].submit_idx >= batch->nr_bos) return E_INVALIDARG;
   VirtualPacket *packet = static_cast<VirtualPacket *>(calloc(1, sizeof(*packet)));
   if (!packet) return E_OUTOFMEMORY;
   for (UINT i = 0; i < batch->nr_cmds; ++i) {
      if (cmds[i].submit_idx >= batch->nr_bos) { free(packet); return E_INVALIDARG; }
      packet->commandBuffers[cmds[i].submit_idx] = true;
   }
   HRESULT hr = S_OK;
   UINT allocations = 0, mapped = 0;
   D3DDDICB_SUBMITCOMMAND submit = {};
   packet->ticket = {QCDX_ESC_MAGIC, QCDX_RENDER_SUBMIT_GPUVA, batch->fence, {0}};
   for (UINT i = 0; SUCCEEDED(hr) && i < batch->nr_bos; ++i) {
      auto *buffer = FindBuffer(device, bos[i].handle);
      if (!buffer) { hr = E_INVALIDARG; break; }
      packet->allocations[allocations++] = buffer->allocation;
      if (i == cmds[first].submit_idx) {
         submit.Commands = buffer->address + cmds[first].submit_offset;
         submit.CommandLength = cmds[first].size;
      }
      list_for_each_entry(qcdx_shared_allocation, shared, &device->sharedAllocations, link) {
         if (shared->boName != buffer->name || !shared->vidmmPitch) continue;
         if (packet->commandBuffers[i] || !shared->bytes) { hr = E_INVALIDARG; break; }
         auto &operand = packet->surfaces[mapped];
         uint64_t address = 0;
         bool write = (bos[i].flags & MSM_SUBMIT_BO_WRITE) != 0;
         auto &mapping = packet->mappings[mapped];
         hr = MapSharedOperand(device, buffer, shared, write, mapping, &address);
         if (address) {
            ++mapped;
            operand = {address, i, shared->gpuPitch, shared->gpuOffset, 0};
         }
         if (FAILED(hr)) break;
         // Only the canonical allocation backs a direct operand during DMA.
         if (mapping.replaced) packet->allocations[allocations - 1] = shared->handle;
         else packet->allocations[allocations++] = shared->handle;
         break;
      }
   }
   if (SUCCEEDED(hr)) {
      D3DDDI_MAKERESIDENT resident = {};
      resident.hPagingQueue = device->paging.hPagingQueue;
      resident.NumAllocations = allocations;
      resident.AllocationList = packet->allocations;
      hr = device->KTCallbacks.pfnMakeResidentCb(device->hRTDevice, &resident);
      if (resident.NumAllocations <= allocations) packet->residentCount = resident.NumAllocations;
      hr = WaitPaging(device, hr, resident.PagingFenceValue);
   }
   if (SUCCEEDED(hr)) {
      packet->ticket.reserved = mapped;
      submit.BroadcastContextCount = 1;
      submit.BroadcastContext[0] = device->virtualContext;
      submit.pPrivateDriverData = packet;
      submit.PrivateDriverDataSize = sizeof(packet->ticket) + mapped * sizeof(packet->surfaces[0]);
      hr = device->KTCallbacks.pfnSubmitCommandCb(device->hRTDevice, &submit);
      if (SUCCEEDED(hr)) {
         drm_msm_wait_fence wait = {};
         wait.fence = batch->fence;
         wait.queueid = batch->queueid;
         do {
            hr = GemEscape(device, DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, wait);
         } while (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
         if (FAILED(hr)) {
            // GPU ownership is uncertain. Device teardown releases these
            // mappings after the scheduler has stopped the context.
            device->unretiredPacket = packet;
            return QcdxGpuVaDeviceStatus(device, DXGI_ERROR_DEVICE_REMOVED);
         }
      }
   }
   HRESULT evicted = EvictPacket(device, packet);
   if (SUCCEEDED(hr)) hr = evicted;
   for (UINT i = 0; i < mapped; ++i) {
      const auto &mapping = packet->mappings[i];
      HRESULT released;
      if (mapping.replaced) {
         VirtualBuffer *bo = mapping.replaced;
         uint64_t restored = 0;
         released = MapAddress(device, bo->allocation, bo->bytes, !bo->readOnly, true, &restored, bo->address);
         if (FAILED(released) || restored != bo->address)
            hr = QcdxGpuVaDeviceStatus(device, DXGI_ERROR_DEVICE_REMOVED);
      } else {
         released = FreeAddress(device, packet->surfaces[i].address, mapping.bytes);
      }
      if (SUCCEEDED(hr)) hr = released;
   }
   free(packet);
   return hr;
}

void
QcdxGpuVaClose(gdikmt_device_d3dddi *device, unsigned name)
{
   if (device->unretiredPacket) return;
   auto *buffer = FindBuffer(device, name);
   if (buffer) { list_del(&buffer->link); ReleaseBuffer(device, buffer); }
}

void
QcdxGpuVaStop(gdikmt_device_d3dddi *device)
{
   HRESULT stopped = S_OK;
   if (device->virtualContext) {
      D3DDDICB_DESTROYCONTEXT destroy = {device->virtualContext};
      stopped = device->KTCallbacks.pfnDestroyContextCb(device->hRTDevice, &destroy);
      device->virtualContext = nullptr;
   }
   if (auto *packet = device->unretiredPacket) {
      if (SUCCEEDED(stopped)) {
         EvictPacket(device, packet);
         for (UINT i = 0; i < packet->ticket.reserved; ++i)
            if (!packet->mappings[i].replaced)
               FreeAddress(device, packet->surfaces[i].address, packet->mappings[i].bytes);
      }
      free(packet);
      device->unretiredPacket = nullptr;
   }
   for (auto &bucket : device->virtualBuffers) {
      list_for_each_entry_safe(VirtualBuffer, item, &bucket, link) {
         list_del(&item->link);
         if (SUCCEEDED(stopped)) ReleaseBuffer(device, item);
         else free(item); // The runtime owns outstanding allocations until device teardown.
      }
   }
   if (device->paging.hPagingQueue) {
      D3DDDI_DESTROYPAGINGQUEUE destroy = {device->paging.hPagingQueue};
      device->KTCallbacks.pfnDestroyPagingQueueCb(device->hRTDevice, &destroy);
      device->paging = {};
   }
}
