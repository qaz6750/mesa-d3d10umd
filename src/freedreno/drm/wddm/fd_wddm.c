/*
 * libdrm subset for freedreno on Windows: every DRM ioctl becomes one qcdxkm escape
 * (qcdx_escape.h) to the WDDM KMD, which implements the Linux msm driver's ioctls with the
 * same structs. "fds" are small integers handed out by fd_wddm_open() and bound to the escape
 * callback of the D3D10 UMD device (gdikmt).
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "sys/mman.h"
#include "drm-uapi/msm_drm.h"
#include "qcdx_escape.h"

#define FD_WDDM_BASE   0x7000

struct fd_wddm_slot {
   fd_wddm_escape_fn escape;
   void *ctx;
   int refs;
   int handle;
   unsigned active;
   int detaching;
};
static struct fd_wddm_slot *fds;
static int slot_count;
static int next_fd = FD_WDDM_BASE;

/* DWM creates/destroys D3D devices from several threads */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static SRWLOCK fds_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE fds_drained = CONDITION_VARIABLE_INIT;

/* In-flight escapes retain their slot index while a new D3D device can grow
 * the table. Access the array only under fds_lock. */
static int
allocate_slot(fd_wddm_escape_fn escape, void *ctx)
{
   if (next_fd == INT_MAX) {
      errno = EMFILE;
      return -1;
   }
   int i = 0;
   while (i < slot_count && (fds[i].refs || fds[i].active || fds[i].detaching))
      ++i;
   if (i == slot_count) {
      if (slot_count > INT_MAX / 2 || (size_t)slot_count > SIZE_MAX / sizeof(*fds) / 2) {
         errno = EMFILE;
         return -1;
      }
      int capacity = slot_count ? slot_count * 2 : 16;
      struct fd_wddm_slot *slots = realloc(fds, (size_t)capacity * sizeof(*fds));
      if (!slots) {
         errno = ENOMEM;
         return -1;
      }
      fds = slots;
      memset(fds + slot_count, 0, (capacity - slot_count) * sizeof(*fds));
      slot_count = capacity;
   }
   fds[i].escape = escape;
   fds[i].ctx = ctx;
   fds[i].refs = 1;
   fds[i].handle = next_fd++;
   return fds[i].handle;
}

static void
release_empty_table(void)
{
   for (int i = 0; i < slot_count; ++i) {
      if (fds[i].refs || fds[i].active || fds[i].detaching)
         return;
   }
   free(fds);
   fds = NULL;
   slot_count = 0;
}

int
fd_wddm_open(fd_wddm_escape_fn escape, void *ctx)
{
   struct qcdx_escape query = {0};
   struct qcdx_capabilities caps = {0};
   int ret = -1;

   if (!escape) {
      errno = EINVAL;
      return -1;
   }
   caps.size = sizeof(caps);
   caps.abi_major = QCDX_ABI_MAJOR;
   caps.abi_minor = QCDX_ABI_MINOR;
   query.magic = QCDX_ESC_MAGIC;
   query.nr = QCDX_NR_CAPABILITIES;
   query.size = sizeof(caps);
   query.ret = -ENOSYS;
   memcpy(query.data, &caps, sizeof(caps));
   if (escape(ctx, &query, sizeof(query)) != 0 || query.ret != 0 ||
       query.magic != QCDX_ESC_MAGIC || query.nr != QCDX_NR_CAPABILITIES ||
       query.size != sizeof(caps)) {
      errno = ENODEV;
      return -1;
   }
   memcpy(&caps, query.data, sizeof(caps));
   if (caps.size != sizeof(caps) || caps.abi_major != QCDX_ABI_MAJOR ||
       caps.abi_minor != QCDX_ABI_MINOR ||
       caps.gpu_id != 660 || caps.gmem_size != 0x180000 ||
       (caps.chip_id != 0x06060001 && caps.chip_id != 0x06060002)) {
      errno = ENODEV;
      return -1;
   }
   for (unsigned index = 0; index < sizeof(caps.reserved) / sizeof(caps.reserved[0]); index++) {
      if (caps.reserved[index] != 0) {
         errno = ENODEV;
         return -1;
      }
   }
   if ((caps.features & QCDX_CAP_RENDER_REQUIRED) != QCDX_CAP_RENDER_REQUIRED) {
      errno = ENOTSUP;
      return -1;
   }
   AcquireSRWLockExclusive(&fds_lock);
   ret = allocate_slot(escape, ctx);
   ReleaseSRWLockExclusive(&fds_lock);
   return ret;
}

