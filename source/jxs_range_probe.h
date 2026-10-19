/*
 * jxs_range_probe.h - the recorder's glue onto the VENDORED JPEG XS demux.
 *
 * The stream's colour/range declaration is parsed by vendor/jxs/jxs_ts.c, the SAME translation unit
 * the OBS plugin ships (obs-jxs-camera, branch fix/colour-range-truth) - this rig's rule is one
 * parser, never a second. This header is not a parser and holds no constants: it is only the thin
 * C-callable face of that demux, so a C++ application can drive it without seeing the demux's
 * `_Atomic` counters (which are not valid C++).
 *
 * Feed it the raw transport-stream bytes and ask it what the stream declared. The range it reports
 * follows the signed value in BOTH carriage points - the PMT's JXS_video_descriptor and the
 * per-frame 'jxes' box - with the box winning per frame and the PMT as fallback, exactly as the
 * plugin does (that ordering lives in jxs_ts.c; nothing here duplicates it).
 */
#ifndef OSR_JXS_RANGE_PROBE_H
#define OSR_JXS_RANGE_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jxs_range_probe jxs_range_probe_t;

/* The measured quantisation range. Deliberately the same three outcomes the recorder's model
 * already has, so "not measured" cannot be confused with "measured limited". */
enum
{
  JXS_RANGE_UNKNOWN = -1, /* no declaration seen in either carriage point */
  JXS_RANGE_LIMITED = 0,  /* video_full_range = 0 */
  JXS_RANGE_FULL = 1      /* video_full_range = 1 */
};

jxs_range_probe_t *jxs_range_probe_new(void);
void jxs_range_probe_free(jxs_range_probe_t *p);

/* One chunk of transport stream (any whole number of 188-byte packets). */
void jxs_range_probe_feed(jxs_range_probe_t *p, const uint8_t *data, size_t len);

/* Was a declaration seen at all? */
int jxs_range_probe_present(const jxs_range_probe_t *p);

/* JXS_RANGE_FULL / JXS_RANGE_LIMITED / JXS_RANGE_UNKNOWN. */
int jxs_range_probe_full_range(const jxs_range_probe_t *p);

/* The PMT carriage point ALONE: the value signed in the PMT's JXS_video_descriptor, or
 * JXS_RANGE_UNKNOWN when that descriptor was not seen. Kept separate from the merged answer above so
 * a caller (and the test) can tell which of the two carriage points actually said what. */
int jxs_range_probe_pmt_full_range(const jxs_range_probe_t *p);

/* Where the reported declaration came from: "jxes box", "PMT JXS_video_descriptor" or "none".
 * Static string; for the log line, so a reader can tell the two carriage points apart. */
const char *jxs_range_probe_source(const jxs_range_probe_t *p);

/* The signed H.273 codes (primaries/transfer/matrix) of the reported declaration. */
void jxs_range_probe_colour(const jxs_range_probe_t *p, uint8_t *primaries,
                            uint8_t *transfer, uint8_t *matrix);

/* Demux counters, for the log: datagrams fed, complete codestreams, malformed datagrams. */
void jxs_range_probe_stats(const jxs_range_probe_t *p, uint64_t *datagrams,
                           uint64_t *frames, uint64_t *malformed);

#ifdef __cplusplus
}
#endif

#endif /* OSR_JXS_RANGE_PROBE_H */
