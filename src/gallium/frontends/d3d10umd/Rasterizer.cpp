/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Rasterizer.cpp --
 *    Functions that manipulate rasterizer state.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "pipe/p_screen.h"
#include "util/u_upload_mgr.h"
#include "Rasterizer.h"
#include "State.h"

#include "Debug.h"


/* qcdxkm: QCDX_DRAWLOG=1 logs every draw with render target, viewport, scissor (umd.log) */
static float g_topazVp[4];
static int g_topazSc[4];
static bool g_topazRsScissor;
static bool g_topazDrawLog;
static bool g_topazDrawLogInit;
static float g_topazFsSc[4];

static bool
topaz_fs_scissor_on(void)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("QCDX_FSSCISSOR");
      on = e && e[0] == '1';
   }
   return on;
}


static inline void
topaz_box(struct pipe_box *b, int x, int y, int w, int h)
{
   memset(b, 0, sizeof(*b));
   b->x = x; b->y = y; b->width = w; b->height = h; b->depth = 1;
}
struct TopazScFix {
   struct pipe_scissor_state trueSc;
   bool trueScValid;
   struct pipe_resource *stripTex;
   struct pipe_resource *stripRt;   /* RT the pending strips belong to (referenced) */
   struct pipe_box strips[4];
   unsigned numStrips;
};

/* per device: explorer and other apps have several D3D devices in one process; a process-global scratch
 * texture created on one screen and used on another crashed in fd_resource_copy_region */
static struct TopazScFix *
topaz_scfix(Device *d)
{
   if (!d->topazScFix)
      d->topazScFix = (struct TopazScFix *)calloc(1, sizeof(struct TopazScFix));
   return d->topazScFix;
}

extern "C" void
TopazScFixDestroy(void *device)
{
   Device *d = (Device *)device;
   if (!d || !d->topazScFix)
      return;
   pipe_resource_reference(&d->topazScFix->stripTex, NULL);
   pipe_resource_reference(&d->topazScFix->stripRt, NULL);
   free(d->topazScFix);
   d->topazScFix = NULL;
}

static bool
topaz_scfix_on(void)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("QCDX_SCFIX");
      on = !(e && e[0] == '0');
   }
   return on;
}

extern "C" void
TopazStripsRestore(void *device)
{
   if (!device)
      return;
   Device *d = (Device *)device;
   struct TopazScFix *S = d->topazScFix;
   if (!S || !S->numStrips)
      return;
   for (unsigned i = 0; i < S->numStrips; i++)
      d->pipe->resource_copy_region(d->pipe, S->stripRt, 0, S->strips[i].x, S->strips[i].y, 0,
                                    S->stripTex, 0, &S->strips[i]);
   S->numStrips = 0;
   pipe_resource_reference(&S->stripRt, NULL);
}

