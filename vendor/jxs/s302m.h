/*
 * s302m.h - SMPTE ST 302M (AES3-packed PCM) framing, shared by the OBS plugin and
 * the standalone decode harness.
 *
 * The wire law is ffmpeg's libavcodec/s302menc.c, implemented here in the inverse
 * direction for decoding. Per frame: a 4-byte big-endian header
 *   [16-bit payload length][2-bit (channels/2 - 1)][8-bit channel id][2-bit bps]
 *   [4-bit alignment]
 * followed by pairs of samples, every emitted byte bit-reversed (AES3 is LSB-first).
 * For 24-bit stereo a pair is 7 bytes: three full bytes for sample 0, then sample 1
 * split across a 4-bit nibble plus three 8-bit groups, with a VUCF marker (0x10,
 * already in the reversed domain) on the first pair of each 192-pair block.
 *
 * Decoding is ungated everywhere (only encoding is compliance-gated in ffmpeg and
 * in this gst-libav build), so this file is the only "decoder" the plugin needs:
 * no library, no new dependency.
 */
#ifndef S302M_H
#define S302M_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S302M_HEADER_BYTES 4
#define S302M_SAMPLES_PER_FRAME 1024

/* The VUCF framing bit as it appears in the on-wire byte (already bit-reversed). */
#define S302M_VUCF_BIT 0x10

typedef struct {
	unsigned channels;	/* 2/4/6/8 - spec requires even */
	unsigned bits;		/* 16, 20 or 24 */
	size_t pair_bytes;	/* bytes per interleaved sample pair */
	size_t payload_bytes;	/* payload length from the header */
	unsigned samples_per_ch;/* payload_bytes / pair_bytes */
} s302m_frame_info_t;

/* The VUCF framing marker sits on every 192nd PAIR OF THE STREAM, not every 192nd pair
 * of each frame - measured on ffmpeg's own output, whose 1024-pair frames put the
 * markers at 0/192/384/576/768/960 and then 128/320/512/704/896 on the next frame, the
 * phase cycling every three. So decoding needs a running pair position, which is what
 * this carries. It starts at zero for the first frame handed over: a receiver that
 * joins mid-stream cannot know the true phase, and the cost of a wrong phase is one
 * bit on six pairs per frame, so it is counted rather than guessed at. */
typedef struct {
	uint64_t pair_index;   /* running position; the marker sits where this % 192 == 0 */
	uint64_t vucf;	       /* markers seen */
	uint64_t vucf_missing; /* block boundaries with no marker set: a phase warning */
} s302m_unpack_state_t;

/* Parse a 4-byte frame header. Returns false for anything implausible: short input,
 * an odd/zero channel count, an unknown bit-depth code, a payload that is not a whole
 * number of pairs, or a payload larger than n (a frame cannot exceed what we hold). */
bool s302m_parse_header(const uint8_t *hdr, size_t n, s302m_frame_info_t *info);

/* Unpack one frame payload (the bytes after the header) into interleaved int32
 * samples, each 24-bit sample left-aligned in 32 bits exactly as AES3 carries it
 * (sample << 8). out must hold samples_per_ch * channels entries, and state must
 * persist across calls - see its comment for why. */
bool s302m_unpack(const uint8_t *payload, size_t n,
		  const s302m_frame_info_t *info, int32_t *out,
		  s302m_unpack_state_t *state);

#endif /* S302M_H */