int
fd_wddm_dup(int fd)
{
   int result = -1;
   int original;

   if (fd < FD_WDDM_BASE)
      return _dup(fd);
   AcquireSRWLockExclusive(&fds_lock);
   original = -1;
   for (int index = 0; index < slot_count; index++) {
      if (fds[index].refs > 0 && fds[index].handle == fd) {
         original = index;
         break;
      }
   }
   if (original >= 0)
      result = allocate_slot(fds[original].escape, fds[original].ctx);
   else
      errno = EBADF;
   ReleaseSRWLockExclusive(&fds_lock);
   return result;
}

void
fd_wddm_detach(void *ctx)
{
   int pending;

   AcquireSRWLockExclusive(&fds_lock);
   for (int i = 0; i < slot_count; i++) {
      if (fds[i].ctx == ctx && fds[i].escape) {
         fds[i].detaching = 1;
         fds[i].refs = 0;
      }
   }
   do {
      pending = 0;
      for (int i = 0; i < slot_count; i++) {
         if (fds[i].ctx == ctx && fds[i].detaching && fds[i].active != 0) {
            pending = 1;
            break;
         }
      }
      if (pending)
         SleepConditionVariableSRW(&fds_drained, &fds_lock, INFINITE, 0);
   } while (pending);
   for (int i = 0; i < slot_count; i++) {
      if (fds[i].ctx == ctx && fds[i].detaching) {
         fds[i].escape = NULL;
         fds[i].ctx = NULL;
         fds[i].detaching = 0;
      }
   }
   release_empty_table();
   ReleaseSRWLockExclusive(&fds_lock);
}

static int
slot(int fd)
{
   for (int index = 0; index < slot_count; index++) {
      if (fds[index].refs > 0 && fds[index].handle == fd)
         return index;
   }
   return -1;
}

int
fd_wddm_close(int fd)
{
   int index;

   if (fd < FD_WDDM_BASE)
      return _close(fd);
   AcquireSRWLockExclusive(&fds_lock);
   index = slot(fd);
   if (index >= 0 && --fds[index].refs == 0 && fds[index].active == 0 && !fds[index].detaching) {
      fds[index].escape = NULL;
      fds[index].ctx = NULL;
      release_empty_table();
   }
   ReleaseSRWLockExclusive(&fds_lock);
   if (index < 0) {
      errno = EBADF;
      return -1;
   }
   return 0;
}

/* qcdxkm: QCDX_ESCLOG=1 logs escape counts per DRM command nr once per second (busy-polling hunt) */
extern void QcdxWddmLogf(const char *fmt, ...);
static unsigned esc_count[256];
static ULONGLONG esc_t0;
static int esc_log = -1;
static SRWLOCK esc_stats_lock = SRWLOCK_INIT;

static unsigned esc_new_sz[8][4];   /* GEM_NEW: size class x flags class */

static void
esc_stats_new(const void *data)
{
   const struct drm_msm_gem_new *r = (const struct drm_msm_gem_new *)data;
   unsigned c = r->size <= 4096 ? 0 : r->size <= 16384 ? 1 : r->size <= 65536 ? 2 : r->size <= 262144 ? 3 :
                r->size <= 1048576 ? 4 : r->size <= 4194304 ? 5 : r->size <= 16777216 ? 6 : 7;
   unsigned f = (r->flags & MSM_BO_CACHED_COHERENT) ? 1 : (r->flags & MSM_BO_CACHED) ? 2 : (r->flags & MSM_BO_UNCACHED) ? 3 : 0;
   esc_new_sz[c][f]++;
}

static void
esc_stats(unsigned nr)
{
   if (esc_log < 0) {
      const char *e = getenv("QCDX_ESCLOG");
      esc_log = e && e[0] == '1';
   }
   if (!esc_log)
      return;
   esc_count[nr & 0xff]++;
   ULONGLONG now = GetTickCount64();
   if (!esc_t0)
      esc_t0 = now;
   if (now - esc_t0 >= 1000) {
      char line[512];
      int n = snprintf(line, sizeof(line), "ESC/s:");
      for (unsigned i = 0; i < 256 && n < (int)sizeof(line) - 16; i++)
         if (esc_count[i])
            n += snprintf(line + n, sizeof(line) - n, " %02x=%u", i, esc_count[i]);
      QcdxWddmLogf("%s\n", line);
      n = snprintf(line, sizeof(line), "GEM_NEW/s by size (<=4K,16K,64K,256K,1M,4M,16M,>) x flags(wc,cohe,cached,unc):");
      for (unsigned c = 0; c < 8; c++)
         n += snprintf(line + n, sizeof(line) - n, " [%u %u %u %u]", esc_new_sz[c][0], esc_new_sz[c][1], esc_new_sz[c][2], esc_new_sz[c][3]);
      QcdxWddmLogf("%s\n", line);
      memset(esc_new_sz, 0, sizeof(esc_new_sz));
      memset(esc_count, 0, sizeof(esc_count));
      esc_t0 = now;
   }
}