static void
topaz_strips_save(Device *d, const struct pipe_scissor_state *sc)
{
   TopazStripsRestore(d);
   struct TopazScFix *S = topaz_scfix(d);
   if (!S)
      return;
   S->trueSc = *sc;
   S->trueScValid = true;
   if (!d->fb.cbufs[0])
      return;
   struct pipe_resource *rt = d->fb.cbufs[0]->texture;
   if (!rt || rt->target != PIPE_TEXTURE_2D || rt->nr_samples > 1)
      return;
   int w = rt->width0, h = rt->height0;
   int x0 = sc->minx, y0 = sc->miny, x1 = sc->maxx, y1 = sc->maxy;   /* exclusive max, unaligned */
   int ax0 = x0 & ~1, ay0 = y0 & ~1, ax1 = MIN2((x1 + 1) & ~1, w), ay1 = MIN2((y1 + 1) & ~1, h);
   if (x1 <= x0 || y1 <= y0 || x0 >= w || y0 >= h)
      return;
   if (!S->stripTex || S->stripTex->width0 != rt->width0 || S->stripTex->height0 != rt->height0 ||
       S->stripTex->format != rt->format) {
      pipe_resource_reference(&S->stripTex, NULL);
      struct pipe_resource tmpl;
      memset(&tmpl, 0, sizeof(tmpl));
      tmpl.target = PIPE_TEXTURE_2D;
      tmpl.format = rt->format;
      tmpl.width0 = rt->width0;
      tmpl.height0 = rt->height0;
      tmpl.depth0 = 1;
      tmpl.array_size = 1;
      tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
      tmpl.usage = PIPE_USAGE_DEFAULT;
      S->stripTex = d->screen->resource_create(d->screen, &tmpl);
      if (!S->stripTex)
         return;
   }
   unsigned n = 0;
   struct pipe_box *b = S->strips;
   if (ax0 < x0) { topaz_box(&b[n++], ax0, ay0, x0 - ax0, ay1 - ay0); }
   if (ax1 > x1) { topaz_box(&b[n++], x1, ay0, ax1 - x1, ay1 - ay0); }
   if (ay0 < y0) { topaz_box(&b[n++], x0, ay0, MIN2(x1, w) - x0, y0 - ay0); }
   if (ay1 > y1) { topaz_box(&b[n++], x0, y1, MIN2(x1, w) - x0, ay1 - y1); }
   for (unsigned i = 0; i < n; i++)
      d->pipe->resource_copy_region(d->pipe, S->stripTex, 0, b[i].x, b[i].y, 0, rt, 0, &b[i]);
   S->numStrips = n;
   if (n)
      pipe_resource_reference(&S->stripRt, rt);
}

extern "C" void
TopazStripsResave(void *device)
{
   if (!device || !topaz_scfix_on() || topaz_fs_scissor_on())
      return;
   struct TopazScFix *S = ((Device *)device)->topazScFix;
   if (!S || !S->trueScValid)
      return;
   struct pipe_scissor_state sc = S->trueSc;
   topaz_strips_save((Device *)device, &sc);
}

/* CONST[15][0] of every PS = D3D scissor (see ShaderTGSI.c); zeros when the RS scissor is off */
static void
topaz_upload_fs_scissor(struct pipe_context *pipe)
{
   if (!topaz_fs_scissor_on())
      return;
   float data[4] = { 0, 0, 0, 0 };
   if (g_topazRsScissor)
      memcpy(data, g_topazFsSc, sizeof(data));
   struct pipe_constant_buffer cb;
   memset(&cb, 0, sizeof(cb));
   cb.buffer_size = sizeof(data);
   u_upload_data(pipe->stream_uploader, 0, sizeof(data), 16, data, &cb.buffer_offset, &cb.buffer);
   u_upload_unmap(pipe->stream_uploader);
   pipe->set_constant_buffer(pipe, PIPE_SHADER_FRAGMENT, 15, true, &cb);
}

/* qcdxkm: QCDX_DRAWLOG=1 + file C:\ProgramData\qcdx\umd.dump -> at the next Present dump the presented
 * frame and the large textures sampled recently as 32-bit BMPs (umd-dump-<pid>-<n>-<tag>.bmp) */
static struct pipe_resource *g_topazBig[12];
static bool g_topazDumpAlpha;
static unsigned g_topazBigNext;

static void
topaz_remember_big(struct pipe_resource *t)
{
   if (!t || t->target != PIPE_TEXTURE_2D || t->width0 < 400 || t->height0 < 400)
      return;
   for (unsigned i = 0; i < 12; i++)
      if (g_topazBig[i] == t)
         return;
   struct pipe_resource **slot = &g_topazBig[g_topazBigNext++ % 12];
   pipe_resource_reference(slot, NULL);
   pipe_resource_reference(slot, t);
}

