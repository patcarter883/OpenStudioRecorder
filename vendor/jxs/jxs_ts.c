/*
 * jxs_ts.c - the demux half of obs-jxs-camera, shared with the test harness.
 * See jxs_ts.h for the contract and the measured wire format.
 */
#include "jxs_ts.h"
#include "s302m.h"

#include <string.h>

#define TS_PAYLOAD 184
#define PID_PAT 0x0000
#define PID_NULL 0x1FFF
#define STREAM_TYPE_JPEGXS 0x32
#define STREAM_TYPE_PRIVATE 0x06
#define DESCRIPTOR_REGISTRATION 0x05
/* JXS_video_descriptor: an MPEG-2 "extension" descriptor (tag 0x3F) whose
 * extension tag is 0x14, length 30. The colour declaration is the last four of
 * its 29 payload bytes: primaries/transfer/matrix at descriptor payload offsets
 * 25/26/27 and video_full_range in bit 7 of offset 28. Same four values, same
 * order, at jxes-box bytes 22..25 (ts_mux.c builds both from one struct). */
#define DESCRIPTOR_EXTENSION 0x3F
#define DESCRIPTOR_JXS_VIDEO 0x14
#define JXS_VIDEO_DESC_LEN 30
#define JXS_COLOUR_DESC_PRIMARIES_OFF 27 /* from the 0x3F tag, inclusive */
#define JXS_COLOUR_DESC_RANGE_OFF 30
#define JXS_COLOUR_BOX_PRIMARIES_OFF 22 /* from the box start */

static bool is_jxes_box(const uint8_t *p, size_t n, uint32_t *box_size)
{
	if (n < 8)
		return false;
	if (p[4] != 0x6a || p[5] != 0x78 || p[6] != 0x65 || p[7] != 0x73)
		return false; /* 'jxes' */
	*box_size = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	return *box_size >= 8;
}

/* Parse a PES header. Sets *pts (UINT64_MAX if absent) and *header_len. */
static bool parse_pes_header(const uint8_t *p, size_t n, uint64_t *pts,
			     size_t *header_len)
{
	if (n < 14)
		return false;
	if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01)
		return false;
	*pts = UINT64_MAX;
	if (p[7] & 0x80) {
		const uint8_t *q = p + 9;
		*pts = ((uint64_t)((q[0] >> 1) & 0x07) << 30) |
		       ((uint64_t)q[1] << 22) |
		       ((uint64_t)((q[2] >> 1) & 0x7f) << 15) |
		       ((uint64_t)q[3] << 7) | ((uint64_t)((q[4] >> 1) & 0x7f));
	}
	*header_len = 9 + (size_t)p[8];
	return *header_len <= n;
}

static bool parse_pat(const uint8_t *sec, size_t n, uint16_t *pmt_pid)
{
	if (n < 12 || sec[0] != 0x00)
		return false;
	size_t len = ((size_t)(sec[1] & 0x0F) << 8) | sec[2];
	size_t end = 3 + len;
	if (end > n)
		end = n;
	size_t o = 8; /* section hdr(3) tsid(2) ver(1) sec_num(1) last(1) */
	while (o + 4 <= end) {
		uint16_t prog = ((uint16_t)sec[o] << 8) | sec[o + 1];
		uint16_t pid = ((uint16_t)(sec[o + 2] & 0x1F) << 8) | sec[o + 3];
		if (prog != 0) {
			*pmt_pid = pid;
			return true;
		}
		o += 4;
	}
	return false;
}

/* Walk one ES's descriptor list looking for a registration descriptor whose value is
 * exactly the four bytes at reg (e.g. 'BSSD' for SMPTE ST 302M). */
static bool es_has_registration(const uint8_t *descs, size_t n,
				const char reg[4])
{
	size_t o = 0;
	while (o + 2 <= n) {
		uint8_t tag = descs[o];
		size_t dlen = descs[o + 1];
		if (o + 2 + dlen > n)
			return false;
		if (tag == DESCRIPTOR_REGISTRATION && dlen == 4 &&
		    descs[o + 2] == (uint8_t)reg[0] &&
		    descs[o + 3] == (uint8_t)reg[1] &&
		    descs[o + 4] == (uint8_t)reg[2] &&
		    descs[o + 5] == (uint8_t)reg[3])
			return true;
		o += 2 + dlen;
	}
	return false;
}

/* Walk one ES's descriptor list looking for the JXS_video_descriptor - the
 * extension descriptor 0x3F whose extension tag is 0x14 - and read the colour
 * declaration out of it. This is the fallback: the per-frame jxes box overrides
 * it, but a node that only ever repeats the PMT still gets its range read. */
