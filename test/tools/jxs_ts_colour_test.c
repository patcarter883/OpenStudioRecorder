/*
 * jxs_ts_colour_test.c - regression test for the JPEG XS range declaration, exercised through the
 * RECORDER's own probe glue (source/jxs_range_probe.c) over the VENDORED parser (vendor/jxs/jxs_ts.c).
 *
 * The probe must READ the range/colour the node signs, in BOTH carriage points, and report the range
 * that is actually on the wire. This hand-builds the wire bytes (no muxer, no GStreamer, no device),
 * so it is a check of the declaration read and nothing else:
 *
 *   - the PMT's JXS_video_descriptor (extension descriptor 0x3F 0x1E 0x14), and
 *   - the per-frame 30-byte 'jxes' box on the video PID.
 *
 * NULL HYPOTHESIS: the range must FOLLOW the signed value, not a literal - the same code path has to
 * report FULL for one sign and LIMITED for the other, in both carriage points. A hardcoded answer
 * fails here. Cases (e) and (f) also pin the precedence: the per-frame box overrides the PMT.
 */
#include "jxs_range_probe.h"

#include <stdio.h>
#include <string.h>

#define PMT_PID 0x03E8
#define VIDEO_PID 0x0041

static int fails;

static void check(int cond, const char *what)
{
	printf("  %-64s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond)
		fails++;
}

/* Build a PMT section carrying the JXS_video_descriptor for VIDEO_PID. */
static size_t build_pmt(uint8_t *sec, uint8_t prim, uint8_t trans, uint8_t mat,
			uint8_t full_range)
{
	memset(sec, 0, 64);
	size_t o = 0;
	sec[o++] = 0x02; /* table_id = PMT */
	o += 2;          /* section_length (patched below) */
	sec[o++] = 0x00;
	sec[o++] = 0x01; /* program_number */
	sec[o++] = 0xC1; /* reserved, version 0, current */
	sec[o++] = 0x00; /* section_number */
	sec[o++] = 0x00; /* last_section_number */
	sec[o++] = 0xE0 | ((0x0100 >> 8) & 0x1F); /* PCR_PID */
	sec[o++] = 0x00;
	sec[o++] = 0xF0; /* program_info_length = 0 */
	sec[o++] = 0x00;
	/* ES 1: JPEG XS video */
	sec[o++] = 0x32;
	sec[o++] = 0xE0 | ((VIDEO_PID >> 8) & 0x1F);
	sec[o++] = VIDEO_PID & 0xFF;
	size_t es_len_pos = o;
	o += 2; /* ES_info_length */
	/* JXS_video_descriptor: 0x3F 0x1E 0x14 then 29 payload bytes */
	size_t d = o;
	sec[d + 0] = 0x3F;
	sec[d + 1] = 30;
	sec[d + 2] = 0x14;
	sec[d + 3] = 0x00; /* descriptor_version */
	sec[d + 27] = prim;
	sec[d + 28] = trans;
	sec[d + 29] = mat;
	sec[d + 30] = (uint8_t)(full_range ? 0x80 : 0x00);
	o = d + 32;
	size_t es_len = o - d;
	sec[es_len_pos] = 0xF0 | (uint8_t)((es_len >> 8) & 0x0F);
	sec[es_len_pos + 1] = (uint8_t)(es_len & 0xFF);
	/* CRC-32/MPEG-2 is not verified by the demux; bytes present, not signed */
	o += 4;
	size_t len = o - 3; /* section_length covers bytes 3..end */
	sec[1] = 0xB0 | (uint8_t)((len >> 8) & 0x0F);
	sec[2] = (uint8_t)(len & 0xFF);
	return o;
}

/* One TS packet, afc=01 (payload only), 184-byte payload starting at `pay`. */
static void build_packet(uint8_t *pkt, uint16_t pid, int pusi, uint8_t cc,
			 const uint8_t *pay)
{
	pkt[0] = 0x47;
	pkt[1] = (uint8_t)((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
	pkt[2] = (uint8_t)(pid & 0xFF);
	pkt[3] = (uint8_t)(0x10 | (cc & 0x0F));
	memcpy(pkt + 4, pay, 184);
}

/* One video access unit: PES header + jxes box + SOC + junk. */
static void build_video_payload(uint8_t *pay, int with_box, uint8_t prim,
				uint8_t trans, uint8_t mat, uint8_t full_range)
{
	size_t o = 0;
	pay[o++] = 0x00;
	pay[o++] = 0x00;
	pay[o++] = 0x01;
	pay[o++] = 0xBD; /* PES stream_id */
	pay[o++] = 0x00;
	pay[o++] = 0x00;
	pay[o++] = 0x81;
	pay[o++] = 0x80; /* PTS_DTS_flags = PTS only */
	pay[o++] = 0x05; /* PES_header_data_length */
	pay[o++] = 0x21;
	pay[o++] = 0x00;
	pay[o++] = 0x01;
	pay[o++] = 0x00;
	pay[o++] = 0x01; /* PTS */
	if (with_box) {
		pay[o + 0] = 0;
		pay[o + 1] = 0;
		pay[o + 2] = 0;
		pay[o + 3] = 30;
		pay[o + 4] = 'j';
		pay[o + 5] = 'x';
		pay[o + 6] = 'e';
		pay[o + 7] = 's';
		pay[o + 22] = prim;
		pay[o + 23] = trans;
		pay[o + 24] = mat;
		pay[o + 25] = (uint8_t)(full_range ? 0x80 : 0x00);
		o += 30;
	}
	pay[o++] = 0xFF; /* JPEG XS SOC */
	pay[o++] = 0x10;
	while (o < 184)
		pay[o++] = 0x55;
}

/* PMT sign and box sign are separate so the precedence (box wins) can be exercised. */
struct run_case {
	const char *label;
	int with_pmt;
	int with_box;
	uint8_t pmt_full;
	uint8_t box_full;
	int expect_present;
	int expect_full;     /* -2 = don't check */
	int expect_pmt_full; /* -2 = don't check */
	const char *expect_source;
};

static const char *name_of(int r)
{
	return r == JXS_RANGE_FULL ? "FULL"
	     : r == JXS_RANGE_LIMITED ? "limited"
	     : "unknown";
}

static void run(const struct run_case *c)
{
	uint8_t sec[64];
	uint8_t pkt[188];
	uint8_t pay[184];
	uint8_t pmtpay[184];
	jxs_range_probe_t *p = jxs_range_probe_new();

	/* The PMT sitting on PMT_PID 1000 and the video on PID 65 are the probe's own defaults, so a
	 * descriptor on either is read without any PAT - and the test cannot accidentally "pass" by
	 * relying on the video PID being supplied some other way. */
	if (c->with_pmt) {
		memset(pmtpay, 0xFF, sizeof pmtpay);
		pmtpay[0] = 0x00; /* pointer_field */
		size_t seclen = build_pmt(sec, 1, 1, 1, c->pmt_full);
		memcpy(pmtpay + 1, sec, seclen);
		build_packet(pkt, PMT_PID, 1, 0, pmtpay);
		jxs_range_probe_feed(p, pkt, 188);
	}

	for (uint8_t i = 0; i < 2; i++) {
		build_video_payload(pay, c->with_box, 1, 1, 1, c->box_full);
		build_packet(pkt, VIDEO_PID, 1, i, pay);
		jxs_range_probe_feed(p, pkt, 188);
	}

	const int present = jxs_range_probe_present(p);
	const int full = jxs_range_probe_full_range(p);
	const int pmt_full = jxs_range_probe_pmt_full_range(p);
	const char *source = jxs_range_probe_source(p);

	char line[220];
	snprintf(line, sizeof line, "%s: present=%d range=%s pmt=%s source=%s",
		 c->label, present, name_of(full), name_of(pmt_full), source);
	check(present == c->expect_present, line);
	if (c->expect_present && c->expect_full != -2)
		check(full == (c->expect_full ? JXS_RANGE_FULL : JXS_RANGE_LIMITED),
		      c->expect_full ? "  reported range is FULL (the signed value)"
				     : "  reported range is limited (the signed value)");
	if (c->expect_pmt_full != -2) {
		int want = (c->expect_pmt_full == JXS_RANGE_UNKNOWN)
				   ? JXS_RANGE_UNKNOWN
				   : (c->expect_pmt_full ? JXS_RANGE_FULL
							 : JXS_RANGE_LIMITED);
		check(pmt_full == want, "  PMT carriage point read the PMT's own value");
	}
	if (c->expect_source)
		check(strcmp(source, c->expect_source) == 0, "  source names the carriage point");

	jxs_range_probe_free(p);
}

int main(void)
{
	const int FULL = JXS_RANGE_FULL, LIM = JXS_RANGE_LIMITED,
		  UNK = JXS_RANGE_UNKNOWN;
	struct run_case cases[] = {
		/* Both carriage points present and agreeing. */
		{"PMT full, box full", 1, 1, 1, 1, 1, FULL, FULL,
		 "PMT JXS_video_descriptor"},
		{"PMT limited, box limited", 1, 1, 0, 0, 1, LIM, LIM,
		 "PMT JXS_video_descriptor"},
		/* The PMT alone (no per-frame box). */
		{"PMT full, box absent", 1, 0, 1, 0, 1, FULL, FULL,
		 "PMT JXS_video_descriptor"},
		{"PMT limited, box absent", 1, 0, 0, 0, 1, LIM, LIM,
		 "PMT JXS_video_descriptor"},
		/* Precedence, and the box ALONE (no PMT at all). */
		{"PMT full but box says limited", 1, 1, 1, 0, 1, LIM, FULL, "jxes box"},
		{"PMT limited but box says full", 1, 1, 0, 1, 1, FULL, LIM, "jxes box"},
		{"no PMT at all, box says full", 0, 1, 0, 1, 1, FULL, UNK, "jxes box"},
		/* Nothing signed anywhere: nothing may be invented. */
		{"nothing signed", 0, 0, 0, 0, 0, UNK, UNK, "none"},
	};
	const size_t n = sizeof cases / sizeof cases[0];

	printf("range read through the recorder's probe (both carriage points):\n");
	for (size_t i = 0; i < n; i++) {
		printf("%s\n", cases[i].label);
		run(&cases[i]);
	}

	printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
	return fails ? 1 : 0;
}
