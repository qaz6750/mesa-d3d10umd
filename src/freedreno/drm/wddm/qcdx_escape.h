/*
 * Escape protocol between the freedreno WDDM shim (Mesa UMD) and the qcdxkm WDDM KMD.
 * One escape = one Linux msm DRM ioctl: the KMD implements the msm driver's ioctls
 * (include/drm-uapi/msm_drm.h) plus the generic GEM ones, with the same structs.
 * Keep the copies in qcdxkm and Mesa identical.
 */
#ifndef QCDX_ESCAPE_H
#define QCDX_ESCAPE_H

#define QCDX_ESC_MAGIC 0x55504754u /* 'QCDX' */
#define QCDX_ESC_MAX_DATA 1024

/* nr = DRM ioctl number: DRM_COMMAND_BASE (0x40) + DRM_MSM_* or the generic DRM ones below,
   or one of the qcdxkm extras. */
#define QCDX_NR_GEM_CLOSE 0x09 /* struct drm_gem_close */
#define QCDX_NR_GEM_FLINK 0x0a /* struct drm_gem_flink */
#define QCDX_NR_GEM_OPEN 0x0b  /* struct drm_gem_open */
#define QCDX_NR_VERSION 0x1000 /* struct qcdx_version */
#define QCDX_NR_MMAP 0x1001    /* struct qcdx_mmap: map a BO into the caller */
#define QCDX_NR_MUNMAP 0x1002  /* struct qcdx_mmap */
#define QCDX_NR_CAPABILITIES 0x1003 /* struct qcdx_capabilities */
#define QCDX_NR_CANCEL_SUBMIT 0x1004
#define QCDX_NR_DISPLAY_TRACE 0x1005
#define QCDX_NR_BO_LAYOUT 0x1006
#define QCDX_NR_CAPTURE_GPUVA 0x1007
#define QCDX_RENDER_SUBMIT 1u
/* Full linear copy. The allocation list contains the source, then a writable
   destination. VidMM supplies the device-specific handles and residency. */
#define QCDX_RENDER_COPY_SURFACE 2u
/* Full native 32-bit fill. One writable allocation, color in the last dword. */
#define QCDX_RENDER_FILL_SURFACE 3u
#define QCDX_RENDER_COPY_REGION 4u
#define QCDX_RENDER_SUBMIT_VIDMM 5u
#define QCDX_RENDER_SUBMIT_GPUVA 6u

/* Submitted after a render ticket for a linear VidMM-owned surface. The
   runtime supplies the allocation handle and access rights separately. */
#define QCDX_MAX_SUBMIT_BOS 1024u
struct qcdx_vidmm_operand {
    unsigned allocation_index, buffer_index, pitch, offset;
};

/* SubmitCommand private data follows the ticket with these virtual surface
   operands. The kernel resolves each address through the submitting context. */
struct qcdx_gpuva_operand {
    unsigned long long address;
    unsigned buffer_index, pitch, offset, reserved;
};

enum qcdx_display_event {
    QCDX_MODE_BEGIN, QCDX_MODE_RESULT, QCDX_PRESENT_BEGIN, QCDX_PRESENT_CB,
    QCDX_PRESENT_RESULT, QCDX_ALLOCATE_CB, QCDX_RESOLVE_SHARED, QCDX_CREATE_RESOURCE,
    QCDX_OPEN_RESOURCE, QCDX_LOCK_CB, QCDX_UNLOCK_CB, QCDX_ROTATE, QCDX_BLT,
    QCDX_SHARED_COPY, QCDX_CREATE_DEVICE, QCDX_DISPLAY_EVENT_COUNT
};

struct qcdx_display_trace {
    unsigned event, allocation, detail, flags, hr;
};

struct qcdx_render_command
{
    unsigned magic;
    unsigned operation;
    unsigned fence;
    union {
        unsigned reserved;
        unsigned color; /* Native pixel for QCDX_RENDER_FILL_SURFACE. */
    };
};

/* Coordinates are in pixels. Both operands are supplied by VidMM. */
struct qcdx_copy_region
{
    unsigned src_x, src_y, dst_x, dst_y, width, height;
};

struct qcdx_region_command
{
    struct qcdx_render_command header;
    struct qcdx_copy_region region;
};