static bool es_parse_jxs_colour(const uint8_t *descs, size_t n,
				jxs_ts_colour_t *c)
{
	size_t o = 0;
	while (o + 2 <= n) {
		const uint8_t tag = descs[o];
		const size_t dlen = descs[o + 1];
		if (o + 2 + dlen > n)
			return false;
		if (tag == DESCRIPTOR_EXTENSION && dlen >= JXS_VIDEO_DESC_LEN &&
		    descs[o + 2] == DESCRIPTOR_JXS_VIDEO) {
			c->primaries = descs[o + JXS_COLOUR_DESC_PRIMARIES_OFF];
			c->transfer =
				descs[o + JXS_COLOUR_DESC_PRIMARIES_OFF + 1];
			c->matrix = descs[o + JXS_COLOUR_DESC_PRIMARIES_OFF + 2];
			c->full_range =
				(descs[o + JXS_COLOUR_DESC_RANGE_OFF] & 0x80)
					? 1
					: 0;
			c->present = true;
			return true;
		}
		o += 2 + dlen;
	}
	return false;
}

/* Find BOTH elementary streams: the JPEG XS video (0x32) and, if present, the ST 302M
 * audio - stream_type 0x06 carrying a 'BSSD' registration descriptor, which is how
 * tsdemux identifies it too (and how mpegtsmux writes it). Neither is required: a
 * video-only node sets no audio PID, an audio-only PMT sets no video PID. Scan the
 * whole list rather than returning on the first match. The video ES's descriptor list
 * is also scanned for the JXS_video_descriptor, whose colour declaration *colour
 * receives (the PMT fallback for the per-frame jxes box). */
static bool parse_pmt(const uint8_t *sec, size_t n, uint16_t *video_pid,
		      uint16_t *audio_pid, jxs_ts_colour_t *colour)
{
	if (n < 16 || sec[0] != 0x02)
		return false;
	size_t len = ((size_t)(sec[1] & 0x0F) << 8) | sec[2];
	size_t end = 3 + len;
	if (end > n)
		end = n;
	uint16_t pil = ((uint16_t)(sec[10] & 0x0F) << 8) | sec[11];
	size_t o = 12 + pil;
	bool found = false;
	while (o + 5 <= end) {
		uint8_t stream_type = sec[o];
		uint16_t pid = ((uint16_t)(sec[o + 1] & 0x1F) << 8) | sec[o + 2];
		uint16_t esl = ((uint16_t)(sec[o + 3] & 0x0F) << 8) | sec[o + 4];
		if (o + 5 + esl > end)
			break;
		if (stream_type == STREAM_TYPE_JPEGXS && *video_pid == 0) {
			*video_pid = pid;
			found = true;
			es_parse_jxs_colour(sec + o + 5, esl, colour);
		} else if (stream_type == STREAM_TYPE_PRIVATE &&
			   es_has_registration(sec + o + 5, esl, "BSSD") &&
			   *audio_pid == 0) {
			*audio_pid = pid;
			found = true;
		}
		o += 5 + esl;
	}
	return found;
}

static void emit_frame(jxs_ts_demux_t *d)
{
	if (!d->asm_valid)
		return;
	if (!d->asm_corrupt && d->asm_len > 0) {
		d->n_frames++;
		if (d->on_frame)
			d->on_frame(d->user, d->asm_buf, d->asm_len, d->asm_pts,
				    &d->asm_colour);
	}
	d->asm_valid = false;
	d->asm_corrupt = false;
	d->asm_len = 0;
}