static int
esc(int fd, unsigned nr, void *data, unsigned size)
{
   struct qcdx_escape *e;
   fd_wddm_escape_fn callback;
   void *context;
   int index, ret;

   if (size > QCDX_ESC_MAX_DATA || (size != 0 && data == NULL))
      return -EINVAL;
   e = calloc(1, sizeof(*e));
   if (!e)
      return -ENOMEM;
   e->magic = QCDX_ESC_MAGIC;
   e->nr = nr;
   e->size = size;
   e->ret = -ENOSYS;
   if (size)
      memcpy(e->data, data, size);
   AcquireSRWLockExclusive(&fds_lock);
   index = slot(fd);
   if (index < 0 || !fds[index].escape) {
      ReleaseSRWLockExclusive(&fds_lock);
      free(e);
      return -EBADF;
   }
   if (fds[index].active == UINT_MAX) {
      ReleaseSRWLockExclusive(&fds_lock);
      free(e);
      return -EBUSY;
   }
   fds[index].active++;
   callback = fds[index].escape;
   context = fds[index].ctx;
   ReleaseSRWLockExclusive(&fds_lock);
   AcquireSRWLockExclusive(&esc_stats_lock);
   esc_stats(nr);
   if (esc_log > 0 && nr == DRM_COMMAND_BASE + DRM_MSM_GEM_NEW &&
       size >= sizeof(struct drm_msm_gem_new))
      esc_stats_new(data);
   ReleaseSRWLockExclusive(&esc_stats_lock);
   ret = callback(context, e, sizeof(*e));
   AcquireSRWLockExclusive(&fds_lock);
   fds[index].active--;
   if (fds[index].active == 0) {
      if (fds[index].refs == 0 && !fds[index].detaching) {
         fds[index].escape = NULL;
         fds[index].ctx = NULL;
         release_empty_table();
      }
      WakeAllConditionVariable(&fds_drained);
   }
   ReleaseSRWLockExclusive(&fds_lock);
   if (ret == 0) {
      ret = e->magic == QCDX_ESC_MAGIC && e->nr == nr && e->size == size && e->ret <= 0 ? e->ret : -EIO;
      if (ret == 0 && size)
         memcpy(data, e->data, size);
   } else {
      ret = -EIO;
   }
   free(e);
   return ret;
}

/* libdrm convention: 0 or -1 with errno for drmIoctl, -errno for drmCommand* */
int
drmIoctl(int fd, unsigned long request, void *arg)
{
   int ret = esc(fd, request & 0xff, arg, IOCPARM_LEN(request));
   if (ret < 0) {
      errno = -ret;
      return -1;
   }
   return 0;
}

int
drmCommandNone(int fd, unsigned long idx)
{
   return esc(fd, DRM_COMMAND_BASE + idx, NULL, 0);
}

int
drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{
   return esc(fd, DRM_COMMAND_BASE + idx, data, size);
}

int
drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{
   return esc(fd, DRM_COMMAND_BASE + idx, data, size);
}

int
drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{
   return esc(fd, DRM_COMMAND_BASE + idx, data, size);
}

drmVersionPtr
drmGetVersion(int fd)
{
   struct qcdx_version v = {0};
   drmVersionPtr r;

   if (esc(fd, QCDX_NR_VERSION, &v, sizeof(v)) != 0)
      return NULL;
   r = calloc(1, sizeof(*r) + sizeof(v.name));
   if (!r)
      return NULL;
   r->version_major = v.major;
   r->version_minor = v.minor;
   r->version_patchlevel = v.patch;
   r->name = (char *)(r + 1);
   memcpy(r->name, v.name, sizeof(v.name) - 1);
   r->name_len = (int)strlen(r->name);
   r->date = r->desc = r->name + r->name_len;
   return r;
}

void
drmFreeVersion(drmVersionPtr v)
{
   free(v);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   (void)capability;
   *value = 0;
   return -EINVAL;
}

int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -ENOSYS; }
int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -ENOSYS; }
int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENOSYS; }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return -ENOSYS; }
int drmSyncobjSignal(int fd, const uint32_t *h, uint32_t n) { (void)fd; (void)h; (void)n; return -ENOSYS; }
int drmOpenWithType(const char *n, const char *b, int t) { (void)n; (void)b; (void)t; return -1; }

