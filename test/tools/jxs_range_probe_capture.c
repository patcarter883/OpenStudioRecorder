/*
 * jxs_range_probe_capture.c - print what a recorded transport stream DECLARES about its range.
 *
 * Reads a captured .ts (the camera's own bytes, as saved by the recorder's passthrough mode or by
 * any capture) and feeds it through the recorder's probe glue and the VENDORED parser, so the
 * declaration in a file on disk can be checked without OBS, without GStreamer and without a camera.
 *
 *   jxs_range_probe_capture full.ts
 *
 * This is the field check for "did the node really sign what we think it signed" - it answers from
 * the two carriage points in the stream, not from a label someone typed.
 */
#include "jxs_range_probe.h"

#include <stdio.h>
#include <stdlib.h>

#define CHUNK 1316 /* one datagram: 7 x 188-byte packets */

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <capture.ts>\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}

	jxs_range_probe_t *p = jxs_range_probe_new();
	if (!p) {
		fprintf(stderr, "probe allocation failed\n");
		fclose(f);
		return 2;
	}

	uint8_t buf[CHUNK];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		jxs_range_probe_feed(p, buf, n);
	fclose(f);

	uint8_t prim = 0, trans = 0, mat = 0;
	jxs_range_probe_colour(p, &prim, &trans, &mat);
	uint64_t datagrams = 0, frames = 0, malformed = 0;
	jxs_range_probe_stats(p, &datagrams, &frames, &malformed);

	const int present = jxs_range_probe_present(p);
	const int full = jxs_range_probe_full_range(p);
	const int pmt_full = jxs_range_probe_pmt_full_range(p);
	const char *name = full == JXS_RANGE_FULL ? "FULL"
			 : full == JXS_RANGE_LIMITED ? "limited"
						     : "unknown";

	printf("%s\n", argv[1]);
	printf("  declaration present : %s\n", present ? "yes" : "NO");
	printf("  range (merged)      : %s", name);
	if (present)
		printf("  (video_full_range=%d)", full == JXS_RANGE_FULL ? 1 : 0);
	printf("\n");
	printf("  range (PMT point)   : %s\n",
	       pmt_full == JXS_RANGE_FULL ? "FULL"
	       : pmt_full == JXS_RANGE_LIMITED ? "limited"
					       : "unknown");
	printf("  source              : %s\n", jxs_range_probe_source(p));
	printf("  H.273 prim/trans/mat: %u/%u/%u\n", prim, trans, mat);
	printf("  datagrams/frames/bad: %llu/%llu/%llu\n",
	       (unsigned long long)datagrams, (unsigned long long)frames,
	       (unsigned long long)malformed);
	printf("  -> a recording of this stream must be tagged %s\n",
	       full == JXS_RANGE_FULL ? "1:3:5:1 (full)"
	       : full == JXS_RANGE_LIMITED ? "bt709 (limited)"
					   : "0:3:5:1 (unknown)");

	jxs_range_probe_free(p);
	return present ? 0 : 1;
}