static void
topaz_dump_one(struct pipe_context *pipe, struct pipe_resource *res, unsigned seq, const char *tag, unsigned idx)
{
   if (!res || res->target != PIPE_TEXTURE_2D)
      return;
   enum pipe_format f = res->format;
   bool swap;
   if (f == PIPE_FORMAT_B8G8R8A8_UNORM || f == PIPE_FORMAT_B8G8R8X8_UNORM || f == PIPE_FORMAT_B8G8R8A8_SRGB)
      swap = false;
   else if (f == PIPE_FORMAT_R8G8B8A8_UNORM || f == PIPE_FORMAT_R8G8B8X8_UNORM || f == PIPE_FORMAT_R8G8B8A8_SRGB)
      swap = true;
   else {
      QcdxWddmLogf("DUMP %s%u %p %ux%u fmt %d skipped\n", tag, idx, (void *)res, res->width0, res->height0, (int)f);
      return;
   }
   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.width = res->width0;
   box.height = res->height0;
   box.depth = 1;
   struct pipe_transfer *xfer = NULL;
   uint8_t *map = (uint8_t *)pipe->texture_map(pipe, res, 0, PIPE_MAP_READ, &box, &xfer);
   if (!map) {
      QcdxWddmLogf("DUMP %s%u map failed\n", tag, idx);
      return;
   }
   char path[160];
   snprintf(path, sizeof(path), "C:\\ProgramData\\qcdx\\umd-dump-%lu-%u-%s%u.bmp", (unsigned long)GetCurrentProcessId(), seq, tag, idx);
   FILE *fp = fopen(path, "wb");
   if (fp) {
      uint32_t w = res->width0, h = res->height0, sz = w * h * 4;
      uint8_t hdr[54] = { 'B', 'M' };
      uint32_t off = 54, tot = 54 + sz, dib = 40, planes_bpp = (32u << 16) | 1u, comp = 0;
      int32_t nh = -(int32_t)h;
      memcpy(hdr + 2, &tot, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
      memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &nh, 4); memcpy(hdr + 26, &planes_bpp, 4);
      memcpy(hdr + 30, &comp, 4); memcpy(hdr + 34, &sz, 4);
      fwrite(hdr, 1, 54, fp);
      uint8_t *row = (uint8_t *)malloc(w * 4);
      for (uint32_t y = 0; y < h; y++) {
         const uint8_t *s = map + (size_t)y * xfer->stride;
         for (uint32_t x = 0; x < w; x++) {
            row[x * 4 + 0] = swap ? s[x * 4 + 2] : s[x * 4 + 0];
            row[x * 4 + 1] = s[x * 4 + 1];
            row[x * 4 + 2] = swap ? s[x * 4 + 0] : s[x * 4 + 2];
            row[x * 4 + 3] = 255;
            if (g_topazDumpAlpha)   /* QCDX_DUMPALPHA=1: alpha channel as grey */
               row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = s[x * 4 + 3];
         }
         fwrite(row, 1, w * 4, fp);
      }
      free(row);
      fclose(fp);
   }
   pipe->texture_unmap(pipe, xfer);
   QcdxWddmLogf("DUMP %s%u %p %ux%u fmt %d -> %s\n", tag, idx, (void *)res, res->width0, res->height0, (int)f, path);
}

extern "C" void
TopazMaybeDump(void *pipe_, void *presented)
{
   if (!g_topazDrawLog)
      return;
   if (GetFileAttributesA("C:\\ProgramData\\qcdx\\umd.dump") == INVALID_FILE_ATTRIBUTES)
      return;
   DeleteFileA("C:\\ProgramData\\qcdx\\umd.dump");
   g_topazDumpAlpha = getenv("QCDX_DUMPALPHA") && getenv("QCDX_DUMPALPHA")[0] == '1';
   static unsigned seq;
   struct pipe_context *pipe = (struct pipe_context *)pipe_;
   seq++;
   pipe->flush(pipe, NULL, 0);
   topaz_dump_one(pipe, (struct pipe_resource *)presented, seq, "frame", 0);
   for (unsigned i = 0; i < 12; i++)
      topaz_dump_one(pipe, g_topazBig[i], seq, "tex", i);
}

