/*
 * Escape protocol between the freedreno WDDM shim (Mesa UMD) and the qcdxkm WDDM KMD.
 * One escape = one Linux msm DRM ioctl: the KMD implements the msm driver's ioctls
 * (include/drm-uapi/msm_drm.h) plus the generic GEM ones, with the same structs.
 * Shared by both sides (matching copies in drivers/qcdxkm and mesa-overlay).
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
#define QCDX_RENDER_SUBMIT 1u

struct qcdx_render_command
{
    unsigned magic;
    unsigned operation;
    unsigned fence;
    unsigned reserved;
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

/* Allocation private driver data (D3DDDI_ALLOCATIONINFO.pPrivateDriverData), step C2: every
   allocation the UMD creates through pfnAllocateCb (swapchain/shared/primary resources) names the
   BO that holds its pixels; standard allocations created by Dxgkrnl (shared primary, shadow,
   staging, GDI) get the same struct from DxgkDdiGetStandardAllocationDriverData with bo = 0 and
   live in VidMM's aperture backing. */
#define QCDX_ALLOC_MAGIC 0x41475054u /* 'TPGA' */
#define QCDX_ALLOC_UMD 0             /* kind: resource of the UMD */
#define QCDX_ALLOC_SHAREDPRIMARY 1   /* = D3DKMDT_STANDARDALLOCATION_* */
#define QCDX_ALLOC_SHADOW 2
#define QCDX_ALLOC_STAGING 3
#define QCDX_ALLOC_GDI 4
#define QCDX_ALLOC_F_PRIMARY 0x1

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