#define QCDX_ABI_MAJOR 1u
#define QCDX_ABI_MINOR 1u
#define QCDX_CAP_GEM 0x01u
#define QCDX_CAP_GPU_VA 0x02u
#define QCDX_CAP_SUBMIT 0x04u
#define QCDX_CAP_HW_FENCE 0x08u
#define QCDX_CAP_CPU_SYNC 0x10u
#define QCDX_CAP_WDDM_ALLOCATIONS 0x20u
#define QCDX_CAP_RENDER_REQUIRED 0x3fu
/* VidMM-backed linear surfaces can be locked by the UMD and presented by the KMD. */
#define QCDX_CAP_VIDMM_PRESENT 0x40u
#define QCDX_CAP_VIDMM_COPY 0x80u
#define QCDX_CAP_VIDMM_2D 0x100u
#define QCDX_CAP_VIDMM_COPY_REGION 0x200u
/* A live GEM handle retains its backing across cache DONTNEED/WILLNEED. */
#define QCDX_CAP_GEM_RETAINED 0x400u
#define QCDX_CAP_VIDMM_GPUVA 0x800u
enum { QCDX_CAP_DIRECT_GPUVA = 0x1000u, QCDX_CAP_PRIVATE_SUBMIT = 0x2000u };

/* CAPTURE_GPUVA extends drm_msm_gem_submit with this trailer. Flush counts
   are DDI calls since the previous capture on this device. private_bos is
   set by the KMD only after inspecting every captured binding. */
enum qcdx_flush_reason {
    QCDX_FLUSH_PRESENT, QCDX_FLUSH_RESOURCE_SYNC, QCDX_FLUSH_MAP_UPDATE,
    QCDX_FLUSH_RUNTIME, QCDX_FLUSH_ROTATE, QCDX_FLUSH_COUNT
};

struct qcdx_capture_info {
    unsigned flushes[QCDX_FLUSH_COUNT];
    unsigned private_bos;
};

struct qcdx_capabilities
{
    unsigned size;
    unsigned abi_major;
    unsigned abi_minor;
    unsigned gpu_id;
    unsigned chip_id;
    unsigned gmem_size;
    unsigned features;
    unsigned reserved[5];
};

struct qcdx_adapter_info {
    unsigned magic, features;
    unsigned long long minimum_address;
};

/* Extended UMDRIVERPRIVATE reply. The original 4- and 16-byte queries
   retain their layout. The LUID identifies this adapter to the Vulkan ICD. */
struct qcdx_adapter_info_luid {
    struct qcdx_adapter_info base;
    unsigned luid_low;
    int luid_high;
};

struct qcdx_bo_layout {
    unsigned handle, bytes, read_only, reserved;
};

struct qcdx_version
{
    int  major, minor, patch;
    char name[16]; /* "msm" */
};

struct qcdx_mmap
{
    unsigned long long offset; /* MSM_INFO_GET_OFFSET token */
    unsigned long long size;
    unsigned long long addr; /* out: user VA (MMAP), in: VA (MUNMAP) */
};

struct qcdx_escape
{
    unsigned      magic;
    unsigned      nr;
    unsigned      size; /* bytes of data[] */
    int           ret;  /* out: 0 or -errno */
    unsigned char data[QCDX_ESC_MAX_DATA];
};

/* Allocation private data exchanged by the display miniport and user-mode driver.
   A nonzero BO identifies Freedreno storage. A zero BO identifies a VidMM-backed
   surface, created by either the runtime or the UMD. */
#define QCDX_ALLOC_MAGIC 0x41475054u /* 'TPGA' */
#define QCDX_ALLOC_UMD 0             /* kind: resource of the UMD */
#define QCDX_ALLOC_SHAREDPRIMARY 1   /* = D3DKMDT_STANDARDALLOCATION_* */
#define QCDX_ALLOC_SHADOW 2
#define QCDX_ALLOC_STAGING 3
#define QCDX_ALLOC_GDI 4
#define QCDX_ALLOC_F_PRIMARY 0x1
#define QCDX_ALLOC_F_SRGB 0x2
/* Dedicated linear VidMM allocation covering the entire texture BO. */
#define QCDX_ALLOC_F_DIRECT 0x4
#define QCDX_ALLOC_F_GPUVA_BACKING 0x8

struct qcdx_alloc
{
    unsigned magic;
    unsigned kind;
    unsigned width, height, pitch; /* pixels / bytes */
    unsigned format;               /* D3DDDIFORMAT (21 = A8R8G8B8, 22 = X8R8G8B8) */
    unsigned bo;                   /* BO name with the pixels, 0 = VidMM memory */
    unsigned bo_offset;
    unsigned size;  /* bytes */
    unsigned vidpn; /* primary: VidPnSourceId */
    unsigned flags; /* QCDX_ALLOC_F_* */
    unsigned reserved[5];
};

#endif