/* qcdxkm: probe. File C:\ProgramData\qcdx\umd.probe containing a row number Y arms it at the next
 * Present; during the following frame every draw into a 1080-wide RT is followed by a readback of row Y,
 * logged as runs of the green channel ("PROBE k: x0-x1=v ..."). Disarmed (file deleted) at the next Present. */
extern "C" { const void *g_topazPsTokens; }
static int g_topazProbeY = -1;
static int g_topazProbeFrames;
static unsigned g_topazProbeDraw;

extern "C" void
TopazProbePresent(void)
{
   if (g_topazProbeY >= 0) {
      if (--g_topazProbeFrames > 0) {
         QcdxWddmLogf("PROBE frame\n");
         return;
      }
      g_topazProbeY = -1;
      DeleteFileA("C:\\ProgramData\\qcdx\\umd.probe");
      QcdxWddmLogf("PROBE end\n");
      return;
   }
   FILE *f = fopen("C:\\ProgramData\\qcdx\\umd.probe", "r");
   if (f) {
      int y = -1, frames = 1;
      if (fscanf(f, "%d %d", &y, &frames) >= 1 && y >= 0) {
         g_topazProbeY = y;
         g_topazProbeFrames = frames > 0 ? frames : 1;
         g_topazProbeDraw = 0;
         QcdxWddmLogf("PROBE start row %d frames %d\n", y, g_topazProbeFrames);
      }
      fclose(f);
   }
}

static void
topaz_probe_after_draw(Device *d, const char *kind)
{
   if (g_topazProbeY < 0 || !d->fb.cbufs[0])
      return;
   struct pipe_resource *rt = d->fb.cbufs[0]->texture;
   if (!rt || rt->width0 != 1080 || (int)rt->height0 <= g_topazProbeY)
      return;
   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.y = g_topazProbeY;
   box.width = rt->width0;
   box.height = 1;
   box.depth = 1;
   struct pipe_transfer *xfer = NULL;
   d->pipe->flush(d->pipe, NULL, 0);
   uint8_t *map = (uint8_t *)d->pipe->texture_map(d->pipe, rt, 0, PIPE_MAP_READ, &box, &xfer);
   if (!map)
      return;
   char line[1400];
   int n = snprintf(line, sizeof(line), "PROBE %u %s ps=%p sc=%d,%d,%d,%d:", g_topazProbeDraw++, kind,
                    g_topazPsTokens, g_topazSc[0], g_topazSc[1], g_topazSc[2], g_topazSc[3]);
   unsigned x0 = 0;
   for (unsigned x = 1; x <= rt->width0 && n < (int)sizeof(line) - 24; x++) {
      if (x == rt->width0 || (map[x * 4 + 1] >> 3) != (map[x0 * 4 + 1] >> 3)) {
         n += snprintf(line + n, sizeof(line) - n, " %u-%u=%u", x0, x - 1, map[x0 * 4 + 1]);
         x0 = x;
      }
   }
   d->pipe->texture_unmap(d->pipe, xfer);
   QcdxWddmLogf("%s\n", line);
}

extern "C" void
TopazProbeAfterCopy(void *device)
{
   if (device)
      topaz_probe_after_draw((Device *)device, "copy");
}

/* qcdxkm experiment QCDX_FLUSHDRAW=1: flush + wait for the GPU after every draw (fully serialized) */
extern "C" void
TopazAfterDraw(void *device)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("QCDX_FLUSHDRAW");
      on = e && e[0] == '1';
   }
   if (device)
      topaz_probe_after_draw((Device *)device, "draw");
   if (!on || !device)
      return;
   Device *d = (Device *)device;
   struct pipe_fence_handle *fence = NULL;
   d->pipe->flush(d->pipe, &fence, 0);
   if (fence) {
      d->screen->fence_finish(d->screen, NULL, fence, ~0ull);
      d->screen->fence_reference(d->screen, &fence, NULL);
   }
}