/* mmap(fd, offset) of a BO: offset is the MSM_INFO_GET_OFFSET token from the KMD */
void *
mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
   struct qcdx_mmap m = { (unsigned long long)offset, length, 0 };
   (void)addr;
   (void)prot;
   (void)flags;
   if (esc(fd, QCDX_NR_MMAP, &m, sizeof(m)) != 0 || m.addr == 0)
      return MAP_FAILED;
   return (void *)(uintptr_t)m.addr;
}

int
munmap(void *addr, size_t length)
{
   struct qcdx_mmap m = { 0, length, (unsigned long long)(uintptr_t)addr };
   int local[16];
   int *handles = local;
   unsigned count = 0;

   AcquireSRWLockShared(&fds_lock);
   if (slot_count > 16) {
      handles = malloc((size_t)slot_count * sizeof(*handles));
      if (!handles) {
         ReleaseSRWLockShared(&fds_lock);
         errno = ENOMEM;
         return -1;
      }
   }
   for (int index = 0; index < slot_count; index++) {
      if (fds[index].refs > 0)
         handles[count++] = fds[index].handle;
   }
   ReleaseSRWLockShared(&fds_lock);
   /* the mapping belongs to whichever device made it: try every open one */
   int result = -1;
   for (unsigned index = 0; index < count; index++) {
      if (esc(handles[index], QCDX_NR_MUNMAP, &m, sizeof(m)) == 0) {
         result = 0;
         break;
      }
   }
   if (handles != local)
      free(handles);
   return result;
}

/* ---- POSIX bits freedreno needs on Windows (see include/) ---- */
#include <time.h>
#include "pthread.h"
#include "poll.h"
#include "sys/sysinfo.h"

int fd_wddm_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive((PSRWLOCK)m); return 0; }
int fd_wddm_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); return 0; }
int fd_wddm_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
   return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0) ? 0 : -1;
}
int fd_wddm_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); return 0; }
int fd_wddm_poll(struct pollfd *descriptors, unsigned long nfds, int timeout) { (void)descriptors; (void)nfds; (void)timeout; return -1; }
long fd_wddm_syscall(long n) { (void)n; return (long)GetCurrentThreadId(); }
int fd_wddm_usleep(unsigned usec) { Sleep((usec + 999) / 1000); return 0; }

int fd_wddm_sysinfo(struct sysinfo *si)
{
   MEMORYSTATUSEX ms = { sizeof(ms) };
   GlobalMemoryStatusEx(&ms);
   si->totalram = ms.ullTotalPhys;
   si->freeram = ms.ullAvailPhys;
   si->mem_unit = 1;
   return 0;
}

int fd_wddm_clock_gettime(int clk, struct timespec *ts)
{
   static LARGE_INTEGER freq;
   LARGE_INTEGER now;
   (void)clk;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&now);
   ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
   ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000ll / freq.QuadPart);
   return 0;
}

#include <stdarg.h>
#include <stdio.h>
#include <io.h>

int fd_wddm_ioctl(int fd, unsigned long request, ...) { (void)fd; (void)request; errno = ENOTTY; return -1; }

int fd_wddm_vasprintf(char **out, const char *fmt, va_list ap)
{
   va_list ap2;
   int n;
   va_copy(ap2, ap);
   n = _vscprintf(fmt, ap2);
   va_end(ap2);
   if (n < 0 || !(*out = malloc((size_t)n + 1)))
      return -1;
   return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}

int fd_wddm_asprintf(char **out, const char *fmt, ...)
{
   va_list ap;
   int n;
   va_start(ap, fmt);
   n = fd_wddm_vasprintf(out, fmt, ap);
   va_end(ap);
   return n;
}

char *fd_wddm_strndup(const char *s, size_t n)
{
   size_t l = strnlen(s, n);
   char *r = malloc(l + 1);
   if (r) {
      memcpy(r, s, l);
      r[l] = 0;
   }
   return r;
}

int fd_wddm_ftruncate(int fd, long long len) { return _chsize_s(fd, len) == 0 ? 0 : -1; }

/* only used for shader disassembly dumps (debug options): not supported */
FILE *fd_wddm_open_memstream(char **ptr, size_t *size) { *ptr = NULL; *size = 0; return NULL; }

long fd_wddm_sysconf(int name)
{
   SYSTEM_INFO si;
   (void)name;
   GetSystemInfo(&si);
   return (long)si.dwNumberOfProcessors;
}
