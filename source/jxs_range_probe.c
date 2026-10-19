/*
 * jxs_range_probe.c - drives the vendored JPEG XS demux (vendor/jxs/jxs_ts.c) and reports the
 * colour/range declaration it read. See jxs_range_probe.h for why this is glue and not a parser.
 */
#include "jxs_range_probe.h"

#include "jxs_ts.h" /* the VENDORED parser - the one and only copy of these constants */

#include <stdlib.h>
#include <string.h>

/* Same reassembly budget the plugin uses for one codestream (2 MiB); its absence only makes the
 * demux drop frames it cannot hold, which does not affect the declaration we are after. */
#define OSR_PROBE_ASM_CAP (2u * 1024u * 1024u)

/* The plugin's own defaults (src/jxs-camera.c): measured PIDs for the nodes on this rig. The demux
 * also learns the PMT PID from the PAT, so a node that renumbers still resolves. */
#define OSR_PROBE_PMT_PID 1000
#define OSR_PROBE_VIDEO_PID 65

struct jxs_range_probe
{
  jxs_ts_demux_t demux;
  uint8_t *asm_buf;

  /* The last declaration seen in a per-frame jxes box. It wins over the PMT (jxs_ts.c already
   * hands the frame callback the box value, or the PMT fallback when the box is absent), so this
   * only has to record what it was handed. */
  int have_frame_colour;
  uint8_t frame_colour[4]; /* primaries, transfer, matrix, full_range */
};

static void on_frame(void *user, const uint8_t *cs, size_t len, uint64_t pts,
                     const jxs_ts_colour_t *colour)
{
  (void)cs;
  (void)len;
  (void)pts;
  struct jxs_range_probe *p = (struct jxs_range_probe *)user;
  if (colour == NULL || !colour->present)
    return;
  p->have_frame_colour = 1;
  p->frame_colour[0] = colour->primaries;
  p->frame_colour[1] = colour->transfer;
  p->frame_colour[2] = colour->matrix;
  p->frame_colour[3] = colour->full_range;
}

jxs_range_probe_t *jxs_range_probe_new(void)
{
  struct jxs_range_probe *p = (struct jxs_range_probe *)calloc(1, sizeof(*p));
  if (p == NULL)
    return NULL;
  p->asm_buf = (uint8_t *)malloc(OSR_PROBE_ASM_CAP);
  if (p->asm_buf == NULL)
  {
    free(p);
    return NULL;
  }
  /* No audio: this probe wants the video ES's colour declaration and nothing else. */
  jxs_ts_demux_init(&p->demux, p->asm_buf, OSR_PROBE_ASM_CAP, OSR_PROBE_PMT_PID,
                    OSR_PROBE_VIDEO_PID, on_frame, p);
  return p;
}

void jxs_range_probe_free(jxs_range_probe_t *p)
{
  if (p == NULL)
    return;
  free(p->asm_buf);
  free(p);
}

void jxs_range_probe_feed(jxs_range_probe_t *p, const uint8_t *data, size_t len)
{
  if (p == NULL || data == NULL || len == 0)
    return;
  jxs_ts_demux_feed(&p->demux, data, len);
}

int jxs_range_probe_present(const jxs_range_probe_t *p)
{
  if (p == NULL)
    return 0;
  return p->have_frame_colour || p->demux.pmt_colour.present;
}

int jxs_range_probe_full_range(const jxs_range_probe_t *p)
{
  if (p == NULL)
    return JXS_RANGE_UNKNOWN;
  if (p->have_frame_colour)
    return p->frame_colour[3] ? JXS_RANGE_FULL : JXS_RANGE_LIMITED;
  if (p->demux.pmt_colour.present)
    return p->demux.pmt_colour.full_range ? JXS_RANGE_FULL : JXS_RANGE_LIMITED;
  return JXS_RANGE_UNKNOWN;
}

int jxs_range_probe_pmt_full_range(const jxs_range_probe_t *p)
{
  if (p == NULL || !p->demux.pmt_colour.present)
    return JXS_RANGE_UNKNOWN;
  return p->demux.pmt_colour.full_range ? JXS_RANGE_FULL : JXS_RANGE_LIMITED;
}

const char *jxs_range_probe_source(const jxs_range_probe_t *p)
{
  if (p == NULL)
    return "none";
  if (p->have_frame_colour)
  {
    /* The frame callback is handed the box value when a box was present, and the PMT value when it
     * was not (that merge is jxs_ts.c's, and is not duplicated here). When the two agree the value
     * is readable from either; when they differ the box is what won. */
    if (p->demux.pmt_colour.present &&
        p->frame_colour[3] == p->demux.pmt_colour.full_range)
      return "PMT JXS_video_descriptor";
    return "jxes box";
  }
  if (p->demux.pmt_colour.present)
    return "PMT JXS_video_descriptor";
  return "none";
}

void jxs_range_probe_colour(const jxs_range_probe_t *p, uint8_t *primaries,
                            uint8_t *transfer, uint8_t *matrix)
{
  uint8_t prim = 0, trans = 0, mat = 0;
  if (p != NULL)
  {
    if (p->have_frame_colour)
    {
      prim = p->frame_colour[0];
      trans = p->frame_colour[1];
      mat = p->frame_colour[2];
    }
    else if (p->demux.pmt_colour.present)
    {
      prim = p->demux.pmt_colour.primaries;
      trans = p->demux.pmt_colour.transfer;
      mat = p->demux.pmt_colour.matrix;
    }
  }
  if (primaries)
    *primaries = prim;
  if (transfer)
    *transfer = trans;
  if (matrix)
    *matrix = mat;
}

void jxs_range_probe_stats(const jxs_range_probe_t *p, uint64_t *datagrams,
                           uint64_t *frames, uint64_t *malformed)
{
  if (datagrams)
    *datagrams = p ? p->demux.n_datagrams : 0;
  if (frames)
    *frames = p ? p->demux.n_frames : 0;
  if (malformed)
    *malformed = p ? p->demux.n_malformed : 0;
}