extern "C" void
TopazLogDraw(const char *kind, unsigned count, void *device, void *rt, unsigned rtw, unsigned rth)
{
   if (device && !((Device *)device)->topazRsScissor)
      TopazStripsRestore(device);
   if (!g_topazDrawLogInit) {
      const char *e = getenv("QCDX_DRAWLOG");
      g_topazDrawLog = e && e[0] == '1';
      g_topazDrawLogInit = true;
   }
   if (!g_topazDrawLog)
      return;
   char srv[160];
   int n = 0;
   srv[0] = 0;
   if (device) {
      Device *d = (Device *)device;
      for (int i = 0; i < 3; i++) {
         struct pipe_sampler_view *sv = d->sampler_views[PIPE_SHADER_FRAGMENT][i];
         if (sv && sv->texture)
            topaz_remember_big(sv->texture);
         if (sv && sv->texture && n < 120)
            n += snprintf(srv + n, sizeof(srv) - n, " t%d=%p/%ux%u/f%d", i, (void *)sv->texture,
                          sv->texture->width0, sv->texture->height0, (int)sv->format);
      }
   }
   QcdxWddmLogf("DRAW %s n=%u rt=%p %ux%u vp=%.0f,%.0f %.0fx%.0f rs_scissor=%d sc=%d,%d,%d,%d%s\n", kind, count, rt, rtw, rth,
                 g_topazVp[0], g_topazVp[1], g_topazVp[2], g_topazVp[3], g_topazRsScissor ? 1 : 0,
                 g_topazSc[0], g_topazSc[1], g_topazSc[2], g_topazSc[3], srv);
}

