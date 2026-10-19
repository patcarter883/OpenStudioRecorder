/*
 * jxs_ts.h - minimal MPEG-TS demux for the JPEG XS camera stream.
 *
 * OBS-independent on purpose: the plugin and the standalone decode harness
 * (tools/jxs_decode_ts.c) both compile this file, so what the harness verifies
 * is the same demux code that ships.
 *
 * Wire format (measured, see the build brief): datagrams are 1316 B = 7 x
 * 188-byte TS packets starting at the sync byte 0x47. PIDs PAT 0, PMT 1000,
 * video 65. A PUSI packet starts a frame; the PES payload is
 *   PES header (stream_id 0xBD, PTS) | 30-byte 'jxes' box | JPEG XS codestream
 * The codestream starts at the SOC marker 0xFF10. Continuity-counter gaps drop
 * the affected frame without desyncing the parser.
 *
 * Audio (optional second ES, see the audio plan): stream_type 0x06 with a
 * registration descriptor 'BSSD' (SMPTE ST 302M, AES3-packed PCM), PES 0xBD.
 * A node without audio simply has no such ES and nothing here runs.
 */
#ifndef JXS_TS_H
#define JXS_TS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "s302m.h"

#define JXS_TS_PKT 188

/*
 * The stream's colour declaration, as the camera node signs it in TWO places
 * (see the camera-node colour contract, docs/camera-node-colour-contract.md):
 *
 *   - the PMT's JXS_video_descriptor (extension descriptor 0x3F/0x14), and
 *   - the 30-byte 'jxes' box on the video PID, byte-for-byte the same four
 *     values at box bytes 22..25.
 *
 * Values are the H.273 / CICP codes the node signs (BT.709 -> 1/1/1) plus the
 * range flag: full_range is 0 = limited, 1 = full. `present` is false when
 * NEITHER carriage point was seen - a consumer must not invent a range from an
 * absent declaration; it says so and falls back explicitly.
 */
typedef struct {
	bool present;
	uint8_t primaries; /* H.273 colour_primaries        */
	uint8_t transfer;  /* H.273 transfer_characteristics */
	uint8_t matrix;    /* H.273 matrix_coefficients     */
	uint8_t full_range; /* 0 = limited (16-235), 1 = full (0-255) */
} jxs_ts_colour_t;

typedef void (*jxs_ts_frame_cb)(void *user, const uint8_t *cs, size_t len,
				uint64_t pts, const jxs_ts_colour_t *colour);

/* One complete ST 302M frame. payload points at the bytes AFTER the 4-byte frame
 * header and len is the header's payload length; the parsed header is passed along so
 * the consumer does not have to re-scan bytes it no longer holds. pts is the PES PTS
 * in 90 kHz units (UINT64_MAX if the PES carried none). */
typedef void (*jxs_ts_audio_cb)(void *user, const s302m_frame_info_t *info,
				const uint8_t *payload, size_t len, uint64_t pts);

typedef struct {
	/* configuration */
	uint16_t pmt_pid;
	uint16_t video_pid;
	uint16_t audio_pid;
	uint8_t *asm_buf; /* caller-owned reassembly buffer, >= asm_cap */
	size_t asm_cap;
	jxs_ts_frame_cb on_frame;
	void *user;
	uint8_t *audio_buf; /* caller-owned audio reassembly, >= audio_cap */
	size_t audio_cap;
	jxs_ts_audio_cb on_audio;
	void *audio_user;

	/* video state (private) */
	bool have_pmt;
	bool have_cc;
	int last_cc;
	bool asm_valid;
	bool asm_corrupt;
	size_t asm_len;
	uint64_t asm_pts;
	/* Colour declaration. The PMT descriptor (pmt_colour) is the fallback;
	 * the per-frame jxes box overrides it into asm_colour, which is what is
	 * handed to on_frame() with that frame. Video thread only - same thread
	 * that calls on_frame(), so no atomicity needed. */
	jxs_ts_colour_t pmt_colour;
	jxs_ts_colour_t asm_colour;

	/* audio state (private) */
	bool have_audio;
	bool have_audio_cc;
	int last_audio_cc;
	size_t audio_len;
	uint64_t audio_pts;

	/* counters */
	/*
	 * Statistics counters are written by the rx thread (and the audio thread)
	 * but read by the decode thread's stats line. Plain uint64_t there is a data
	 * race with torn reads: benign-looking, but UB, and it means the very numbers
	 * used to diagnose the link can lie. C11 _Atomic keeps every existing `++`
	 * site and printf read working unchanged (an lvalue conversion on an atomic
	 * integer is just a load), at the cost of one lock-free RMW per event.
	 * The containing struct is bzalloc'd, which zeroes all-bits-zero for these
	 * lock-free atomic integer types.
	 */
	_Atomic uint64_t n_datagrams;
	_Atomic uint64_t n_frames; /* complete codestreams emitted */
	_Atomic uint64_t n_lost;   /* frames discarded because packets were lost */
	_Atomic uint64_t n_desync; /* PES/codestream framing errors */
	_Atomic uint64_t n_malformed;
	_Atomic uint64_t n_audio_frames; /* complete 302M frames emitted */
	_Atomic uint64_t n_audio_lost;	 /* dropped: CC gap or malformed frame */
	_Atomic uint64_t n_audio_desync; /* dropped: PES/frame-header framing errors */
} jxs_ts_demux_t;

void jxs_ts_demux_init(jxs_ts_demux_t *d, uint8_t *asm_buf, size_t asm_cap,
		       uint16_t pmt_pid, uint16_t video_pid,
		       jxs_ts_frame_cb cb, void *user);
/* Enable the audio path: the caller supplies its own reassembly buffer. */
void jxs_ts_demux_set_audio(jxs_ts_demux_t *d, uint8_t *buf, size_t cap,
			    jxs_ts_audio_cb cb, void *user);
void jxs_ts_demux_feed(jxs_ts_demux_t *d, const uint8_t *data, size_t len);
void jxs_ts_demux_flush(jxs_ts_demux_t *d);

#endif /* JXS_TS_H */