static void feed_video(jxs_ts_demux_t *d, uint8_t cc, bool pusi,
		       const uint8_t *pay, size_t paylen)
{
	if (d->have_cc) {
		int step = (cc - d->last_cc) & 0x0F;
		if (step == 0) {
			return; /* duplicate of the previous video packet */
		} else if (step > 1) {
			if (d->asm_valid) {
				/* lost the frame in flight - drop it, don't desync */
				d->n_lost++;
				d->asm_valid = false;
				d->asm_corrupt = true;
			}
		}
	}
	d->last_cc = cc;
	d->have_cc = true;

	if (pusi) {
		emit_frame(d);

		uint64_t pts = UINT64_MAX;
		size_t hlen = 0;
		if (!parse_pes_header(pay, paylen, &pts, &hlen)) {
			d->n_desync++;
			d->asm_valid = false;
			return;
		}
		size_t o = hlen;
		uint32_t bsz = 0;
		const bool have_box = is_jxes_box(pay + o, paylen - o, &bsz);
		if (have_box) {
			if (o + bsz > paylen) {
				d->n_desync++;
				d->asm_valid = false;
				return;
			}
			/* The jxes box carries that frame's colour
			 * declaration at bytes 22..25 - the same four values
			 * the PMT's JXS_video_descriptor signs. It is
			 * per-frame, so it is authoritative and overrides the
			 * PMT fallback. */
			if (bsz >= JXS_COLOUR_BOX_PRIMARIES_OFF + 4) {
				d->asm_colour.present = true;
				d->asm_colour.primaries =
					pay[o + JXS_COLOUR_BOX_PRIMARIES_OFF];
				d->asm_colour.transfer =
					pay[o + JXS_COLOUR_BOX_PRIMARIES_OFF + 1];
				d->asm_colour.matrix =
					pay[o + JXS_COLOUR_BOX_PRIMARIES_OFF + 2];
				d->asm_colour.full_range =
					(pay[o + JXS_COLOUR_BOX_PRIMARIES_OFF + 3] &
					 0x80)
						? 1
						: 0;
			} else {
				d->asm_colour = d->pmt_colour;
			}
			o += bsz;
		} else {
			/* No jxes box on this access unit: fall back to the
			 * colour declaration signed in the PMT descriptor. */
			d->asm_colour = d->pmt_colour;
		}
		if (o + 2 >= paylen || pay[o] != 0xFF || pay[o + 1] != 0x10) {
			d->n_desync++; /* not a JPEG XS SOC */
			d->asm_valid = false;
			return;
		}
		size_t n = paylen - o;
		if (n > d->asm_cap) {
			d->asm_valid = false;
			d->asm_corrupt = true;
			return;
		}
		d->asm_pts = pts;
		d->asm_len = n;
		d->asm_valid = true;
		d->asm_corrupt = false;
		memcpy(d->asm_buf, pay + o, n);
		return;
	}

	if (d->asm_valid && !d->asm_corrupt) {
		if (d->asm_len + paylen > d->asm_cap) {
			d->asm_valid = false;
			d->asm_corrupt = true;
			return;
		}
		memcpy(d->asm_buf + d->asm_len, pay, paylen);
		d->asm_len += paylen;
	}
}

/* Emit every whole 302M frame the buffer now holds. The 4-byte frame header bounds each
 * frame; a PES may carry several, and a frame may span TS packets. A frame that claims
 * more than the buffer can hold is malformed: drop the pending bytes and count, which
 * resyncs at the next PES boundary. */
static void drain_audio(jxs_ts_demux_t *d)
{
	while (d->audio_len >= S302M_HEADER_BYTES) {
		s302m_frame_info_t info;
		if (!s302m_parse_header(d->audio_buf, d->audio_len, &info)) {
			d->n_audio_desync++;
			d->audio_len = 0;
			return;
		}
		size_t need = S302M_HEADER_BYTES + info.payload_bytes;
		if (need > d->audio_cap) {
			d->n_audio_lost++;
			d->audio_len = 0;
			return;
		}
		if (d->audio_len < need)
			return; /* wait for the rest of this frame */
		d->n_audio_frames++;
		if (d->on_audio)
			d->on_audio(d->audio_user, &info,
				    d->audio_buf + S302M_HEADER_BYTES,
				    info.payload_bytes, d->audio_pts);
		d->audio_len -= need;
		if (d->audio_len)
			memmove(d->audio_buf, d->audio_buf + need, d->audio_len);
	}
}

static void feed_audio(jxs_ts_demux_t *d, uint8_t cc, bool pusi,
		       const uint8_t *pay, size_t paylen)
{
	if (d->have_audio_cc) {
		int step = (cc - d->last_audio_cc) & 0x0F;
		if (step == 0) {
			return; /* duplicate packet */
		} else if (step > 1) {
			if (d->audio_len) {
				d->n_audio_lost++;
				d->audio_len = 0;
			}
		}
	}
	d->last_audio_cc = cc;
	d->have_audio_cc = true;

	if (pusi) {
		uint64_t pts = UINT64_MAX;
		size_t hlen = 0;
		if (!parse_pes_header(pay, paylen, &pts, &hlen)) {
			d->n_audio_desync++;
			return;
		}
		d->audio_pts = pts;
		/* A 302M frame routinely spans PES packets, so the buffer persists across
		 * them: the frame boundary is defined by the 302M header alone. (It is not
		 * dropped here - that was a misreading of "frames spanning packets" and it
		 * discarded most of the stream.) */
		pay += hlen;
		paylen -= hlen;
		/* Bound the append by PES_packet_length: the bytes after it, up to the end
		 * of the TS packet, are link stuffing, and letting them in desyncs the
		 * frame parser at every PES. Zero means unbounded (until the next PUSI). */
		{
			size_t plen = ((size_t)pay[0 - hlen + 4] << 8) |
				      pay[0 - hlen + 5];
			if (plen && 6 + plen < hlen + paylen)
				paylen = 6 + plen - hlen;
		}
	}

	if (paylen > d->audio_cap - d->audio_len) {
		d->n_audio_lost++;
		d->audio_len = 0;
		return;
	}
	memcpy(d->audio_buf + d->audio_len, pay, paylen);
	d->audio_len += paylen;
	drain_audio(d);
}