/*
 * ----------------------------------------------------------------------
 *
 * SetViewports --
 *
 *    The SetViewports function sets viewports.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetViewports(D3D10DDI_HDEVICE hDevice,                                        // IN
             UINT NumViewports,                                               // IN
             UINT ClearViewports,                                             // IN
             __in_ecount (NumViewports) const D3D10_DDI_VIEWPORT *pViewports) // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_viewport_state states[PIPE_MAX_VIEWPORTS];

   ASSERT(NumViewports + ClearViewports <=
          D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);

   for (UINT i = 0; i < NumViewports; ++i) {
      const D3D10_DDI_VIEWPORT *pViewport = &pViewports[i];
      float width = pViewport->Width;
      float height = pViewport->Height;
      float x = pViewport->TopLeftX;
      float y = pViewport->TopLeftY;
      float z = pViewport->MinDepth;
      float half_width = width / 2.0f;
      float half_height = height / 2.0f;
      float depth = pViewport->MaxDepth - z;

      states[i].scale[0] = half_width;
      states[i].scale[1] = -half_height;
      states[i].scale[2] = depth;

      states[i].translate[0] = half_width + x;
      states[i].translate[1] = half_height + y;
      states[i].translate[2] = z;
   }
   if (NumViewports) {
      g_topazVp[0] = pViewports[0].TopLeftX; g_topazVp[1] = pViewports[0].TopLeftY;
      g_topazVp[2] = pViewports[0].Width;    g_topazVp[3] = pViewports[0].Height;
   }
   if (ClearViewports) {
      memset(states + NumViewports, 0,
             sizeof(struct pipe_viewport_state) * ClearViewports);
   }
   pipe->set_viewport_states(pipe, 0, NumViewports + ClearViewports,
                             states);
}


/*
 * ----------------------------------------------------------------------
 *
 * SetScissorRects --
 *
 *    The SetScissorRects function marks portions of render targets
 *    that rendering is confined to.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetScissorRects(D3D10DDI_HDEVICE hDevice,                            // IN
                UINT NumScissorRects,                                // IN
                UINT ClearScissorRects,                              // IN
                __in_ecount (NumRects) const D3D10_DDI_RECT *pRects) // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_scissor_state states[PIPE_MAX_VIEWPORTS];

   ASSERT(NumScissorRects + ClearScissorRects <=
          D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);

   for (UINT i = 0; i < NumScissorRects; ++i) {
      const D3D10_DDI_RECT *pRect = &pRects[i];
      /* gallium scissor values are unsigned so lets make
      * sure that we don't overflow */
      states[i].minx = pRect->left   < 0 ? 0 : pRect->left;
      states[i].miny = pRect->top    < 0 ? 0 : pRect->top;
      states[i].maxx = pRect->right  < 0 ? 0 : pRect->right;
      states[i].maxy = pRect->bottom < 0 ? 0 : pRect->bottom;
      /* qcdxkm experiment QCDX_SCPAD=1: right/bottom +1 px, =2: every edge 1 px wider (dashed strokes
       * along damage edges = edge pixels DWM thinks it painted?) */
      if (i == 0) {
         g_topazFsSc[0] = (float)states[0].minx;
         g_topazFsSc[1] = (float)states[0].miny;
         g_topazFsSc[2] = 16384.0f - (float)states[0].maxx;
         g_topazFsSc[3] = 16384.0f - (float)states[0].maxy;
      }
      /* qcdxkm: hardware scissor widened to whole 2x2 quads; the exact D3D scissor is applied in the PS
       * (ShaderTGSI.c, demote) so the edge pixels' quad partners stay helper lanes. */
      if (i == 0 && topaz_scfix_on() && !topaz_fs_scissor_on())
         topaz_strips_save(CastDevice(hDevice), &states[0]);
      if (topaz_fs_scissor_on() || topaz_scfix_on()) {
         states[i].minx &= ~1u;
         states[i].miny &= ~1u;
         states[i].maxx = (states[i].maxx + 1) & ~1u;
         states[i].maxy = (states[i].maxy + 1) & ~1u;
      }
      const char *pad = getenv("QCDX_SCPAD");
      if (pad && (pad[0] == '1' || pad[0] == '2')) {
         states[i].maxx += 1;
         states[i].maxy += 1;
         if (pad[0] == '2') {
            states[i].minx = states[i].minx ? states[i].minx - 1 : 0;
            states[i].miny = states[i].miny ? states[i].miny - 1 : 0;
         }
      }
   }
   if (NumScissorRects) {
      g_topazSc[0] = pRects[0].left; g_topazSc[1] = pRects[0].top;
      g_topazSc[2] = pRects[0].right; g_topazSc[3] = pRects[0].bottom;
   }
   if (ClearScissorRects) {
      memset(states + NumScissorRects, 0,
             sizeof(struct pipe_scissor_state) * ClearScissorRects);
   }
   pipe->set_scissor_states(pipe, 0, NumScissorRects + ClearScissorRects,
                            states);
   topaz_upload_fs_scissor(pipe);
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateRasterizerStateSize --
 *
 *    The CalcPrivateRasterizerStateSize function determines the size
 *    of the user-mode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the size
 *    of the resource video memory) for a rasterizer state.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateRasterizerStateSize(
   D3D10DDI_HDEVICE hDevice,                                // IN
   __in const D3D10_DDI_RASTERIZER_DESC *pRasterizerDesc)   // IN
{
   return sizeof(RasterizerState);
}


static uint
translate_cull_mode(D3D10_DDI_CULL_MODE CullMode)
{
   switch (CullMode) {
   case D3D10_DDI_CULL_NONE:
      return PIPE_FACE_NONE;
   case D3D10_DDI_CULL_FRONT:
      return PIPE_FACE_FRONT;
   case D3D10_DDI_CULL_BACK:
      return PIPE_FACE_BACK;
   default:
      assert(0);
      return PIPE_FACE_NONE;
   }
}

static uint
translate_fill_mode(D3D10_DDI_FILL_MODE FillMode)
{
   switch (FillMode) {
   case D3D10_DDI_FILL_WIREFRAME:
      return PIPE_POLYGON_MODE_LINE;
   case D3D10_DDI_FILL_SOLID:
      return PIPE_POLYGON_MODE_FILL;
   default:
      assert(0);
      return PIPE_POLYGON_MODE_FILL;
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateRasterizerState --
 *
 *    The CreateRasterizerState function creates a rasterizer state.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateRasterizerState(
    D3D10DDI_HDEVICE hDevice,                               // IN
    __in const D3D10_DDI_RASTERIZER_DESC *pRasterizerDesc,  // IN
    D3D10DDI_HRASTERIZERSTATE hRasterizerState,             // IN
    D3D10DDI_HRTRASTERIZERSTATE hRTRasterizerState)         // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   RasterizerState *pRasterizerState = CastRasterizerState(hRasterizerState);

   struct pipe_rasterizer_state state;
   memset(&state, 0, sizeof state);

   state.flatshade_first = 1;
   state.front_ccw = (pRasterizerDesc->FrontCounterClockwise ? 1 : 0);
   state.cull_face = translate_cull_mode(pRasterizerDesc->CullMode);
   state.fill_front = translate_fill_mode(pRasterizerDesc->FillMode);
   state.fill_back = state.fill_front;
   state.scissor = (pRasterizerDesc->ScissorEnable ? 1 : 0);
   state.line_smooth = (pRasterizerDesc->AntialiasedLineEnable ? 1 : 0);
   state.offset_units = (float)pRasterizerDesc->DepthBias;
   state.offset_scale = pRasterizerDesc->SlopeScaledDepthBias;
   state.offset_clamp = pRasterizerDesc->DepthBiasClamp;
   state.multisample = /* pRasterizerDesc->MultisampleEnable */ 0;
   state.half_pixel_center = 1;
   state.bottom_edge_rule = 0;
   state.clip_halfz = 1;
   state.depth_clip_near = pRasterizerDesc->DepthClipEnable ? 1 : 0;
   state.depth_clip_far = pRasterizerDesc->DepthClipEnable ? 1 : 0;
   state.depth_clamp = 1;

   state.point_quad_rasterization = 1;
   state.point_size = 1.0f;
   state.point_line_tri_clip = 1;

   state.line_width = 1.0f;
   state.line_rectangular = 0;

   pRasterizerState->handle = pipe->create_rasterizer_state(pipe, &state);
   pRasterizerState->scissor = state.scissor != 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyRasterizerState --
 *
 *    The DestroyRasterizerState function destroys the specified
 *    rasterizer state object. The rasterizer state object can be
 *    destoyed only if it is not currently bound to a display device.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyRasterizerState(D3D10DDI_HDEVICE hDevice,                     // IN
                       D3D10DDI_HRASTERIZERSTATE hRasterizerState)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   RasterizerState *pRasterizerState = CastRasterizerState(hRasterizerState);

   pipe->delete_rasterizer_state(pipe, pRasterizerState->handle);
}


/*
 * ----------------------------------------------------------------------
 *
 * SetRasterizerState --
 *
 *    The SetRasterizerState function sets the rasterizer state.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetRasterizerState(D3D10DDI_HDEVICE hDevice,                   // IN
                   D3D10DDI_HRASTERIZERSTATE hRasterizerState) // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   void *state = CastPipeRasterizerState(hRasterizerState);

   Device *topazDev = CastDevice(hDevice);
   bool topazOld = topazDev->topazRsScissor;
   topazDev->topazRsScissor = CastRasterizerState(hRasterizerState) ? CastRasterizerState(hRasterizerState)->scissor : false;
   g_topazRsScissor = topazDev->topazRsScissor;   /* logging only */
   if (topazOld != topazDev->topazRsScissor) {
      if (topazDev->topazRsScissor)
         TopazStripsResave(CastDevice(hDevice));
      else
         TopazStripsRestore(CastDevice(hDevice));
   }
   pipe->bind_rasterizer_state(pipe, state);
   topaz_upload_fs_scissor(pipe);
}