void jxs_ts_demux_init(jxs_ts_demux_t *d, uint8_t *asm_buf, size_t asm_cap,
		       uint16_t pmt_pid, uint16_t video_pid,
		       jxs_ts_frame_cb cb, void *user)
{
	memset(d, 0, sizeof *d);
	d->pmt_pid = pmt_pid;
	d->video_pid = video_pid;
	d->asm_buf = asm_buf;
	d->asm_cap = asm_cap;
	d->on_frame = cb;
	d->user = user;
	d->last_cc = -1;
	d->last_audio_cc = -1;
}

void jxs_ts_demux_set_audio(jxs_ts_demux_t *d, uint8_t *buf, size_t cap,
			    jxs_ts_audio_cb cb, void *user)
{
	d->audio_buf = buf;
	d->audio_cap = cap;
	d->on_audio = cb;
	d->audio_user = user;
}

void jxs_ts_demux_flush(jxs_ts_demux_t *d)
{
	d->asm_valid = false;
	d->asm_corrupt = false;
	d->asm_len = 0;
	if (d->audio_len) {
		d->n_audio_lost++;
		d->audio_len = 0;
	}
}

void jxs_ts_demux_feed(jxs_ts_demux_t *d, const uint8_t *data, size_t len)
{
	d->n_datagrams++;
	if (len == 0 || len % JXS_TS_PKT != 0 || data[0] != 0x47) {
		d->n_malformed++;
		return;
	}
	for (size_t off = 0; off + JXS_TS_PKT <= len; off += JXS_TS_PKT) {
		const uint8_t *q = data + off;
		if (q[0] != 0x47) {
			d->n_malformed++;
			break;
		}
		uint16_t pid = (uint16_t)(((q[1] & 0x1F) << 8) | q[2]);
		bool pusi = (q[1] & 0x40) != 0;
		uint8_t afc = (q[3] >> 4) & 0x03;
		if (pid == PID_NULL)
			continue;

		size_t paylen;
		const uint8_t *pay;
		if (afc & 0x02) {
			uint8_t afl = q[4];
			if (afl > TS_PAYLOAD - 1) {
				d->n_malformed++;
				continue;
			}
			/* 4 hdr + 1 AFL byte + afl AF bytes, then payload */
			pay = q + 5 + afl;
			paylen = (TS_PAYLOAD - 1) - afl;
		} else {
			pay = q + 4;
			paylen = TS_PAYLOAD;
		}
		if (!(afc & 0x01))
			continue; /* no payload (e.g. a PCR-only packet) */

		if (pid == PID_PAT) {
			if (paylen < 4)
				continue;
			uint16_t p;
			if (pay[0] < paylen &&
			    parse_pat(pay + 1 + pay[0], paylen - 1 - pay[0], &p))
				d->pmt_pid = p;
		} else if (pid == d->pmt_pid) {
			if (paylen < 4)
				continue;
			uint16_t v = 0, a = 0;
			if (pay[0] < paylen &&
			    parse_pmt(pay + 1 + pay[0], paylen - 1 - pay[0], &v,
				      &a, &d->pmt_colour)) {
				if (v)
					d->video_pid = v;
				if (a) {
					d->audio_pid = a;
					d->have_audio = true;
				}
				d->have_pmt = true;
			}
		} else if (pid == d->video_pid && d->video_pid) {
			feed_video(d, (uint8_t)(q[3] & 0x0F), pusi, pay, paylen);
		} else if (d->audio_pid && pid == d->audio_pid && d->audio_buf) {
			feed_audio(d, (uint8_t)(q[3] & 0x0F), pusi, pay, paylen);
		}
	}
}
